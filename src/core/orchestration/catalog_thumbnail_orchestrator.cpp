#include "catalog_thumbnail_orchestrator.h"

#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

#include <QByteArray>
#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QtConcurrentRun>

namespace flexraw::core::orchestration
{
namespace
{

struct ValidatedThumbnailItem
{
    client::CatalogThumbnailItemIdentity identity;
    types::FileDescriptor source;
};

using ValidatedThumbnailItemResult = client::ClientResult<ValidatedThumbnailItem, client::ClientError>;

// 목적: Catalog thumbnail contract validation 실패 생성
// 입력: message: diagnostics 전용 UTF-8 설명
// 출력: InvalidArgument client error
[[nodiscard]] client::ClientError makeInvalidArgument(std::string message)
{
    return {client::ClientErrorCode::InvalidArgument, std::move(message)};
}

// 목적: contract UTF-8 byte string을 손실 없이 QString으로 변환
// 입력: value: UTF-8로 선언된 client string
// 출력: round-trip 가능한 Unicode 문자열 또는 invalid UTF-8이면 빈 값
[[nodiscard]] std::optional<QString> decodeClientString(const std::string& value)
{
    const QByteArray bytes(value.data(), static_cast<qsizetype>(value.size()));
    const QString decoded = QString::fromUtf8(bytes);
    return decoded.toUtf8() == bytes ? std::optional<QString>{decoded} : std::nullopt;
}

// 목적: Qt-free file kind를 thumbnail pipeline 입력 enum으로 변환
// 입력: kind: client contract file 분류
// 출력: 지원되는 RAW/raster kind 또는 Unknown
[[nodiscard]] types::SupportedFileKind toInternalFileKind(client::CatalogFileKind kind) noexcept
{
    switch (kind)
    {
    case client::CatalogFileKind::Raw:
        return types::SupportedFileKind::Raw;
    case client::CatalogFileKind::RasterImage:
        return types::SupportedFileKind::RasterImage;
    case client::CatalogFileKind::Unknown:
        return types::SupportedFileKind::Unknown;
    }
    return types::SupportedFileKind::Unknown;
}

// 목적: tagged thumbnail identity를 window 중복 제거용 deterministic key로 변환
// 입력: identity: Catalog PhotoId 또는 transient normalized locator
// 출력: kind prefix를 포함한 window-local key
[[nodiscard]] std::string identityKey(const client::CatalogThumbnailItemIdentity& identity)
{
    return identity.kind == client::CatalogThumbnailIdentityKind::CatalogPhoto
               ? std::string("photo:") + std::to_string(identity.photoId.value)
               : std::string("source:") + identity.transientSourceLocator;
}

// 목적: Qt-free thumbnail item identity와 locator를 pipeline 입력으로 검증·변환
// 입력: item: tagged identity, normalized UTF-8 locator와 file metadata
// 출력: internal descriptor 또는 InvalidArgument 오류
[[nodiscard]] ValidatedThumbnailItemResult validateThumbnailItem(const client::CatalogThumbnailItem& item)
{
    const std::optional<QString> sourceLocator = decodeClientString(item.sourceLocator);
    const std::optional<QString> extension = decodeClientString(item.extension);
    const std::optional<QString> displayName = decodeClientString(item.displayName);
    if (!sourceLocator.has_value() || !extension.has_value() || !displayName.has_value())
    {
        return ValidatedThumbnailItemResult::failure(
            makeInvalidArgument("Catalog thumbnail item contains invalid UTF-8 text."));
    }

    const QString normalizedLocator = QDir::cleanPath(QDir::fromNativeSeparators(sourceLocator->trimmed()));
    if (normalizedLocator.isEmpty() || normalizedLocator == QStringLiteral(".") ||
        !QFileInfo(normalizedLocator).isAbsolute() || normalizedLocator != *sourceLocator)
    {
        return ValidatedThumbnailItemResult::failure(
            makeInvalidArgument("Catalog thumbnail source locator must be normalized and absolute."));
    }

    switch (item.identity.kind)
    {
    case client::CatalogThumbnailIdentityKind::CatalogPhoto:
        if (item.identity.photoId.value <= 0 || !item.identity.transientSourceLocator.empty())
        {
            return ValidatedThumbnailItemResult::failure(
                makeInvalidArgument("Catalog thumbnail Photo identity is invalid."));
        }
        break;
    case client::CatalogThumbnailIdentityKind::TransientSource:
        if (item.identity.photoId.value != 0 || item.identity.transientSourceLocator != item.sourceLocator)
        {
            return ValidatedThumbnailItemResult::failure(
                makeInvalidArgument("Transient thumbnail identity must equal its normalized source locator."));
        }
        break;
    default:
        return ValidatedThumbnailItemResult::failure(
            makeInvalidArgument("Catalog thumbnail identity kind is invalid."));
    }

    const types::SupportedFileKind kind = toInternalFileKind(item.kind);
    if (kind != types::SupportedFileKind::Raw && kind != types::SupportedFileKind::RasterImage)
    {
        return ValidatedThumbnailItemResult::failure(
            makeInvalidArgument("Catalog thumbnail file kind is unsupported."));
    }

    return ValidatedThumbnailItemResult::success({item.identity, {*sourceLocator, *extension, *displayName, kind}});
}

// 목적: thumbnail window의 bounded item 수와 target pixel extent 검증
// 입력: command: frontend가 계산한 visible/adjacent window
// 출력: 유효하면 빈 값, 아니면 InvalidArgument 오류
[[nodiscard]] std::optional<client::ClientError> validateWindowCommand(
    const client::ReplaceCatalogThumbnailWindowCommand& command)
{
    if (command.items.empty())
    {
        return makeInvalidArgument("Catalog thumbnail replacement window must not be empty; use clear instead.");
    }
    if (command.items.size() > client::MaximumCatalogThumbnailWindowSize)
    {
        return makeInvalidArgument("Catalog thumbnail window exceeds its bounded size.");
    }
    if (command.targetExtent.width == 0 || command.targetExtent.height == 0 ||
        command.targetExtent.width > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
        command.targetExtent.height > static_cast<std::uint32_t>(std::numeric_limits<int>::max()))
    {
        return makeInvalidArgument("Catalog thumbnail target extent must be a positive supported pixel size.");
    }
    return std::nullopt;
}

}  // namespace

// 목적: viewport thumbnail을 직렬 background decode하는 application-scoped Orchestrator 생성
// 입력: pipeline: worker에서 사용할 synchronous thumbnail pipeline, parent: Qt 부모 object
// 출력: bounded pending window를 가진 Orchestrator 객체
CatalogThumbnailOrchestrator::CatalogThumbnailOrchestrator(std::unique_ptr<ICatalogThumbnailPipeline> pipeline,
                                                           QObject* parent)
    : QObject(parent), m_pipeline(std::move(pipeline))
{
    if (m_pipeline == nullptr)
    {
        throw std::invalid_argument("Catalog thumbnail pipeline must not be null.");
    }
    qRegisterMetaType<CatalogThumbnailWindowStarted>();
    qRegisterMetaType<CatalogThumbnailFrame>();
    qRegisterMetaType<CatalogThumbnailIssue>();
    qRegisterMetaType<CatalogThumbnailWindowTerminal>();
    m_workerPool.setObjectName(QStringLiteral("CatalogThumbnailPool"));
    m_workerPool.setMaxThreadCount(1);
}

// 목적: active thumbnail decode를 취소하고 worker resource 정리
// 입력: 없음
// 출력: pending/active thumbnail 작업이 남지 않음
CatalogThumbnailOrchestrator::~CatalogThumbnailOrchestrator()
{
    m_acceptingRequests = false;
    m_pendingJobs.clear();
    cancelActiveJobs();
    m_activeWindow.reset();
    m_workerPool.clear();
    m_workerPool.waitForDone();
    for (ActiveJob& job : m_activeJobs)
    {
        delete job.watcher;
        job.watcher = nullptr;
    }
    m_activeJobs.clear();
}

// 목적: 현재 viewport와 인접 범위의 Qt-free tagged thumbnail window 교체
// 입력: command: 최대 200개 item과 양수 target pixel 크기
// 출력: owner generation과 중복 제거 item 수 또는 validation·shutdown 오류
client::CatalogThumbnailWindowResult CatalogThumbnailOrchestrator::replaceThumbnailWindow(
    const client::ReplaceCatalogThumbnailWindowCommand& command)
{
    if (!m_acceptingRequests)
    {
        return client::CatalogThumbnailWindowResult::failure(
            {client::ClientErrorCode::Conflict, "Catalog thumbnail orchestrator is shutting down."});
    }
    if (const std::optional<client::ClientError> error = validateWindowCommand(command); error.has_value())
    {
        return client::CatalogThumbnailWindowResult::failure(*error);
    }
    if (m_windowRevision == std::numeric_limits<std::uint64_t>::max())
    {
        return client::CatalogThumbnailWindowResult::failure(
            {client::ClientErrorCode::Unknown, "Catalog thumbnail window generation space is exhausted."});
    }

    QVector<PendingJob> validatedJobs;
    validatedJobs.reserve(static_cast<qsizetype>(command.items.size()));
    std::set<std::string> acceptedIdentities;
    for (const client::CatalogThumbnailItem& item : command.items)
    {
        const ValidatedThumbnailItemResult validated = validateThumbnailItem(item);
        if (validated.hasError())
        {
            return client::CatalogThumbnailWindowResult::failure(validated.error());
        }
        if (acceptedIdentities.insert(identityKey(validated.value().identity)).second)
        {
            validatedJobs.push_back({{}, validated.value().identity, validated.value().source, {}});
        }
    }

    cancelCurrentWindow();
    const client::CatalogThumbnailWindowGeneration generation{++m_windowRevision};
    const client::CatalogThumbnailTargetExtent targetExtent = command.targetExtent;
    const QSize targetSize{static_cast<int>(targetExtent.width), static_cast<int>(targetExtent.height)};
    for (PendingJob& job : validatedJobs)
    {
        job.generation = generation;
        job.targetSize = targetSize;
    }
    m_pendingJobs = std::move(validatedJobs);
    const auto acceptedItemCount = static_cast<std::uint32_t>(m_pendingJobs.size());
    m_activeWindow = ActiveWindow{generation, targetExtent, acceptedItemCount, 0};
    const client::CatalogThumbnailWindowReceipt receipt{generation, acceptedItemCount};
    emit thumbnailWindowStarted({receipt, targetExtent});
    startNextJob();
    return client::CatalogThumbnailWindowResult::success(receipt);
}

// 목적: active thumbnail window와 pending decode를 idempotent하게 정리
// 입력: 없음
// 출력: active generation이 있으면 Cancelled terminal을 발행한 성공 또는 shutdown 오류
client::CatalogThumbnailClearResult CatalogThumbnailOrchestrator::clearThumbnailWindow()
{
    if (!m_acceptingRequests)
    {
        return client::CatalogThumbnailClearResult::failure(
            {client::ClientErrorCode::Conflict, "Catalog thumbnail orchestrator is shutting down."});
    }
    cancelCurrentWindow();
    return client::CatalogThumbnailClearResult::success(std::monostate{});
}

// 목적: event adapter initial delivery에 사용할 current thumbnail lifecycle 조회
// 입력: 없음
// 출력: active generation, target과 요청·settled item count
client::CatalogThumbnailWindowSnapshot CatalogThumbnailOrchestrator::thumbnailWindowSnapshot() const noexcept
{
    if (!m_activeWindow.has_value())
    {
        return {};
    }
    return {m_activeWindow->generation,
            m_activeWindow->targetExtent,
            m_activeWindow->requestedItemCount,
            m_activeWindow->settledItemCount};
}

// 목적: worker slot이 비어 있으면 최신 window의 다음 thumbnail decode 시작
// 입력: 없음
// 출력: active 작업 수가 concurrency 상한 이내로 유지됨
void CatalogThumbnailOrchestrator::startNextJob()
{
    if (!m_acceptingRequests || !m_activeJobs.isEmpty() || m_pendingJobs.isEmpty())
    {
        return;
    }
    if (m_nextRequestId == std::numeric_limits<types::RequestId>::max())
    {
        m_nextRequestId = 1;
    }

    const types::RequestId requestId = m_nextRequestId++;
    PendingJob pending = m_pendingJobs.takeFirst();
    ActiveJob active;
    active.request = pending;
    active.watcher = new QFutureWatcher<CatalogThumbnailPipelineResult>(this);
    const types::CancellationToken token = active.cancellationSource.token();
    QFutureWatcher<CatalogThumbnailPipelineResult>* watcher = active.watcher;
    m_activeJobs.insert(requestId, active);
    connect(watcher, &QFutureWatcher<CatalogThumbnailPipelineResult>::finished, this, [this, requestId] {
        handleJobFinished(requestId);
    });
    watcher->setFuture(QtConcurrent::run(&m_workerPool, [this, pending = std::move(pending), token] {
        return m_pipeline->load(pending.source, pending.targetSize, token);
    }));
}

// 목적: background decode 결과를 tagged identity와 generation 기반 stale filtering 후 event로 변환
// 입력: requestId: 완료된 active thumbnail 작업 identity
// 출력: current window면 frame/issue 하나와 마지막 item의 Completed terminal 발생
void CatalogThumbnailOrchestrator::handleJobFinished(types::RequestId requestId)
{
    auto iterator = m_activeJobs.find(requestId);
    if (iterator == m_activeJobs.end())
    {
        return;
    }

    ActiveJob job = iterator.value();
    m_activeJobs.erase(iterator);
    const CatalogThumbnailPipelineResult result = job.watcher->result();
    job.watcher->deleteLater();
    const bool current = m_activeWindow.has_value() && job.request.generation == m_activeWindow->generation &&
                         !job.cancellationSource.token().isCancellationRequested();
    if (current)
    {
        ++m_activeWindow->settledItemCount;
        if (result.hasValue())
        {
            emit thumbnailReady({job.request.generation, job.request.identity, result.value()});
        }
        else
        {
            emit thumbnailFailed({job.request.generation, job.request.identity, result.error()});
        }
        completeCurrentWindowIfSettled();
    }
    startNextJob();
}

// 목적: current thumbnail window의 pending/active 작업 취소와 exact terminal 확정
// 입력: 없음
// 출력: active window가 없고 stale worker 결과 publish가 차단됨
void CatalogThumbnailOrchestrator::cancelCurrentWindow()
{
    if (!m_activeWindow.has_value())
    {
        m_pendingJobs.clear();
        return;
    }

    const client::CatalogThumbnailWindowGeneration generation = m_activeWindow->generation;
    cancelActiveJobs();
    m_pendingJobs.clear();
    m_activeWindow.reset();
    emit thumbnailWindowTerminal({generation, client::CatalogThumbnailTerminalState::Cancelled, std::nullopt});
}

// 목적: current window의 모든 item이 settled되면 Completed terminal 확정
// 입력: 없음
// 출력: 완료 조건이면 active window가 제거되고 terminal signal 발생
void CatalogThumbnailOrchestrator::completeCurrentWindowIfSettled()
{
    if (!m_activeWindow.has_value() || m_activeWindow->settledItemCount < m_activeWindow->requestedItemCount ||
        !m_pendingJobs.isEmpty() || !m_activeJobs.isEmpty())
    {
        return;
    }

    const client::CatalogThumbnailWindowGeneration generation = m_activeWindow->generation;
    m_activeWindow.reset();
    emit thumbnailWindowTerminal({generation, client::CatalogThumbnailTerminalState::Completed, std::nullopt});
}

// 목적: active decode에 cooperative cancellation 요청
// 입력: 없음
// 출력: 향후 stale frame publish가 차단됨
void CatalogThumbnailOrchestrator::cancelActiveJobs()
{
    for (ActiveJob& job : m_activeJobs)
    {
        job.cancellationSource.requestCancellation();
    }
}

}  // namespace flexraw::core::orchestration
