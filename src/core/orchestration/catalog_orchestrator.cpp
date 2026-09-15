#include "catalog_orchestrator.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <utility>

#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QMetaType>
#include <QThread>
#include <QtConcurrentRun>

#include "catalog_database.h"
#include "catalog_develop_repository.h"
#include "catalog_folder_importer.h"
#include "catalog_path.h"
#include "catalog_photo_repository.h"
#include "catalog_project_repository.h"
#include "client_error_projection.h"
#include "folder_scanner.h"
#include "log.h"
#include "source_binding.h"

namespace flexraw::core::orchestration
{
namespace
{

using OptionalRequestResult = types::Result<std::optional<types::RequestId>, types::CoreError>;
using OptionalPhotoIdResult = types::Result<std::optional<types::PhotoId>, types::CoreError>;

static_assert(client::DefaultCatalogPhotoPageSize == catalog::DefaultCatalogPhotoPageSize);
static_assert(client::MaximumCatalogPhotoPageSize == catalog::MaximumCatalogPhotoPageSize);

// 목적: QString을 Qt-free client contract용 UTF-8 string으로 변환
// 입력: value: Orchestration 또는 domain 문자열
// 출력: byte length를 보존한 UTF-8 string
[[nodiscard]] std::string toClientString(const QString& value)
{
    const QByteArray utf8 = value.toUtf8();
    return {utf8.constData(), static_cast<std::size_t>(utf8.size())};
}

// 목적: persisted Folder 집계 record를 Qt-free client snapshot으로 변환
// 입력: folder: normalized source parent path와 linked Photo 수
// 출력: UTF-8 path와 fixed-width count snapshot
[[nodiscard]] client::CatalogFolderSnapshot toClientFolder(const catalog::CatalogFolderSummary& folder)
{
    return {toClientString(folder.path), static_cast<std::int64_t>(folder.photoCount)};
}

// 목적: persisted Project record를 Qt-free client snapshot으로 변환
// 입력: project: Catalog repository가 반환한 Project record
// 출력: fixed-width identity와 UTF-8 이름 snapshot
[[nodiscard]] client::CatalogProjectSnapshot toClientProject(const catalog::CatalogProjectRecord& project)
{
    return {{project.id.value}, toClientString(project.name)};
}

// 목적: Project mutation의 Core 성공·오류를 Qt-free receipt result로 변환
// 입력: result: 기존 use case 결과, receipt: 성공 시 반환할 fixed-width identity
// 출력: identity receipt 또는 같은 분류의 client 오류
template<typename Receipt>
[[nodiscard]] client::ClientResult<Receipt, client::ClientError> toClientProjectMutation(
    const CatalogProjectMutationResult& result, Receipt receipt)
{
    if (result.hasError())
    {
        return client::ClientResult<Receipt, client::ClientError>::failure(toClientError(result.error()));
    }
    return client::ClientResult<Receipt, client::ClientError>::success(std::move(receipt));
}

// 목적: domain file kind를 Qt-free Catalog photo kind로 변환
// 입력: kind: persisted photo의 file 분류
// 출력: 같은 의미의 client enum
[[nodiscard]] client::CatalogFileKind toClientFileKind(types::SupportedFileKind kind) noexcept
{
    switch (kind)
    {
    case types::SupportedFileKind::Raw:
        return client::CatalogFileKind::Raw;
    case types::SupportedFileKind::RasterImage:
        return client::CatalogFileKind::RasterImage;
    case types::SupportedFileKind::Unknown:
        return client::CatalogFileKind::Unknown;
    }
    return client::CatalogFileKind::Unknown;
}

// 목적: folder scan entry를 Qt-free presentation snapshot으로 변환
// 입력: entry: 지원 파일 경로·표시 이름·분류
// 출력: UTF-8 문자열과 client file kind를 가진 immutable snapshot
[[nodiscard]] client::FolderItemSnapshot toClientFolderItem(const catalog::CatalogEntry& entry)
{
    return {
        toClientString(entry.file.path),
        toClientString(entry.file.extension),
        toClientString(entry.file.displayName),
        toClientFileKind(entry.file.kind),
    };
}

// 목적: domain scan status를 Qt-free Catalog scan status로 변환
// 입력: status: persisted photo scan 상태
// 출력: 같은 의미의 client enum
[[nodiscard]] client::CatalogScanStatus toClientScanStatus(types::FileScanStatus status) noexcept
{
    switch (status)
    {
    case types::FileScanStatus::Pending:
        return client::CatalogScanStatus::Pending;
    case types::FileScanStatus::Ready:
        return client::CatalogScanStatus::Ready;
    case types::FileScanStatus::Unsupported:
        return client::CatalogScanStatus::Unsupported;
    case types::FileScanStatus::Failed:
        return client::CatalogScanStatus::Failed;
    }
    return client::CatalogScanStatus::Pending;
}

// 목적: domain source binding state를 Qt-free Catalog source state로 변환
// 입력: state: persisted source identity 상태
// 출력: 같은 의미의 client enum
[[nodiscard]] client::CatalogSourceState toClientSourceState(catalog::SourceBindingState state) noexcept
{
    switch (state)
    {
    case catalog::SourceBindingState::FingerprintPending:
        return client::CatalogSourceState::FingerprintPending;
    case catalog::SourceBindingState::Available:
        return client::CatalogSourceState::Available;
    case catalog::SourceBindingState::Missing:
        return client::CatalogSourceState::Missing;
    case catalog::SourceBindingState::VerificationRequired:
        return client::CatalogSourceState::VerificationRequired;
    case catalog::SourceBindingState::IdentityUnverified:
        return client::CatalogSourceState::IdentityUnverified;
    case catalog::SourceBindingState::ReplacementDetected:
        return client::CatalogSourceState::ReplacementDetected;
    case catalog::SourceBindingState::Unreadable:
        return client::CatalogSourceState::Unreadable;
    case catalog::SourceBindingState::Unlinked:
        return client::CatalogSourceState::Unlinked;
    }
    return client::CatalogSourceState::FingerprintPending;
}

// 목적: source fingerprint metadata와 optional SHA-256를 Qt-free snapshot으로 변환
// 입력: fingerprint: Catalog에 저장된 source observation baseline
// 출력: fixed-width metadata와 byte vector
[[nodiscard]] client::CatalogSourceFingerprint toClientFingerprint(const types::SourceFingerprint& fingerprint)
{
    client::CatalogSourceFingerprint snapshot;
    snapshot.sizeBytes = fingerprint.sizeBytes;
    snapshot.modifiedAtMs = fingerprint.modifiedAtMs;
    snapshot.sha256.reserve(static_cast<std::size_t>(fingerprint.sha256.size()));
    for (const char byte : fingerprint.sha256)
    {
        snapshot.sha256.push_back(static_cast<std::uint8_t>(byte));
    }
    return snapshot;
}

// 목적: persisted photo record 전체를 Qt-free page state로 변환
// 입력: photo: Catalog repository photo record
// 출력: identity/source/display/fingerprint 의미를 보존한 snapshot
[[nodiscard]] client::CatalogPhotoSnapshot toClientPhoto(const catalog::CatalogPhotoRecord& photo)
{
    client::CatalogPhotoSnapshot snapshot;
    snapshot.id = {photo.id.value};
    if (photo.source.has_value())
    {
        snapshot.sourcePath = toClientString(photo.source->path);
    }
    snapshot.lastKnownPath = toClientString(photo.lastKnownPath);
    snapshot.extension = toClientString(photo.extension);
    snapshot.displayName = toClientString(photo.displayName);
    snapshot.kind = toClientFileKind(photo.kind);
    snapshot.scanStatus = toClientScanStatus(photo.scanStatus);
    snapshot.fingerprint = toClientFingerprint(photo.fingerprint);
    snapshot.sourceState = toClientSourceState(photo.sourceState);
    return snapshot;
}

// 목적: domain page cursor의 scope-bound key를 Qt-free cursor로 변환
// 입력: cursor: display name, PhotoId와 optional Folder/Project scope
// 출력: UTF-8/fixed-width cursor snapshot
[[nodiscard]] client::CatalogPhotoPageCursor toClientCursor(const catalog::CatalogPhotoPageCursor& cursor)
{
    client::CatalogPhotoPageCursor snapshot;
    snapshot.displayName = toClientString(cursor.displayName);
    snapshot.photoId = {cursor.photoId.value};
    if (cursor.exactFolderPath.has_value())
    {
        snapshot.exactFolderPath = toClientString(*cursor.exactFolderPath);
    }
    if (cursor.projectId.has_value())
    {
        snapshot.projectId = client::ClientProjectId{cursor.projectId->value};
    }
    return snapshot;
}

// 목적: UTF-8 client string을 internal QString으로 변환
// 입력: value: byte length가 명시된 UTF-8 문자열
// 출력: 같은 Unicode text를 보유한 QString
[[nodiscard]] QString fromClientString(const std::string& value)
{
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

// 목적: Qt-free UTF-8 contract string을 손실 없이 internal QString으로 변환
// 입력: value: UTF-8로 선언된 byte string
// 출력: round-trip 가능한 Unicode text 또는 invalid UTF-8이면 빈 값
[[nodiscard]] std::optional<QString> decodeClientString(const std::string& value)
{
    const QByteArray bytes(value.data(), static_cast<qsizetype>(value.size()));
    const QString decoded = QString::fromUtf8(bytes);
    return decoded.toUtf8() == bytes ? std::optional<QString>{decoded} : std::nullopt;
}

// 목적: Qt-free bounded page request를 기존 Catalog repository request로 변환
// 입력: request: client page 크기, 방향, cursor와 optional scope
// 출력: 같은 keyset 의미의 internal request
[[nodiscard]] catalog::CatalogPhotoPageRequest toCatalogPhotoPageRequest(const client::CatalogPhotoPageRequest& request)
{
    catalog::CatalogPhotoPageRequest internal;
    internal.pageSize = request.pageSize;
    internal.direction = request.direction == client::CatalogPhotoPageDirection::Backward
                             ? catalog::CatalogPhotoPageDirection::Backward
                             : catalog::CatalogPhotoPageDirection::Forward;
    if (request.cursor.has_value())
    {
        catalog::CatalogPhotoPageCursor cursor;
        cursor.displayName = fromClientString(request.cursor->displayName);
        cursor.photoId = {request.cursor->photoId.value};
        if (request.cursor->exactFolderPath.has_value())
        {
            cursor.exactFolderPath = fromClientString(*request.cursor->exactFolderPath);
        }
        if (request.cursor->projectId.has_value())
        {
            cursor.projectId = catalog::ProjectId{request.cursor->projectId->value};
        }
        internal.cursor = std::move(cursor);
    }
    if (request.exactFolderPath.has_value())
    {
        internal.exactFolderPath = fromClientString(*request.exactFolderPath);
    }
    if (request.projectId.has_value())
    {
        internal.projectId = catalog::ProjectId{request.projectId->value};
    }
    return internal;
}

// 목적: internal bounded page와 cursor를 Qt-free client state로 변환
// 입력: page: stable order photo record와 optional 양방향 cursor
// 출력: ownership이 독립된 client page snapshot
[[nodiscard]] client::CatalogPhotoPage toClientPhotoPage(const catalog::CatalogPhotoPage& page)
{
    client::CatalogPhotoPage snapshot;
    snapshot.photos.reserve(static_cast<std::size_t>(page.photos.size()));
    for (const catalog::CatalogPhotoRecord& photo : page.photos)
    {
        snapshot.photos.push_back(toClientPhoto(photo));
    }
    if (page.previousCursor.has_value())
    {
        snapshot.previousCursor = toClientCursor(*page.previousCursor);
    }
    if (page.nextCursor.has_value())
    {
        snapshot.nextCursor = toClientCursor(*page.nextCursor);
    }
    return snapshot;
}

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

struct CatalogOrchestrator::FolderOperationRuntime
{
    struct ActiveOperation
    {
        client::FolderOperationReceipt receipt;
        QString expectedCatalogPath;
    };

    QFutureWatcher<catalog::CatalogScanResult> watcher;
    std::optional<ActiveOperation> activeOperation;
};

// 목적: catalog-session state와 직렬 background fingerprint pool 생성
// 입력: parent: Qt 부모 object
// 출력: catalog가 열리지 않은 CatalogOrchestrator 객체
CatalogOrchestrator::CatalogOrchestrator(QObject* parent) : QObject(parent)
{
    qRegisterMetaType<CatalogPhotoState>();
    qRegisterMetaType<CatalogSourceUpdate>();
    qRegisterMetaType<CatalogIssue>();
    m_folderOperationRuntime = std::make_unique<FolderOperationRuntime>();
    connect(&m_folderOperationRuntime->watcher,
            &QFutureWatcher<catalog::CatalogScanResult>::finished,
            this,
            &CatalogOrchestrator::handleFolderScanFinished);
    m_fingerprintPool.setObjectName(QStringLiteral("CatalogFingerprintPool"));
    m_fingerprintPool.setMaxThreadCount(1);
}

// 목적: active fingerprint 작업을 취소하고 catalog connection 정리
// 입력: 없음
// 출력: worker와 database resource가 남지 않음
CatalogOrchestrator::~CatalogOrchestrator()
{
    m_acceptingRequests = false;
    m_folderOperationRuntime.reset();
    cancelAllSourceRequests(false);
    m_folderImporter.reset();
    m_developRepository.reset();
    m_projectRepository.reset();
    m_photoRepository.reset();
    m_database.reset();
}

// 목적: 현재 catalog-session immutable state 반환
// 입력: 없음
// 출력: open 여부와 normalized absolute catalog path
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
    m_projectRepository = std::make_unique<catalog::CatalogProjectRepository>(*m_database);
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
    m_projectRepository.reset();
    m_photoRepository.reset();
    m_database.reset();
    return state();
}

// 목적: 열린 catalog에서 optional Folder/Project scope와 stable cursor 기반 bounded photo page 조회
// 입력: request: page 크기, 방향, optional exclusive cursor와 mutually-exclusive scope
// 출력: 표시 순서의 bounded page 또는 session·validation·database 오류
CatalogPhotoPageResult CatalogOrchestrator::queryPhotos(const catalog::CatalogPhotoPageRequest& request) const
{
    if (const std::optional<types::CoreError> error = requireOpenCatalog(); error.has_value())
    {
        return CatalogPhotoPageResult::failure(*error);
    }

    if (request.projectId.has_value())
    {
        const catalog::CatalogProjectFindResult project = m_projectRepository->findById(*request.projectId);
        if (project.hasError())
        {
            return CatalogPhotoPageResult::failure(project.error());
        }
        if (!project.value().has_value())
        {
            return CatalogPhotoPageResult::failure(
                {types::ErrorCode::NotFound, QStringLiteral("Project does not exist.")});
        }
    }
    return m_photoRepository->queryPage(request);
}

// 목적: optional Catalog/Folder/Project scope의 bounded page를 Qt-free snapshot으로 조회
// 입력: request: fixed-width page 크기, 방향, optional cursor와 UTF-8 scope
// 출력: stable order photo snapshot과 양방향 cursor 또는 구조화된 client 오류
client::CatalogPhotoPageResult CatalogOrchestrator::queryPhotoPage(const client::CatalogPhotoPageRequest& request) const
{
    const CatalogPhotoPageResult page = queryPhotos(toCatalogPhotoPageRequest(request));
    if (page.hasError())
    {
        return client::CatalogPhotoPageResult::failure(toClientError(page.error()));
    }
    return client::CatalogPhotoPageResult::success(toClientPhotoPage(page.value()));
}

// 목적: active Catalog에서 linked Photo가 존재하는 distinct Folder snapshot 조회
// 입력: 없음
// 출력: normalized UTF-8 path 순서와 양수 photo 수 또는 구조화된 client 오류
client::CatalogFolderListResult CatalogOrchestrator::listFolders() const
{
    if (const std::optional<types::CoreError> error = requireOpenCatalog(); error.has_value())
    {
        return client::CatalogFolderListResult::failure(toClientError(*error));
    }

    const catalog::CatalogFolderQueryResult folders = m_photoRepository->queryFolders();
    if (folders.hasError())
    {
        return client::CatalogFolderListResult::failure(toClientError(folders.error()));
    }

    std::vector<client::CatalogFolderSnapshot> snapshots;
    snapshots.reserve(static_cast<std::size_t>(folders.value().size()));
    for (const catalog::CatalogFolderSummary& folder : folders.value())
    {
        snapshots.push_back(toClientFolder(folder));
    }
    return client::CatalogFolderListResult::success(std::move(snapshots));
}

// 목적: active Catalog 안에 logical Project 생성
// 입력: name: 공백 제거 후 비어 있지 않은 Project 표시 이름
// 출력: 발급된 Project identity와 이름 또는 session·validation·database 오류
CatalogProjectResult CatalogOrchestrator::createProject(const QString& name)
{
    if (const std::optional<types::CoreError> error = requireOpenCatalog(); error.has_value())
    {
        return CatalogProjectResult::failure(*error);
    }
    return m_projectRepository->createProject(name);
}

// 목적: active Catalog의 Project 목록 조회
// 입력: 없음
// 출력: 이름과 identity stable order의 Project 목록 또는 session·database 오류
CatalogProjectListResult CatalogOrchestrator::queryProjects() const
{
    if (const std::optional<types::CoreError> error = requireOpenCatalog(); error.has_value())
    {
        return CatalogProjectListResult::failure(*error);
    }
    return m_projectRepository->queryProjects();
}

// 목적: active Catalog Project를 Qt-free client snapshot으로 조회
// 입력: 없음
// 출력: UTF-8 이름과 fixed-width identity 목록 또는 구조화된 client 오류
client::CatalogProjectListResult CatalogOrchestrator::listProjects() const
{
    const CatalogProjectListResult projects = queryProjects();
    if (projects.hasError())
    {
        return client::CatalogProjectListResult::failure(toClientError(projects.error()));
    }

    std::vector<client::CatalogProjectSnapshot> snapshots;
    snapshots.reserve(static_cast<std::size_t>(projects.value().size()));
    for (const catalog::CatalogProjectRecord& project : projects.value())
    {
        snapshots.push_back(toClientProject(project));
    }
    return client::CatalogProjectListResult::success(std::move(snapshots));
}

// 목적: Qt-free command로 active Catalog Project 생성
// 입력: command: UTF-8 Project 표시 이름
// 출력: 생성된 Project snapshot 또는 구조화된 client 오류
client::CatalogProjectResult CatalogOrchestrator::createProject(const client::CreateProjectCommand& command)
{
    const CatalogProjectResult project = createProject(fromClientString(command.name));
    if (project.hasError())
    {
        return client::CatalogProjectResult::failure(toClientError(project.error()));
    }
    return client::CatalogProjectResult::success(toClientProject(project.value()));
}

// 목적: Qt-free command로 active Catalog Project 이름 변경
// 입력: command: fixed-width identity와 UTF-8 새 표시 이름
// 출력: 변경된 Project snapshot 또는 구조화된 client 오류
client::CatalogProjectResult CatalogOrchestrator::renameProject(const client::RenameProjectCommand& command)
{
    const CatalogProjectResult project =
        renameProject(catalog::ProjectId{command.projectId.value}, fromClientString(command.name));
    if (project.hasError())
    {
        return client::CatalogProjectResult::failure(toClientError(project.error()));
    }
    return client::CatalogProjectResult::success(toClientProject(project.value()));
}

// 목적: Qt-free command로 active Catalog Project와 membership 삭제
// 입력: command: 삭제할 Project identity
// 출력: Photo 보존을 전제로 한 mutation receipt 또는 구조화된 client 오류
client::CatalogProjectDeleteResult CatalogOrchestrator::deleteProject(const client::DeleteProjectCommand& command)
{
    return toClientProjectMutation(removeProject(catalog::ProjectId{command.projectId.value}),
                                   client::CatalogProjectDeleteReceipt{command.projectId});
}

// 목적: Qt-free command로 Project membership에 Photo 추가
// 입력: command: Project와 Photo identity 한 쌍
// 출력: 처리한 identity receipt 또는 구조화된 client 오류
client::CatalogProjectMembershipResult CatalogOrchestrator::addPhotoToProject(
    const client::ProjectPhotoMembershipCommand& command)
{
    return toClientProjectMutation(
        addPhotoToProject(catalog::ProjectId{command.projectId.value}, types::PhotoId{command.photoId.value}),
        client::CatalogProjectMembershipReceipt{command.projectId, command.photoId});
}

// 목적: Qt-free command로 Project membership에서 Photo 제거
// 입력: command: Project와 Photo identity 한 쌍
// 출력: 처리한 identity receipt 또는 구조화된 client 오류
client::CatalogProjectMembershipResult CatalogOrchestrator::removePhotoFromProject(
    const client::ProjectPhotoMembershipCommand& command)
{
    return toClientProjectMutation(
        removePhotoFromProject(catalog::ProjectId{command.projectId.value}, types::PhotoId{command.photoId.value}),
        client::CatalogProjectMembershipReceipt{command.projectId, command.photoId});
}

// 목적: active Catalog Project의 표시 이름 변경
// 입력: projectId: 변경 대상, name: 새 Project 표시 이름
// 출력: 갱신된 Project 또는 session·validation·not-found·database 오류
CatalogProjectResult CatalogOrchestrator::renameProject(catalog::ProjectId projectId, const QString& name)
{
    if (const std::optional<types::CoreError> error = requireOpenCatalog(); error.has_value())
    {
        return CatalogProjectResult::failure(*error);
    }
    return m_projectRepository->renameProject(projectId, name);
}

// 목적: active Catalog에서 Project와 membership만 삭제
// 입력: projectId: 삭제할 Project identity
// 출력: Photo와 Develop state를 보존한 성공 표식 또는 오류
CatalogProjectMutationResult CatalogOrchestrator::removeProject(catalog::ProjectId projectId)
{
    if (const std::optional<types::CoreError> error = requireOpenCatalog(); error.has_value())
    {
        return CatalogProjectMutationResult::failure(*error);
    }
    return m_projectRepository->removeProject(projectId);
}

// 목적: Project에 기존 Catalog Photo membership 추가
// 입력: projectId: 대상 Project, photoId: 추가할 stable Photo identity
// 출력: idempotent 성공 표식 또는 session·identity·database 오류
CatalogProjectMutationResult CatalogOrchestrator::addPhotoToProject(catalog::ProjectId projectId,
                                                                    types::PhotoId photoId)
{
    if (const std::optional<types::CoreError> error = requireOpenCatalog(); error.has_value())
    {
        return CatalogProjectMutationResult::failure(*error);
    }
    return m_projectRepository->addPhoto(projectId, photoId);
}

// 목적: Project에서 Photo membership 제거
// 입력: projectId: 대상 Project, photoId: 제거할 stable Photo identity
// 출력: idempotent 성공 표식 또는 session·identity·database 오류
CatalogProjectMutationResult CatalogOrchestrator::removePhotoFromProject(catalog::ProjectId projectId,
                                                                         types::PhotoId photoId)
{
    if (const std::optional<types::CoreError> error = requireOpenCatalog(); error.has_value())
    {
        return CatalogProjectMutationResult::failure(*error);
    }
    return m_projectRepository->removePhoto(projectId, photoId);
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

// 목적: Qt-free command로 Catalog와 독립적인 단일 folder scan 제출
// 입력: command: UTF-8 folder path
// 출력: accepted operation receipt 또는 validation·busy 오류
client::FolderOperationResult CatalogOrchestrator::submitFolderScan(const client::ScanFolderCommand& command)
{
    return submitFolderOperation(client::FolderOperationKind::Scan, command.folderPath, std::nullopt);
}

// 목적: Qt-free command로 expected Catalog에 묶인 단일 folder import 제출
// 입력: command: UTF-8 folder path와 submit 시점 Catalog path
// 출력: accepted operation receipt 또는 validation·session·busy 오류
client::FolderOperationResult CatalogOrchestrator::submitFolderImport(const client::ImportFolderCommand& command)
{
    return submitFolderOperation(client::FolderOperationKind::Import,
                                 command.folderPath,
                                 std::optional<std::string>{command.expectedCatalogPath});
}

// 목적: Qt event adapter initial projection용 current Folder operation 조회
// 입력: 없음
// 출력: accepted active receipt 또는 idle 상태의 빈 값
std::optional<client::FolderOperationReceipt> CatalogOrchestrator::activeFolderOperation() const
{
    if (m_folderOperationRuntime == nullptr || !m_folderOperationRuntime->activeOperation.has_value())
    {
        return std::nullopt;
    }
    return m_folderOperationRuntime->activeOperation->receipt;
}

// 목적: 공통 validation과 single-flight 정책으로 Folder operation 제출
// 입력: kind: scan/import 구분, folderPath: UTF-8 경로, expectedCatalogPath: import session baseline
// 출력: accepted receipt 또는 owner-thread·validation·session·busy 오류
client::FolderOperationResult CatalogOrchestrator::submitFolderOperation(
    client::FolderOperationKind kind,
    const std::string& folderPath,
    const std::optional<std::string>& expectedCatalogPath)
{
    if (QThread::currentThread() != thread())
    {
        return client::FolderOperationResult::failure(
            {client::ClientErrorCode::Conflict, "Folder operation must be submitted on its owner thread."});
    }
    if (!m_acceptingRequests || m_folderOperationRuntime == nullptr)
    {
        return client::FolderOperationResult::failure(
            {client::ClientErrorCode::Conflict, "Catalog orchestrator is shutting down."});
    }
    const QString requestedFolderPath = fromClientString(folderPath);
    if (requestedFolderPath.isEmpty() || requestedFolderPath != requestedFolderPath.trimmed())
    {
        return client::FolderOperationResult::failure(
            {client::ClientErrorCode::InvalidArgument, "Folder path is empty or contains outer whitespace."});
    }
    if (m_folderOperationRuntime->activeOperation.has_value())
    {
        return client::FolderOperationResult::failure(
            {client::ClientErrorCode::Conflict, "Another folder operation is already active."});
    }

    QString normalizedExpectedCatalogPath;
    if (kind == client::FolderOperationKind::Import)
    {
        const QString requestedCatalogPath =
            expectedCatalogPath.has_value() ? fromClientString(*expectedCatalogPath) : QString{};
        if (requestedCatalogPath.isEmpty() || requestedCatalogPath != requestedCatalogPath.trimmed())
        {
            return client::FolderOperationResult::failure(
                {client::ClientErrorCode::InvalidArgument,
                 "Expected Catalog path is empty or contains outer whitespace."});
        }
        if (const std::optional<types::CoreError> error = requireOpenCatalog(); error.has_value())
        {
            return client::FolderOperationResult::failure(toClientError(*error));
        }
        if (!catalog::catalogPathsReferToSameFile(requestedCatalogPath, m_database->catalogPath()))
        {
            return client::FolderOperationResult::failure(
                {client::ClientErrorCode::Conflict, "Active Catalog does not match the import baseline."});
        }
        normalizedExpectedCatalogPath = m_database->catalogPath();
    }
    if (m_nextFolderOperationId == std::numeric_limits<std::uint64_t>::max())
    {
        return client::FolderOperationResult::failure(
            {client::ClientErrorCode::Unknown, "Folder operation ID space is exhausted."});
    }

    const QString normalizedFolderPath = QFileInfo(requestedFolderPath).absoluteFilePath();
    client::FolderOperationReceipt receipt;
    receipt.id = {m_nextFolderOperationId++};
    receipt.kind = kind;
    receipt.folderPath = toClientString(normalizedFolderPath);
    m_folderOperationRuntime->activeOperation =
        FolderOperationRuntime::ActiveOperation{receipt, normalizedExpectedCatalogPath};
    emit folderOperationStarted(receipt);
    m_folderOperationRuntime->watcher.setFuture(
        QtConcurrent::run([normalizedFolderPath] { return catalog::scanFolder(normalizedFolderPath); }));
    return client::FolderOperationResult::success(std::move(receipt));
}

// 목적: background scan 결과를 scan snapshot 또는 Catalog persistence terminal로 조립
// 입력: 없음; active runtime과 watcher result 사용
// 출력: active operation을 비운 뒤 terminal signal 하나 발생
void CatalogOrchestrator::handleFolderScanFinished()
{
    if (m_folderOperationRuntime == nullptr || !m_folderOperationRuntime->activeOperation.has_value())
    {
        return;
    }

    const FolderOperationRuntime::ActiveOperation active = *m_folderOperationRuntime->activeOperation;
    const catalog::CatalogScanResult scanned = m_folderOperationRuntime->watcher.result();
    client::FolderOperationTerminal terminal;
    terminal.receipt = active.receipt;
    if (scanned.hasError())
    {
        terminal.state = client::FolderOperationTerminalState::Failed;
        terminal.error = toClientError(scanned.error());
    }
    else if (active.receipt.kind == client::FolderOperationKind::Scan)
    {
        client::FolderScanCompletion completion;
        completion.items.reserve(static_cast<std::size_t>(scanned.value().size()));
        for (const catalog::CatalogEntry& entry : scanned.value())
        {
            completion.items.push_back(toClientFolderItem(entry));
        }
        terminal.completion = client::FolderOperationCompletion{std::move(completion)};
    }
    else if (m_database == nullptr || m_database->catalogPath() != active.expectedCatalogPath)
    {
        terminal.state = client::FolderOperationTerminalState::Failed;
        terminal.error = client::ClientError{client::ClientErrorCode::Conflict,
                                             "Active Catalog changed before folder import persistence."};
    }
    else
    {
        const CatalogImportResult imported = importScannedEntries(scanned.value());
        if (imported.hasError())
        {
            terminal.state = client::FolderOperationTerminalState::Failed;
            terminal.error = toClientError(imported.error());
        }
        else
        {
            client::FolderImportCompletion completion;
            completion.discoveredCount = static_cast<std::int64_t>(imported.value().scannedCount);
            completion.appliedCount = static_cast<std::int64_t>(imported.value().storedCount);
            completion.photoIds.reserve(static_cast<std::size_t>(imported.value().photoIds.size()));
            for (types::PhotoId photoId : imported.value().photoIds)
            {
                completion.photoIds.push_back({photoId.value});
            }
            terminal.completion = client::FolderOperationCompletion{std::move(completion)};
        }
    }

    m_folderOperationRuntime->activeOperation.reset();
    emit folderOperationTerminal(terminal);
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

// 목적: Qt-free command로 replacement source를 기존 Photo identity의 새 baseline으로 수용
// 입력: command: develop state를 유지할 fixed-width Photo identity
// 출력: accepted request context 또는 validation·capability·session 오류
client::SourceRequestResult CatalogOrchestrator::acceptReplacement(const client::AcceptReplacementCommand& command)
{
    if (command.photoId.value <= 0)
    {
        return client::SourceRequestResult::failure(
            {client::ClientErrorCode::InvalidArgument, "Source Resolution Photo identity must be positive."});
    }

    const CatalogSourceSubmissionResult submitted = acceptReplacement(types::PhotoId{command.photoId.value});
    if (submitted.hasError())
    {
        return client::SourceRequestResult::failure(toClientError(submitted.error()));
    }
    const auto active = m_activeJobs.constFind(submitted.value());
    if (active == m_activeJobs.cend())
    {
        return client::SourceRequestResult::failure(
            {client::ClientErrorCode::Unknown, "Accepted source request is missing from its owner."});
    }
    return client::SourceRequestResult::success(toSourceRequestReceipt(submitted.value(), active.value()));
}

// 목적: Qt-free command로 replacement source에 새 Photo identity 발급
// 입력: command: source binding을 해제할 기존 fixed-width Photo identity
// 출력: accepted request context 또는 validation·capability·session 오류
client::SourceRequestResult CatalogOrchestrator::registerReplacementAsNew(
    const client::RegisterReplacementAsNewCommand& command)
{
    if (command.photoId.value <= 0)
    {
        return client::SourceRequestResult::failure(
            {client::ClientErrorCode::InvalidArgument, "Source Resolution Photo identity must be positive."});
    }

    const CatalogSourceSubmissionResult submitted = registerReplacementAsNew(types::PhotoId{command.photoId.value});
    if (submitted.hasError())
    {
        return client::SourceRequestResult::failure(toClientError(submitted.error()));
    }
    const auto active = m_activeJobs.constFind(submitted.value());
    if (active == m_activeJobs.cend())
    {
        return client::SourceRequestResult::failure(
            {client::ClientErrorCode::Unknown, "Accepted source request is missing from its owner."});
    }
    return client::SourceRequestResult::success(toSourceRequestReceipt(submitted.value(), active.value()));
}

// 목적: Qt-free command로 기존 Photo를 normalized absolute locator에 재연결
// 입력: command: fixed-width Photo identity와 UTF-8 lexical locator
// 출력: accepted request context 또는 validation·capability·session 오류
client::SourceRequestResult CatalogOrchestrator::relinkSource(const client::RelinkSourceCommand& command)
{
    const std::optional<QString> decodedLocator = decodeClientString(command.sourceLocator);
    if (command.photoId.value <= 0 || !decodedLocator.has_value())
    {
        return client::SourceRequestResult::failure(
            {client::ClientErrorCode::InvalidArgument, "Source relink command identity or UTF-8 locator is invalid."});
    }
    const QString normalizedLocator = QDir::cleanPath(QDir::fromNativeSeparators(decodedLocator->trimmed()));
    if (normalizedLocator.isEmpty() || normalizedLocator == QStringLiteral(".") ||
        !QFileInfo(normalizedLocator).isAbsolute() || normalizedLocator != *decodedLocator)
    {
        return client::SourceRequestResult::failure(
            {client::ClientErrorCode::InvalidArgument,
             "Source relink locator must be normalized, absolute UTF-8 lexical text."});
    }

    const CatalogSourceSubmissionResult submitted =
        relinkSource(types::PhotoId{command.photoId.value}, types::SourceLocator{normalizedLocator});
    if (submitted.hasError())
    {
        return client::SourceRequestResult::failure(toClientError(submitted.error()));
    }
    const auto active = m_activeJobs.constFind(submitted.value());
    if (active == m_activeJobs.cend())
    {
        return client::SourceRequestResult::failure(
            {client::ClientErrorCode::Unknown, "Accepted source request is missing from its owner."});
    }
    return client::SourceRequestResult::success(toSourceRequestReceipt(submitted.value(), active.value()));
}

// 목적: Qt-free identity로 accepted source request 취소
// 입력: requestId: owner가 발급한 source request identity
// 출력: 취소된 identity 또는 stale·validation 오류
client::SourceRequestCancelResult CatalogOrchestrator::cancelSourceRequest(client::SourceRequestId requestId)
{
    if (requestId.value == 0)
    {
        return client::SourceRequestCancelResult::failure(
            {client::ClientErrorCode::InvalidArgument, "Source request identity must be positive."});
    }
    if (!cancelSourceRequest(static_cast<types::RequestId>(requestId.value)))
    {
        return client::SourceRequestCancelResult::failure(
            {client::ClientErrorCode::NotFound, "Source request is no longer active."});
    }
    return client::SourceRequestCancelResult::success(requestId);
}

// 목적: Qt event adapter initial projection용 current source lifecycle 조회
// 입력: 없음
// 출력: request identity·kind·Photo·locator를 가진 active request 목록
client::SourceResolutionSnapshot CatalogOrchestrator::sourceResolutionSnapshot() const
{
    client::SourceResolutionSnapshot snapshot;
    snapshot.activeRequests.reserve(static_cast<std::size_t>(m_activeJobs.size()));
    for (auto job = m_activeJobs.cbegin(); job != m_activeJobs.cend(); ++job)
    {
        snapshot.activeRequests.push_back(toSourceRequestReceipt(job.key(), job.value()));
    }
    std::sort(snapshot.activeRequests.begin(), snapshot.activeRequests.end(), [](const auto& left, const auto& right) {
        return left.id.value < right.id.value;
    });
    return snapshot;
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
    if (locator.path.isEmpty() || locator.path != locator.path.trimmed())
    {
        return CatalogSourceSubmissionResult::failure(
            {types::ErrorCode::InvalidArgument,
             QStringLiteral("Relink source path is empty or contains outer whitespace.")});
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
    if (m_database == nullptr || m_photoRepository == nullptr || m_projectRepository == nullptr ||
        m_developRepository == nullptr || m_folderImporter == nullptr)
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

// 목적: active fingerprint job을 Qt-free source request context로 투영
// 입력: requestId: owner identity, job: purpose·Photo·locator runtime
// 출력: event와 command receipt가 공유하는 immutable request context
client::SourceRequestReceipt CatalogOrchestrator::toSourceRequestReceipt(types::RequestId requestId,
                                                                         const ActiveFingerprintJob& job) const
{
    client::SourceRequestKind kind = client::SourceRequestKind::VerifySource;
    switch (job.purpose)
    {
    case FingerprintPurpose::EstablishBaseline:
        kind = client::SourceRequestKind::EstablishBaseline;
        break;
    case FingerprintPurpose::VerifySource:
        kind = client::SourceRequestKind::VerifySource;
        break;
    case FingerprintPurpose::AcceptReplacement:
        kind = client::SourceRequestKind::AcceptReplacement;
        break;
    case FingerprintPurpose::RegisterReplacementAsNew:
        kind = client::SourceRequestKind::RegisterReplacementAsNew;
        break;
    case FingerprintPurpose::RelinkSource:
        kind = client::SourceRequestKind::RelinkSource;
        break;
    }
    return {{requestId}, kind, {job.photoId.value}, toClientString(job.locator.path)};
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
    if (!types::isValidPhotoId(photoId) || locator.path.isEmpty() || locator.path != locator.path.trimmed())
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
    emit sourceBindingStarted(requestId, photoId);
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
