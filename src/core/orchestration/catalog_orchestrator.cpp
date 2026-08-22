#include "catalog_orchestrator.h"

#include <exception>
#include <limits>
#include <utility>

#include <QFileInfo>
#include <QFutureWatcher>
#include <QMetaType>
#include <QtConcurrentRun>

#include "catalog_database.h"
#include "catalog_develop_repository.h"
#include "catalog_folder_importer.h"
#include "catalog_photo_repository.h"
#include "log.h"
#include "source_binding.h"

namespace flexraw::core::orchestration
{
namespace
{

using OptionalRequestResult = types::Result<std::optional<types::RequestId>, types::CoreError>;
using OptionalPhotoIdResult = types::Result<std::optional<types::PhotoId>, types::CoreError>;

// 목적: catalog-session 선행조건 실패를 구조화된 error로 생성
// 입력: message: caller에게 전달할 technical 설명
// 출력: Conflict로 분류된 CoreError
[[nodiscard]] types::CoreError makeSessionError(QString message)
{
    return {types::ErrorCode::Conflict, std::move(message)};
}

// 목적: 예상하지 못한 fingerprint worker 예외를 구조화된 error로 변환
// 입력: message: exception 세부 설명
// 출력: Unknown으로 분류된 CoreError
[[nodiscard]] types::CoreError makeUnhandledError(QString message)
{
    return {types::ErrorCode::Unknown, std::move(message)};
}

// 목적: file metadata 오류를 persisted source binding state로 변환
// 입력: error: metadata inspection 오류
// 출력: missing·unreadable state 또는 변환할 수 없으면 빈 값
[[nodiscard]] std::optional<catalog::SourceBindingState> sourceStateForError(const types::CoreError& error)
{
    if (error.code == types::ErrorCode::NotFound)
    {
        return catalog::SourceBindingState::Missing;
    }
    if (error.code == types::ErrorCode::PermissionDenied)
    {
        return catalog::SourceBindingState::Unreadable;
    }
    return std::nullopt;
}

// 목적: 사용자 source resolution command가 허용되는 persisted state인지 확인
// 입력: state: 현재 source binding state
// 출력: replacement 수용 또는 신규 등록이 가능하면 true
[[nodiscard]] bool isReplacementResolutionState(catalog::SourceBindingState state) noexcept
{
    return state == catalog::SourceBindingState::IdentityUnverified ||
           state == catalog::SourceBindingState::ReplacementDetected;
}

// 목적: relink command가 허용되는 persisted state인지 확인
// 입력: state: 현재 source binding state
// 출력: source resolution이 필요한 state면 true
[[nodiscard]] bool isRelinkResolutionState(catalog::SourceBindingState state) noexcept
{
    return catalog::requiresSourceResolution(state);
}

// 목적: existing photo record와 현재 locator로 신규 photo 등록 entry 생성
// 입력: record: 표시 metadata 소유 record, locator: 신규 source 위치
// 출력: repository가 저장할 Ready/Pending CatalogEntry
[[nodiscard]] catalog::CatalogEntry makeReplacementEntry(const catalog::CatalogPhotoRecord& record,
                                                         const types::SourceLocator& locator)
{
    const QFileInfo fileInfo(locator.path);
    return {
        types::FileDescriptor{
            locator.path,
            fileInfo.suffix().isEmpty() ? record.extension : fileInfo.suffix().toLower(),
            fileInfo.fileName().isEmpty() ? record.displayName : fileInfo.fileName(),
            record.kind,
        },
        types::FileScanStatus::Ready,
    };
}

}  // namespace

// 목적: catalog-session state와 직렬 background fingerprint pool 생성
// 입력: parent: Qt 부모 object
// 출력: catalog가 열리지 않은 CatalogOrchestrator 객체
CatalogOrchestrator::CatalogOrchestrator(QObject* parent) : QObject(parent)
{
    qRegisterMetaType<CatalogPhotoState>();
    qRegisterMetaType<CatalogSourceUpdate>();
    qRegisterMetaType<CatalogIssue>();
    m_fingerprintPool.setObjectName(QStringLiteral("CatalogFingerprintPool"));
    m_fingerprintPool.setMaxThreadCount(1);
}

// 목적: active fingerprint 작업을 취소하고 catalog connection 정리
// 입력: 없음
// 출력: worker와 database resource가 남지 않음
CatalogOrchestrator::~CatalogOrchestrator()
{
    m_acceptingRequests = false;
    cancelAllSourceRequests(false);
    m_folderImporter.reset();
    m_developRepository.reset();
    m_photoRepository.reset();
    m_database.reset();
}

// 목적: 현재 catalog-session immutable state 반환
// 입력: 없음
// 출력: open 여부와 canonical catalog path
CatalogSessionState CatalogOrchestrator::state() const
{
    return {m_database != nullptr, m_database != nullptr ? m_database->catalogPath() : QString{}};
}

// 목적: catalog database를 열고 migration과 repository session 구성
// 입력: catalogPath: 생성하거나 열 catalog file 경로
// 출력: 열린 session state 또는 validation·migration 오류
CatalogSessionResult CatalogOrchestrator::openCatalog(const QString& catalogPath)
{
    if (m_database != nullptr)
    {
        return CatalogSessionResult::failure(makeSessionError(QStringLiteral("A catalog session is already open.")));
    }

    catalog::CatalogDatabaseOpenResult opened = catalog::CatalogDatabase::open(catalogPath);
    if (opened.hasError())
    {
        return CatalogSessionResult::failure(opened.error());
    }

    m_database = std::move(opened.value());
    m_photoRepository = std::make_unique<catalog::CatalogPhotoRepository>(*m_database);
    m_developRepository = std::make_unique<catalog::CatalogDevelopRepository>(*m_database);
    m_folderImporter = std::make_unique<catalog::CatalogFolderImporter>(*m_database);
    return CatalogSessionResult::success(state());
}

// 목적: active fingerprint 작업을 취소하고 현재 catalog session 종료
// 입력: 없음
// 출력: 닫힌 session state
CatalogSessionState CatalogOrchestrator::closeCatalog()
{
    cancelAllSourceRequests(true);
    m_folderImporter.reset();
    m_developRepository.reset();
    m_photoRepository.reset();
    m_database.reset();
    return state();
}

// 목적: 열린 catalog에서 optional exact-folder scope와 stable cursor 기반 bounded photo page 조회
// 입력: request: page 크기, 방향, optional exclusive cursor와 folder scope
// 출력: 표시 순서의 bounded page 또는 session·validation·database 오류
CatalogPhotoPageResult CatalogOrchestrator::queryPhotos(const catalog::CatalogPhotoPageRequest& request) const
{
    if (const std::optional<types::CoreError> error = requireOpenCatalog(); error.has_value())
    {
        return CatalogPhotoPageResult::failure(*error);
    }

    return m_photoRepository->queryPage(request);
}

// 목적: folder 사진을 catalog에 등록하고 pending fingerprint 작업 예약
// 입력: folderPath: scan할 folder 경로
// 출력: import 수와 identity·background request 목록 또는 오류
CatalogImportResult CatalogOrchestrator::importFolder(const QString& folderPath)
{
    if (const std::optional<types::CoreError> error = requireOpenCatalog(); error.has_value())
    {
        return CatalogImportResult::failure(*error);
    }

    const catalog::CatalogFolderImportResult imported = m_folderImporter->importFolder(folderPath);
    if (imported.hasError())
    {
        return CatalogImportResult::failure(imported.error());
    }

    CatalogImportSummary summary;
    summary.scannedCount = imported.value().entries.size();
    summary.storedCount = imported.value().storedCount;
    summary.photoIds.reserve(summary.scannedCount);
    summary.fingerprintRequestIds.reserve(summary.scannedCount);
    for (const catalog::CatalogEntry& entry : imported.value().entries)
    {
        const catalog::CatalogPhotoRecordResult record = m_photoRepository->findBySourcePath(entry.file.path);
        if (record.hasError())
        {
            return CatalogImportResult::failure(record.error());
        }
        if (!record.value().has_value())
        {
            return CatalogImportResult::failure(
                {types::ErrorCode::NotFound, QStringLiteral("Imported photo identity could not be resolved.")});
        }

        summary.photoIds.push_back(record.value()->id);
        const OptionalRequestResult observed = observeSource(*record.value());
        if (observed.hasError())
        {
            return CatalogImportResult::failure(observed.error());
        }
        if (observed.value().has_value())
        {
            summary.fingerprintRequestIds.push_back(*observed.value());
        }
    }

    return CatalogImportResult::success(std::move(summary));
}

// 목적: worker에서 완료한 folder scan 결과를 열린 catalog에 저장하고 fingerprint 작업 예약
// 입력: entries: UI thread 밖에서 scan·분류가 끝난 지원 photo 목록
// 출력: import 수와 identity·background request 목록 또는 오류
CatalogImportResult CatalogOrchestrator::importScannedEntries(const QVector<catalog::CatalogEntry>& entries)
{
    if (const std::optional<types::CoreError> error = requireOpenCatalog(); error.has_value())
    {
        return CatalogImportResult::failure(*error);
    }

    const catalog::CatalogPhotoStoreResult stored = m_photoRepository->upsert(entries);
    if (stored.hasError())
    {
        return CatalogImportResult::failure(stored.error());
    }

    CatalogImportSummary summary;
    summary.scannedCount = entries.size();
    summary.storedCount = stored.value();
    summary.photoIds.reserve(summary.scannedCount);
    summary.fingerprintRequestIds.reserve(summary.scannedCount);
    for (const catalog::CatalogEntry& entry : entries)
    {
        const catalog::CatalogPhotoRecordResult record = m_photoRepository->findBySourcePath(entry.file.path);
        if (record.hasError())
        {
            return CatalogImportResult::failure(record.error());
        }
        if (!record.value().has_value())
        {
            return CatalogImportResult::failure(
                {types::ErrorCode::NotFound, QStringLiteral("Imported photo identity could not be resolved.")});
        }

        summary.photoIds.push_back(record.value()->id);
        const OptionalRequestResult observed = observeSource(*record.value());
        if (observed.hasError())
        {
            return CatalogImportResult::failure(observed.error());
        }
        if (observed.value().has_value())
        {
            summary.fingerprintRequestIds.push_back(*observed.value());
        }
    }

    return CatalogImportResult::success(std::move(summary));
}

// 목적: Editor activation 대상 photo를 active Catalog에 등록하거나 기존 identity로 resolve
// 입력: entry: folder scan에서 얻은 단일 supported photo
// 출력: stable catalog-local PhotoId 또는 session·database 오류
CatalogPhotoRegistrationResult CatalogOrchestrator::registerPhoto(const catalog::CatalogEntry& entry)
{
    if (const std::optional<types::CoreError> error = requireOpenCatalog(); error.has_value())
    {
        return CatalogPhotoRegistrationResult::failure(*error);
    }

    const catalog::CatalogPhotoStoreResult stored = m_photoRepository->upsert({entry});
    if (stored.hasError())
    {
        return CatalogPhotoRegistrationResult::failure(stored.error());
    }

    const catalog::CatalogPhotoRecordResult record = m_photoRepository->findBySourcePath(entry.file.path);
    if (record.hasError())
    {
        return CatalogPhotoRegistrationResult::failure(record.error());
    }
    if (!record.value().has_value())
    {
        return CatalogPhotoRegistrationResult::failure(
            {types::ErrorCode::NotFound, QStringLiteral("Activated photo identity could not be resolved.")});
    }

    const OptionalRequestResult observed = observeSource(*record.value());
    return observed.hasError() ? CatalogPhotoRegistrationResult::failure(observed.error())
                               : CatalogPhotoRegistrationResult::success(record.value()->id);
}

// 목적: PhotoId의 develop state와 현재 source binding을 resolve
// 입력: photoId: 선택하거나 조회할 catalog-local identity
// 출력: processing 가능 여부와 optional verification request를 포함한 state
CatalogPhotoStateResult CatalogOrchestrator::resolvePhoto(types::PhotoId photoId)
{
    if (const std::optional<types::CoreError> error = requireOpenCatalog(); error.has_value())
    {
        return CatalogPhotoStateResult::failure(*error);
    }

    const catalog::CatalogPhotoRecordResult record = m_photoRepository->findById(photoId);
    if (record.hasError())
    {
        return CatalogPhotoStateResult::failure(record.error());
    }
    if (!record.value().has_value())
    {
        return CatalogPhotoStateResult::failure(
            {types::ErrorCode::NotFound, QStringLiteral("Catalog photo does not exist.")});
    }

    const OptionalRequestResult observed = observeSource(*record.value());
    if (observed.hasError())
    {
        return CatalogPhotoStateResult::failure(observed.error());
    }
    return loadPhotoState(photoId, observed.value());
}

// 목적: caller baseline revision과 일치할 때 PhotoId의 develop state 저장
// 입력: photoId: 저장 대상, params: 새 develop 값, expectedRevision: load 당시 persisted revision
// 출력: 증가한 persisted revision을 포함한 photo state 또는 conflict
CatalogPhotoStateResult CatalogOrchestrator::saveDevelopState(types::PhotoId photoId,
                                                              const types::DevelopParams& params,
                                                              types::DevelopRevision expectedRevision)
{
    if (const std::optional<types::CoreError> error = requireOpenCatalog(); error.has_value())
    {
        return CatalogPhotoStateResult::failure(*error);
    }

    const catalog::CatalogDevelopSaveResult saved = m_developRepository->saveState(photoId, params, expectedRevision);
    return saved.hasError() ? CatalogPhotoStateResult::failure(saved.error()) : loadPhotoState(photoId);
}

// 목적: 교체·identity 미확인 source를 기존 PhotoId의 새 baseline으로 수용
// 입력: photoId: 기존 develop state를 유지할 identity
// 출력: background hash request ID 또는 현재 state 오류
CatalogSourceSubmissionResult CatalogOrchestrator::acceptReplacement(types::PhotoId photoId)
{
    if (const std::optional<types::CoreError> error = requireOpenCatalog(); error.has_value())
    {
        return CatalogSourceSubmissionResult::failure(*error);
    }

    const catalog::CatalogPhotoRecordResult record = m_photoRepository->findById(photoId);
    if (record.hasError())
    {
        return CatalogSourceSubmissionResult::failure(record.error());
    }
    if (!record.value().has_value() || !record.value()->source.has_value())
    {
        return CatalogSourceSubmissionResult::failure(
            {types::ErrorCode::NotFound, QStringLiteral("Replacement source is not linked.")});
    }
    if (!isReplacementResolutionState(record.value()->sourceState))
    {
        return CatalogSourceSubmissionResult::failure(
            makeSessionError(QStringLiteral("Photo source is not waiting for replacement resolution.")));
    }

    return submitFingerprintJob(FingerprintPurpose::AcceptReplacement, photoId, *record.value()->source);
}

// 목적: 교체·identity 미확인 source를 새 PhotoId로 등록
// 입력: photoId: source binding을 해제할 기존 identity
// 출력: background hash request ID 또는 현재 state 오류
CatalogSourceSubmissionResult CatalogOrchestrator::registerReplacementAsNew(types::PhotoId photoId)
{
    if (const std::optional<types::CoreError> error = requireOpenCatalog(); error.has_value())
    {
        return CatalogSourceSubmissionResult::failure(*error);
    }

    const catalog::CatalogPhotoRecordResult record = m_photoRepository->findById(photoId);
    if (record.hasError())
    {
        return CatalogSourceSubmissionResult::failure(record.error());
    }
    if (!record.value().has_value() || !record.value()->source.has_value())
    {
        return CatalogSourceSubmissionResult::failure(
            {types::ErrorCode::NotFound, QStringLiteral("Replacement source is not linked.")});
    }
    if (!isReplacementResolutionState(record.value()->sourceState))
    {
        return CatalogSourceSubmissionResult::failure(
            makeSessionError(QStringLiteral("Photo source is not waiting for replacement resolution.")));
    }

    const catalog::CatalogEntry replacement = makeReplacementEntry(*record.value(), *record.value()->source);
    return submitFingerprintJob(
        FingerprintPurpose::RegisterReplacementAsNew, photoId, *record.value()->source, replacement);
}

// 목적: 기존 PhotoId를 content match가 확인된 다른 locator에 재연결
// 입력: photoId: 유지할 identity, locator: 검증할 새 source 위치
// 출력: background hash request ID 또는 현재 state 오류
CatalogSourceSubmissionResult CatalogOrchestrator::relinkSource(types::PhotoId photoId,
                                                                const types::SourceLocator& locator)
{
    if (const std::optional<types::CoreError> error = requireOpenCatalog(); error.has_value())
    {
        return CatalogSourceSubmissionResult::failure(*error);
    }
    if (locator.path.trimmed().isEmpty())
    {
        return CatalogSourceSubmissionResult::failure(
            {types::ErrorCode::InvalidArgument, QStringLiteral("Relink source path is empty.")});
    }

    const catalog::CatalogPhotoRecordResult record = m_photoRepository->findById(photoId);
    if (record.hasError())
    {
        return CatalogSourceSubmissionResult::failure(record.error());
    }
    if (!record.value().has_value())
    {
        return CatalogSourceSubmissionResult::failure(
            {types::ErrorCode::NotFound, QStringLiteral("Catalog photo does not exist.")});
    }
    if (!isRelinkResolutionState(record.value()->sourceState))
    {
        return CatalogSourceSubmissionResult::failure(
            makeSessionError(QStringLiteral("Photo source is not waiting for relink resolution.")));
    }
    if (!types::hasSourceContentHash(record.value()->fingerprint))
    {
        return CatalogSourceSubmissionResult::failure(
            makeSessionError(QStringLiteral("Relink requires an existing source content fingerprint baseline.")));
    }

    return submitFingerprintJob(FingerprintPurpose::RelinkSource, photoId, locator);
}

// 목적: 지정 fingerprint request의 향후 mutation과 event publish 취소
// 입력: requestId: 취소할 request identity
// 출력: active request를 취소했으면 true
bool CatalogOrchestrator::cancelSourceRequest(types::RequestId requestId)
{
    const auto job = m_activeJobs.find(requestId);
    if (job == m_activeJobs.end())
    {
        return false;
    }

    job.value().cancellationSource.requestCancellation();
    m_activePhotoRequests.remove(job.value().photoId.value);
    m_activeJobs.erase(job);
    emit sourceBindingCancelled(requestId);
    return true;
}

// 목적: 열린 session repository 존재 여부 확인
// 입력: 없음
// 출력: 열려 있으면 빈 error, 아니면 session 오류
std::optional<types::CoreError> CatalogOrchestrator::requireOpenCatalog() const
{
    if (m_database == nullptr || m_photoRepository == nullptr || m_developRepository == nullptr ||
        m_folderImporter == nullptr)
    {
        return makeSessionError(QStringLiteral("No catalog session is open."));
    }
    return std::nullopt;
}

// 목적: photo record와 persisted develop state를 adapter용 snapshot으로 조립
// 입력: photoId: 조회할 identity, requestId: optional active verification request
// 출력: 조립된 state 또는 repository 오류
CatalogPhotoStateResult CatalogOrchestrator::loadPhotoState(types::PhotoId photoId,
                                                            std::optional<types::RequestId> requestId) const
{
    const catalog::CatalogPhotoRecordResult record = m_photoRepository->findById(photoId);
    if (record.hasError())
    {
        return CatalogPhotoStateResult::failure(record.error());
    }
    if (!record.value().has_value())
    {
        return CatalogPhotoStateResult::failure(
            {types::ErrorCode::NotFound, QStringLiteral("Catalog photo does not exist.")});
    }

    const catalog::CatalogDevelopStateResult developState = m_developRepository->loadState(photoId);
    if (developState.hasError())
    {
        return CatalogPhotoStateResult::failure(developState.error());
    }

    const types::DevelopParams params =
        developState.value().has_value() ? developState.value()->params : types::DevelopParams{};
    const types::DevelopRevision revision =
        developState.value().has_value() ? developState.value()->revision : types::DevelopRevision{0};
    return CatalogPhotoStateResult::success({
        *record.value(),
        params,
        revision,
        record.value()->source.has_value() && catalog::allowsSourceProcessing(record.value()->sourceState),
        requestId,
    });
}

// 목적: current source metadata를 평가하고 필요하면 hash verification 예약
// 입력: record: 평가할 persisted photo record
// 출력: optional request ID 또는 metadata·repository 오류
OptionalRequestResult CatalogOrchestrator::observeSource(const catalog::CatalogPhotoRecord& record)
{
    if (!record.source.has_value())
    {
        return OptionalRequestResult::success(std::nullopt);
    }

    const auto activeRequest = m_activePhotoRequests.constFind(record.id.value);
    if (activeRequest != m_activePhotoRequests.cend())
    {
        return OptionalRequestResult::success(*activeRequest);
    }

    const catalog::SourceFingerprintResult metadata = catalog::inspectSourceMetadata(*record.source);
    if (metadata.hasError())
    {
        const std::optional<catalog::SourceBindingState> observedState = sourceStateForError(metadata.error());
        if (!observedState.has_value())
        {
            return OptionalRequestResult::failure(metadata.error());
        }
        if (record.sourceState != *observedState)
        {
            const catalog::CatalogPhotoMutationResult stored =
                m_photoRepository->recordSourceState(record.id, *observedState);
            if (stored.hasError())
            {
                return OptionalRequestResult::failure(stored.error());
            }
        }
        return OptionalRequestResult::success(std::nullopt);
    }

    if (isReplacementResolutionState(record.sourceState))
    {
        return OptionalRequestResult::success(std::nullopt);
    }

    const catalog::SourceBindingState evaluated =
        catalog::evaluateSourceBinding(record.fingerprint, {catalog::SourceAvailability::Available, metadata.value()});
    if (evaluated == catalog::SourceBindingState::Available &&
        record.sourceState == catalog::SourceBindingState::Available)
    {
        return OptionalRequestResult::success(std::nullopt);
    }

    if (evaluated == catalog::SourceBindingState::FingerprintPending)
    {
        if (record.sourceState != catalog::SourceBindingState::FingerprintPending)
        {
            const catalog::CatalogPhotoMutationResult stored =
                m_photoRepository->recordSourceState(record.id, catalog::SourceBindingState::IdentityUnverified);
            return stored.hasError() ? OptionalRequestResult::failure(stored.error())
                                     : OptionalRequestResult::success(std::nullopt);
        }
        const CatalogSourceSubmissionResult submitted =
            submitFingerprintJob(FingerprintPurpose::EstablishBaseline, record.id, *record.source);
        return submitted.hasError() ? OptionalRequestResult::failure(submitted.error())
                                    : OptionalRequestResult::success(submitted.value());
    }

    if (evaluated == catalog::SourceBindingState::IdentityUnverified)
    {
        if (record.sourceState != evaluated)
        {
            const catalog::CatalogPhotoMutationResult stored =
                m_photoRepository->recordSourceState(record.id, evaluated);
            if (stored.hasError())
            {
                return OptionalRequestResult::failure(stored.error());
            }
        }
        return OptionalRequestResult::success(std::nullopt);
    }

    if (record.sourceState != catalog::SourceBindingState::VerificationRequired)
    {
        const catalog::CatalogPhotoMutationResult stored =
            m_photoRepository->recordSourceState(record.id, catalog::SourceBindingState::VerificationRequired);
        if (stored.hasError())
        {
            return OptionalRequestResult::failure(stored.error());
        }
    }
    const CatalogSourceSubmissionResult submitted =
        submitFingerprintJob(FingerprintPurpose::VerifySource, record.id, *record.source);
    return submitted.hasError() ? OptionalRequestResult::failure(submitted.error())
                                : OptionalRequestResult::success(submitted.value());
}

// 목적: source fingerprint 계산을 직렬 worker pool에 제출
// 입력: purpose: 완료 후 transition, photoId: 대상, locator: hash source, replacementEntry: 신규 등록 정보
// 출력: accepted request ID 또는 session·중복·ID 오류
CatalogSourceSubmissionResult CatalogOrchestrator::submitFingerprintJob(FingerprintPurpose purpose,
                                                                        types::PhotoId photoId,
                                                                        types::SourceLocator locator,
                                                                        catalog::CatalogEntry replacementEntry)
{
    if (!m_acceptingRequests)
    {
        return CatalogSourceSubmissionResult::failure(
            {types::ErrorCode::Unknown, QStringLiteral("Catalog orchestrator is shutting down.")});
    }
    if (const std::optional<types::CoreError> error = requireOpenCatalog(); error.has_value())
    {
        return CatalogSourceSubmissionResult::failure(*error);
    }
    if (!types::isValidPhotoId(photoId) || locator.path.trimmed().isEmpty())
    {
        return CatalogSourceSubmissionResult::failure(
            {types::ErrorCode::InvalidArgument, QStringLiteral("Fingerprint request identity or source is invalid.")});
    }
    if (m_activePhotoRequests.contains(photoId.value))
    {
        return CatalogSourceSubmissionResult::failure(
            makeSessionError(QStringLiteral("A source fingerprint request is already active for this photo.")));
    }
    if (m_nextRequestId == std::numeric_limits<types::RequestId>::max())
    {
        return CatalogSourceSubmissionResult::failure(
            {types::ErrorCode::Unknown, QStringLiteral("Catalog request ID space is exhausted.")});
    }

    const types::RequestId requestId = m_nextRequestId++;
    const types::CancellationSource cancellationSource;
    auto* watcher = new QFutureWatcher<catalog::SourceFingerprintResult>(this);
    m_activeJobs.insert(requestId,
                        {purpose, photoId, locator, std::move(replacementEntry), cancellationSource, watcher});
    m_activePhotoRequests.insert(photoId.value, requestId);
    connect(watcher, &QFutureWatcher<catalog::SourceFingerprintResult>::finished, this, [this, requestId, watcher] {
        const catalog::SourceFingerprintResult result = watcher->result();
        handleFingerprintFinished(requestId, result);
        watcher->deleteLater();
    });

    const types::CancellationToken cancellationToken = cancellationSource.token();
    watcher->setFuture(QtConcurrent::run(
        &m_fingerprintPool, [locator = std::move(locator), cancellationToken]() -> catalog::SourceFingerprintResult {
            try
            {
                return catalog::calculateSourceFingerprint(locator, cancellationToken);
            }
            catch (const std::exception& error)
            {
                return catalog::SourceFingerprintResult::failure(
                    makeUnhandledError(QStringLiteral("Unhandled source fingerprint exception: %1").arg(error.what())));
            }
            catch (...)
            {
                return catalog::SourceFingerprintResult::failure(
                    makeUnhandledError(QStringLiteral("Unhandled non-standard source fingerprint exception.")));
            }
        }));
    return CatalogSourceSubmissionResult::success(requestId);
}

// 목적: worker fingerprint 결과를 repository state transition과 terminal event로 변환
// 입력: requestId: 완료된 request, result: 계산된 fingerprint 또는 오류
// 출력: updated·failed·cancelled terminal signal 하나
void CatalogOrchestrator::handleFingerprintFinished(types::RequestId requestId,
                                                    const catalog::SourceFingerprintResult& result)
{
    const auto activeJob = m_activeJobs.find(requestId);
    if (activeJob == m_activeJobs.end())
    {
        return;
    }

    const ActiveFingerprintJob job = activeJob.value();
    const bool cancellationRequested = job.cancellationSource.token().isCancellationRequested();
    m_activePhotoRequests.remove(job.photoId.value);
    m_activeJobs.erase(activeJob);
    if (cancellationRequested || (result.hasError() && result.error().code == types::ErrorCode::Cancelled))
    {
        emit sourceBindingCancelled(requestId);
        return;
    }
    if (result.hasError())
    {
        LOG_WARN("catalog", "Source fingerprint failed: {}", result.error().message.toStdString());
        emit sourceBindingFailed({requestId, job.photoId, result.error()});
        return;
    }

    const OptionalPhotoIdResult applied = applyFingerprintResult(job, result.value());
    if (applied.hasError())
    {
        LOG_WARN("catalog", "Source binding transition failed: {}", applied.error().message.toStdString());
        emit sourceBindingFailed({requestId, job.photoId, applied.error()});
        return;
    }

    const catalog::CatalogPhotoRecordResult updated = m_photoRepository->findById(job.photoId);
    if (updated.hasError() || !updated.value().has_value())
    {
        const types::CoreError error =
            updated.hasError()
                ? updated.error()
                : types::CoreError{types::ErrorCode::NotFound, QStringLiteral("Updated catalog photo is missing.")};
        emit sourceBindingFailed({requestId, job.photoId, error});
        return;
    }

    std::optional<catalog::CatalogPhotoRecord> createdPhoto;
    if (applied.value().has_value())
    {
        const catalog::CatalogPhotoRecordResult created = m_photoRepository->findById(*applied.value());
        if (created.hasError() || !created.value().has_value())
        {
            const types::CoreError error =
                created.hasError()
                    ? created.error()
                    : types::CoreError{types::ErrorCode::NotFound, QStringLiteral("Created catalog photo is missing.")};
            emit sourceBindingFailed({requestId, job.photoId, error});
            return;
        }
        createdPhoto = *created.value();
    }

    emit sourceBindingUpdated({requestId, job.photoId, *updated.value(), std::move(createdPhoto)});
}

// 목적: fingerprint purpose에 맞는 domain repository mutation 실행
// 입력: job: active request context, fingerprint: 완전한 SHA-256 observation
// 출력: optional 신규 PhotoId 또는 transition 오류
OptionalPhotoIdResult CatalogOrchestrator::applyFingerprintResult(const ActiveFingerprintJob& job,
                                                                  const types::SourceFingerprint& fingerprint)
{
    catalog::CatalogPhotoMutationResult mutation = catalog::CatalogPhotoMutationResult::success({});
    switch (job.purpose)
    {
    case FingerprintPurpose::EstablishBaseline:
        mutation = m_photoRepository->establishSourceFingerprint(job.photoId, fingerprint);
        break;
    case FingerprintPurpose::VerifySource:
    {
        const catalog::CatalogPhotoRecordResult record = m_photoRepository->findById(job.photoId);
        if (record.hasError())
        {
            return OptionalPhotoIdResult::failure(record.error());
        }
        if (!record.value().has_value())
        {
            return OptionalPhotoIdResult::failure(
                {types::ErrorCode::NotFound, QStringLiteral("Catalog photo does not exist.")});
        }
        const catalog::SourceBindingState evaluated = catalog::evaluateSourceBinding(
            record.value()->fingerprint, {catalog::SourceAvailability::Available, fingerprint});
        if (evaluated == catalog::SourceBindingState::Available)
        {
            mutation = m_photoRepository->confirmSourceMatch(job.photoId, fingerprint);
        }
        else if (evaluated == catalog::SourceBindingState::ReplacementDetected)
        {
            mutation =
                m_photoRepository->recordSourceState(job.photoId, catalog::SourceBindingState::ReplacementDetected);
        }
        else
        {
            return OptionalPhotoIdResult::failure(
                makeSessionError(QStringLiteral("Source verification produced an unexpected binding state.")));
        }
        break;
    }
    case FingerprintPurpose::AcceptReplacement:
        mutation = m_photoRepository->acceptReplacement(job.photoId, fingerprint);
        break;
    case FingerprintPurpose::RegisterReplacementAsNew:
    {
        const catalog::CatalogPhotoCreateResult created =
            m_photoRepository->registerReplacementAsNew(job.photoId, job.replacementEntry, fingerprint);
        return created.hasError() ? OptionalPhotoIdResult::failure(created.error())
                                  : OptionalPhotoIdResult::success(created.value());
    }
    case FingerprintPurpose::RelinkSource:
        mutation = m_photoRepository->relinkSource(job.photoId, job.locator, fingerprint);
        break;
    }

    return mutation.hasError() ? OptionalPhotoIdResult::failure(mutation.error())
                               : OptionalPhotoIdResult::success(std::nullopt);
}

// 목적: active fingerprint request를 모두 취소하고 worker 종료 대기
// 입력: publishCancellation: request별 cancellation signal 발생 여부
// 출력: active request map과 photo deduplication map이 비워짐
void CatalogOrchestrator::cancelAllSourceRequests(bool publishCancellation)
{
    QVector<types::RequestId> requestIds;
    requestIds.reserve(m_activeJobs.size());
    for (auto job = m_activeJobs.begin(); job != m_activeJobs.end(); ++job)
    {
        job.value().cancellationSource.requestCancellation();
        requestIds.push_back(job.key());
    }
    m_activePhotoRequests.clear();
    m_activeJobs.clear();
    if (publishCancellation)
    {
        for (types::RequestId requestId : std::as_const(requestIds))
        {
            emit sourceBindingCancelled(requestId);
        }
    }
    m_fingerprintPool.waitForDone();
}

}  // namespace flexraw::core::orchestration
