#include "qt_export_client_adapter.h"

#include <algorithm>
#include <limits>
#include <mutex>
#include <type_traits>
#include <utility>

#include <QByteArray>
#include <QMetaObject>
#include <QString>
#include <QThread>
#include <QVector>

#include "client_error_projection.h"
#include "editor_client_projection.h"
#include "export_contracts.h"
#include "export_orchestrator.h"

namespace flexraw::ui::export_
{
namespace
{

using ProjectedRequestResult =
    core::client::ClientResult<core::orchestration::ExportRequest, core::client::ClientError>;
using ProjectedPlacementResult =
    core::client::ClientResult<core::orchestration::ExportPlacementOptions, core::client::ClientError>;

// 목적: byte length가 보존된 UTF-8 client string을 Qt string으로 변환
// 입력: value: frontend-neutral locator 또는 profile text
// 출력: 같은 Unicode text의 QString
[[nodiscard]] QString fromClientString(const std::string& value)
{
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

// 목적: Qt string을 byte length가 보존된 UTF-8 client string으로 변환
// 입력: value: Orchestrator path 또는 technical text
// 출력: Qt-free UTF-8 string
[[nodiscard]] std::string toClientString(const QString& value)
{
    const QByteArray utf8 = value.toUtf8();
    return {utf8.constData(), static_cast<std::size_t>(utf8.size())};
}

// 목적: fixed-width client count로 음수 internal count 유출 방지
// 입력: value: internal aggregate count
// 출력: 0 이상 uint64 count
[[nodiscard]] std::uint64_t toClientCount(const int value) noexcept
{
    return value <= 0 ? 0 : static_cast<std::uint64_t>(value);
}

// 목적: Qt-free source kind를 current Core file kind로 복원
// 입력: kind: product contract source 분류
// 출력: processing preparation이 사용하는 file kind
[[nodiscard]] core::types::SupportedFileKind fromClientSourceKind(core::client::CatalogFileKind kind) noexcept
{
    switch (kind)
    {
    case core::client::CatalogFileKind::Raw:
        return core::types::SupportedFileKind::Raw;
    case core::client::CatalogFileKind::RasterImage:
        return core::types::SupportedFileKind::RasterImage;
    case core::client::CatalogFileKind::Unknown:
        return core::types::SupportedFileKind::Unknown;
    }
    return core::types::SupportedFileKind::Unknown;
}

// 목적: Qt-free raster option을 current processing-owned option으로 복원
// 입력: options: product contract format/quality/color snapshot
// 출력: 기존 Export validation과 codec path가 사용하는 option
[[nodiscard]] core::export_::RasterExportOptions fromClientOptions(const core::client::ExportRasterOptions& options)
{
    core::export_::RasterExportOptions projected;
    switch (options.format)
    {
    case core::client::ExportRasterFormat::Jpeg:
        projected.format = core::export_::RasterExportFormat::Jpeg;
        break;
    case core::client::ExportRasterFormat::Png:
        projected.format = core::export_::RasterExportFormat::Png;
        break;
    case core::client::ExportRasterFormat::Tiff:
        projected.format = core::export_::RasterExportFormat::Tiff;
        break;
    }
    projected.jpegQuality = options.jpegQuality;
    projected.pngCompression = options.pngCompression;
    projected.tiffCompression = options.tiffCompression == core::client::ExportTiffCompression::None
                                    ? core::export_::TiffCompression::None
                                    : core::export_::TiffCompression::Lzw;
    projected.maximumDimension = options.maximumDimension;
    switch (options.outputColorSpace)
    {
    case core::client::ExportOutputColorSpace::Srgb:
        projected.outputColorSpace = core::export_::RasterOutputColorSpace::Srgb;
        break;
    case core::client::ExportOutputColorSpace::AdobeRgb:
        projected.outputColorSpace = core::export_::RasterOutputColorSpace::AdobeRgb;
        break;
    case core::client::ExportOutputColorSpace::DisplayP3:
        projected.outputColorSpace = core::export_::RasterOutputColorSpace::DisplayP3;
        break;
    }
    projected.includeMetadata = options.includeMetadata;
    return projected;
}

// 목적: product source/develop/output 값을 existing file request로 복원
// 입력: request: Qt-free single-item Export request
// 출력: existing pipeline preparation용 request
[[nodiscard]] core::orchestration::ExportFileRequest fromClientFileRequest(
    const core::client::ExportFileRequest& request)
{
    std::optional<core::types::DevelopParams> developParams;
    if (request.developParams.has_value())
    {
        developParams = core::orchestration::fromClientDevelopParams(*request.developParams);
    }
    return {
        {fromClientString(request.source.path),
         fromClientString(request.source.extension),
         fromClientString(request.source.displayName),
         fromClientSourceKind(request.source.kind)},
        fromClientString(request.outputLocator),
        fromClientString(request.catalogLocator),
        std::move(developParams),
        fromClientOptions(request.options),
        request.useDefaultDevelopParamsWhenCatalogPhotoMissing,
    };
}

// 목적: Qt-free request variant를 existing Orchestrator request variant로 복원
// 입력: request: file, batch 또는 explicit item-list product request
// 출력: projected request 또는 fixed-width 범위 오류
[[nodiscard]] ProjectedRequestResult projectRequest(const core::client::ExportRequest& request)
{
    return std::visit(
        [](const auto& value) -> ProjectedRequestResult {
            using RequestType = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<RequestType, core::client::ExportFileRequest>)
            {
                return ProjectedRequestResult::success(
                    core::orchestration::ExportRequest{fromClientFileRequest(value)});
            }
            else if constexpr (std::is_same_v<RequestType, core::client::ExportBatchRequest>)
            {
                if (value.workerCount > static_cast<std::uint32_t>(std::numeric_limits<int>::max()))
                {
                    return ProjectedRequestResult::failure(
                        {core::client::ClientErrorCode::InvalidArgument, "Export worker count exceeds Core range."});
                }
                std::optional<core::types::DevelopParams> developParams;
                if (value.developParams.has_value())
                {
                    developParams = core::orchestration::fromClientDevelopParams(*value.developParams);
                }
                core::orchestration::ExportBatchRequest projected{
                    fromClientString(value.inputFolderLocator),
                    fromClientString(value.outputFolderLocator),
                    fromClientString(value.catalogLocator),
                    std::move(developParams),
                    static_cast<int>(value.workerCount),
                    fromClientOptions(value.options),
                };
                return ProjectedRequestResult::success(core::orchestration::ExportRequest{std::move(projected)});
            }
            else
            {
                QVector<core::orchestration::ExportFileRequest> items;
                items.reserve(static_cast<qsizetype>(value.items.size()));
                for (const core::client::ExportFileRequest& item : value.items)
                {
                    items.push_back(fromClientFileRequest(item));
                }
                return ProjectedRequestResult::success(
                    core::orchestration::ExportRequest{core::orchestration::ExportItemListRequest{std::move(items)}});
            }
        },
        request);
}

// 목적: profile identity를 현재 endpoint/storage snapshot으로 해석
// 입력: placement: Local/Remote/Auto와 optional stable profile, profileClient: application settings resource
// 출력: existing placement option 또는 RemoteOnly profile 오류
[[nodiscard]] ProjectedPlacementResult projectPlacement(const core::client::ExportPlacementOptions& placement,
                                                        core::client::IWorkerProfileClient& profileClient)
{
    core::orchestration::ExportPlacementOptions projected;
    switch (placement.policy)
    {
    case core::client::ExportPlacementPolicy::LocalOnly:
        projected.policy = core::orchestration::ExportPlacementPolicy::LocalOnly;
        return ProjectedPlacementResult::success(std::move(projected));
    case core::client::ExportPlacementPolicy::RemoteOnly:
        projected.policy = core::orchestration::ExportPlacementPolicy::RemoteOnly;
        break;
    case core::client::ExportPlacementPolicy::Auto:
        projected.policy = core::orchestration::ExportPlacementPolicy::Auto;
        break;
    }

    if (!placement.workerProfileId.has_value() || placement.workerProfileId->value.empty())
    {
        if (placement.policy == core::client::ExportPlacementPolicy::Auto)
        {
            return ProjectedPlacementResult::success(std::move(projected));
        }
        return ProjectedPlacementResult::failure(
            {core::client::ClientErrorCode::InvalidArgument, "Remote export requires a Worker profile identity."});
    }

    const core::client::WorkerProfileListResult profiles = profileClient.listWorkerProfiles();
    if (profiles.hasError())
    {
        if (placement.policy == core::client::ExportPlacementPolicy::Auto)
        {
            return ProjectedPlacementResult::success(std::move(projected));
        }
        return ProjectedPlacementResult::failure(profiles.error());
    }
    const auto profile =
        std::ranges::find(profiles.value(), *placement.workerProfileId, &core::client::WorkerProfileSnapshot::id);
    if (profile == profiles.value().end())
    {
        if (placement.policy == core::client::ExportPlacementPolicy::Auto)
        {
            return ProjectedPlacementResult::success(std::move(projected));
        }
        return ProjectedPlacementResult::failure(
            {core::client::ClientErrorCode::NotFound, "The selected Worker profile does not exist."});
    }
    if (!profile->enabled)
    {
        if (placement.policy == core::client::ExportPlacementPolicy::Auto)
        {
            return ProjectedPlacementResult::success(std::move(projected));
        }
        return ProjectedPlacementResult::failure(
            {core::client::ClientErrorCode::InvalidArgument, "The selected Worker profile is disabled."});
    }

    projected.remoteTarget = core::orchestration::ExportRemoteTarget{
        fromClientString(profile->host),
        profile->port,
        fromClientString(profile->expectedSourceStorageId),
        fromClientString(profile->expectedOutputStorageId),
    };
    return ProjectedPlacementResult::success(std::move(projected));
}

// 목적: internal item failure kind를 stable product domain failure로 투영
// 입력: kind: scheduling/execution result classification
// 출력: frontend-neutral Export failure kind
[[nodiscard]] core::client::ExportFailureKind toClientFailureKind(
    core::orchestration::ExportItemFailureKind kind) noexcept
{
    switch (kind)
    {
    case core::orchestration::ExportItemFailureKind::None:
        return core::client::ExportFailureKind::None;
    case core::orchestration::ExportItemFailureKind::Eligibility:
        return core::client::ExportFailureKind::Eligibility;
    case core::orchestration::ExportItemFailureKind::DispatchExhausted:
        return core::client::ExportFailureKind::DispatchExhausted;
    case core::orchestration::ExportItemFailureKind::Execution:
        return core::client::ExportFailureKind::Execution;
    case core::orchestration::ExportItemFailureKind::Ambiguous:
        return core::client::ExportFailureKind::Ambiguous;
    }
    return core::client::ExportFailureKind::Execution;
}

// 목적: existing scheduling counter를 frontend-neutral snapshot으로 복사
// 입력: metrics: request 또는 lifetime scheduling counter
// 출력: fixed-width product metric snapshot
[[nodiscard]] core::client::ExportSchedulingMetrics toClientMetrics(
    const core::orchestration::ExportSchedulingMetrics& metrics) noexcept
{
    return {metrics.submitted,
            metrics.succeeded,
            metrics.failed,
            metrics.cancelled,
            metrics.ambiguous,
            metrics.localDispatchAttempts,
            metrics.remoteDispatchAttempts,
            metrics.serverBusyCount,
            metrics.resourceBusyCount,
            metrics.connectionFailedCount,
            metrics.localExecuted,
            metrics.remoteExecuted};
}

// 목적: item terminal 결과를 locator/domain failure가 보존된 Qt-free 값으로 투영
// 입력: item: existing Export item result
// 출력: optional common error와 domain failure kind를 가진 product result
[[nodiscard]] core::client::ExportItemResult toClientItemResult(const core::orchestration::ExportItemResult& item)
{
    std::optional<core::client::ClientError> error;
    if (!item.succeeded)
    {
        error = core::orchestration::toClientError(item.error);
    }
    return {toClientString(item.sourcePath),
            toClientString(item.outputPath),
            item.succeeded,
            std::move(error),
            toClientFailureKind(item.failureKind)};
}

// 목적: aggregate report를 fixed-width counts와 independent item storage로 투영
// 입력: report: existing completed request report
// 출력: Qt-free immutable report
[[nodiscard]] core::client::ExportReport toClientReport(const core::orchestration::ExportReport& report)
{
    core::client::ExportReport projected;
    projected.totalCount = toClientCount(report.totalCount);
    projected.succeededCount = toClientCount(report.succeededCount);
    projected.failedCount = toClientCount(report.failedCount);
    projected.items.reserve(static_cast<std::size_t>(report.items.size()));
    for (const core::orchestration::ExportItemResult& item : report.items)
    {
        projected.items.push_back(toClientItemResult(item));
    }
    projected.scheduling = toClientMetrics(report.scheduling);
    return projected;
}

}  // namespace

struct QtExportClientAdapter::SubscriptionState
{
    // 목적: queued event와 이후 callback 전달을 idempotent하게 차단
    // 입력: 없음
    // 출력: inactive state와 해제된 callback
    void unsubscribe() noexcept
    {
        const std::scoped_lock lock(mutex);
        active = false;
        callback = {};
    }

    // 목적: subscription callback 전달 가능 상태를 thread-safe하게 조회
    // 입력: 없음
    // 출력: active이면 true
    [[nodiscard]] bool isActive() const noexcept
    {
        const std::scoped_lock lock(mutex);
        return active;
    }

    mutable std::recursive_mutex mutex;
    bool active{true};
    core::client::ExportCallback callback;
};

class QtExportClientAdapter::Subscription final : public core::client::IExportSubscription
{
public:
    // 목적: public handle과 shared Export delivery state 연결
    // 입력: state: adapter queued callback과 공유할 lifetime state
    // 출력: active RAII subscription handle
    explicit Subscription(SubscriptionStatePtr state) : m_state(std::move(state)) {}

    // 목적: 마지막 handle 해제 시 queued event와 이후 callback 전달 차단
    // 입력: 없음
    // 출력: inactive subscription
    ~Subscription() override
    {
        unsubscribe();
    }

    // 목적: queued event와 이후 Export callback 전달을 idempotent하게 차단
    // 입력: 없음
    // 출력: 없음
    void unsubscribe() noexcept override
    {
        m_state->unsubscribe();
    }

    // 목적: subscription이 이후 callback을 받을 수 있는지 조회
    // 입력: 없음
    // 출력: callback 전달이 허용된 상태이면 true
    [[nodiscard]] bool isActive() const noexcept override
    {
        return m_state->isActive();
    }

private:
    SubscriptionStatePtr m_state;
};

// 목적: application-scoped Export owner와 Worker profile resource를 Qt-free product surface에 연결
// 입력: orchestrator: scheduling/terminal authority, workerProfileClient: endpoint resolution source, parent: Qt owner
// 출력: current Qt delivery context에 bound된 Export adapter
QtExportClientAdapter::QtExportClientAdapter(core::orchestration::ExportOrchestrator& orchestrator,
                                             core::client::IWorkerProfileClient& workerProfileClient,
                                             QObject* parent)
    : QObject(parent), m_orchestrator(&orchestrator), m_workerProfileClient(&workerProfileClient)
{
    Q_ASSERT(m_orchestrator->thread() == thread());
    m_snapshot.scheduling = toClientMetrics(m_orchestrator->schedulingMetrics());
    connect(m_orchestrator,
            &core::orchestration::ExportOrchestrator::exportAccepted,
            this,
            &QtExportClientAdapter::recordAccepted,
            Qt::DirectConnection);
    connect(m_orchestrator,
            &core::orchestration::ExportOrchestrator::exportProgressed,
            this,
            &QtExportClientAdapter::recordProgress,
            Qt::DirectConnection);
    connect(m_orchestrator,
            &core::orchestration::ExportOrchestrator::exportCompleted,
            this,
            &QtExportClientAdapter::recordCompleted,
            Qt::DirectConnection);
    connect(m_orchestrator,
            &core::orchestration::ExportOrchestrator::exportFailed,
            this,
            &QtExportClientAdapter::recordFailed,
            Qt::DirectConnection);
    connect(m_orchestrator,
            &core::orchestration::ExportOrchestrator::exportCancelled,
            this,
            &QtExportClientAdapter::recordCancelled,
            Qt::DirectConnection);
}

// 목적: queued callback을 차단하고 outliving subscription handle을 inactive 상태로 전환
// 입력: 없음
// 출력: adapter destruction 이후 callback 없음
QtExportClientAdapter::~QtExportClientAdapter()
{
    m_shuttingDown = true;
    for (const std::weak_ptr<SubscriptionState>& weakState : m_subscriptions)
    {
        if (const SubscriptionStatePtr state = weakState.lock(); state != nullptr)
        {
            state->unsubscribe();
        }
    }
    m_subscriptions.clear();
}

// 목적: Qt-free Export command를 현재 Orchestrator request와 resolved Worker target으로 투영
// 입력: command: file·item-list·batch request와 placement/profile identity
// 출력: accepted request receipt 또는 validation·profile·owner 오류
core::client::ExportSubmissionResult QtExportClientAdapter::submitExport(
    const core::client::SubmitExportCommand& command)
{
    if (QThread::currentThread() != thread())
    {
        return core::client::ExportSubmissionResult::failure(
            {core::client::ClientErrorCode::Conflict, "Export command must run on its adapter delivery context."});
    }
    if (m_shuttingDown)
    {
        return core::client::ExportSubmissionResult::failure(
            {core::client::ClientErrorCode::Conflict, "Export adapter is shutting down."});
    }

    ProjectedRequestResult request = projectRequest(command.request);
    if (request.hasError())
    {
        return core::client::ExportSubmissionResult::failure(request.error());
    }
    ProjectedPlacementResult placement = projectPlacement(command.placement, *m_workerProfileClient);
    if (placement.hasError())
    {
        return core::client::ExportSubmissionResult::failure(placement.error());
    }
    const core::orchestration::ExportSubmissionResult submitted =
        m_orchestrator->submitExport(request.value(), placement.value());
    if (submitted.hasError())
    {
        return core::client::ExportSubmissionResult::failure(core::orchestration::toClientError(submitted.error()));
    }
    return core::client::ExportSubmissionResult::success({{submitted.value()}});
}

// 목적: accepted Export request cancellation을 authoritative owner에 전달
// 입력: requestId: Qt-free Export request identity
// 출력: 취소 수락 identity 또는 stale·validation 오류
core::client::ExportCancellationResult QtExportClientAdapter::cancelExport(
    const core::client::ExportRequestId requestId)
{
    if (requestId.value == 0)
    {
        return core::client::ExportCancellationResult::failure(
            {core::client::ClientErrorCode::InvalidArgument, "Export request identity must not be zero."});
    }
    if (QThread::currentThread() != thread())
    {
        return core::client::ExportCancellationResult::failure(
            {core::client::ClientErrorCode::Conflict, "Export cancellation must run on its adapter delivery context."});
    }
    if (!m_orchestrator->cancelExport(requestId.value))
    {
        return core::client::ExportCancellationResult::failure(
            {core::client::ClientErrorCode::NotFound, "The Export request is no longer active."});
    }
    return core::client::ExportCancellationResult::success(requestId);
}

// 목적: 전체 또는 지정 active Export request와 lifetime scheduling metric 조회
// 입력: requestId: optional active request identity
// 출력: immutable snapshot 또는 stale·thread 오류
core::client::ExportSnapshotResult QtExportClientAdapter::exportSnapshot(
    const std::optional<core::client::ExportRequestId> requestId) const
{
    if (QThread::currentThread() != thread())
    {
        return core::client::ExportSnapshotResult::failure(
            {core::client::ClientErrorCode::Conflict, "Export snapshot must be read on its adapter delivery context."});
    }
    if (!requestId.has_value())
    {
        return core::client::ExportSnapshotResult::success(m_snapshot);
    }
    if (requestId->value == 0)
    {
        return core::client::ExportSnapshotResult::failure(
            {core::client::ClientErrorCode::InvalidArgument, "Export request identity must not be zero."});
    }
    const auto active =
        std::ranges::find(m_snapshot.activeRequests, *requestId, &core::client::ActiveExportSnapshot::requestId);
    if (active == m_snapshot.activeRequests.end())
    {
        return core::client::ExportSnapshotResult::failure(
            {core::client::ClientErrorCode::NotFound, "The Export request is no longer active."});
    }
    core::client::ExportSnapshot selected;
    selected.activeRequests.push_back(*active);
    selected.scheduling = m_snapshot.scheduling;
    return core::client::ExportSnapshotResult::success(std::move(selected));
}

// 목적: initial active snapshot과 이후 accepted·progress·terminal lifecycle 구독
// 입력: callback: immutable Export event consumer
// 출력: RAII unsubscribe handle 또는 callback·thread 오류
core::client::ExportSubscriptionResult QtExportClientAdapter::subscribeToExports(core::client::ExportCallback callback)
{
    if (!callback)
    {
        return core::client::ExportSubscriptionResult::failure(
            {core::client::ClientErrorCode::InvalidArgument, "Export callback must not be empty."});
    }
    if (QThread::currentThread() != thread())
    {
        return core::client::ExportSubscriptionResult::failure(
            {core::client::ClientErrorCode::Conflict,
             "Export subscription must be created on its adapter delivery context."});
    }
    if (m_shuttingDown)
    {
        return core::client::ExportSubscriptionResult::failure(
            {core::client::ClientErrorCode::Conflict, "Export event adapter is shutting down."});
    }

    const SubscriptionStatePtr state = std::make_shared<SubscriptionState>();
    state->callback = std::move(callback);
    core::client::ExportSubscriptionHandle handle = std::make_shared<Subscription>(state);
    std::erase_if(m_subscriptions, [](const std::weak_ptr<SubscriptionState>& weakState) {
        const SubscriptionStatePtr existing = weakState.lock();
        return existing == nullptr || !existing->isActive();
    });
    m_subscriptions.emplace_back(state);
    const core::client::ExportEvent initialEvent{
        nextEventSequence(), true, m_snapshot, std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt};
    if (!enqueueEvent(state, initialEvent))
    {
        state->unsubscribe();
        return core::client::ExportSubscriptionResult::failure(
            {core::client::ClientErrorCode::Unknown, "Unable to queue the initial Export event."});
    }
    return core::client::ExportSubscriptionResult::success(std::move(handle));
}

// 목적: adapter lifetime에서 0을 사용하지 않는 event ordering sequence 발급
// 입력: 없음
// 출력: 다음 Export event sequence
core::client::ExportEventSequence QtExportClientAdapter::nextEventSequence() noexcept
{
    const std::uint64_t value = m_nextEventSequence;
    m_nextEventSequence = value == std::numeric_limits<std::uint64_t>::max() ? 1 : value + 1;
    return {value};
}

// 목적: owner가 accepted한 request를 active snapshot과 event로 투영
// 입력: requestId: authoritative aggregate identity
// 출력: active request 등록과 accepted event
void QtExportClientAdapter::recordAccepted(const std::uint64_t requestId)
{
    const core::client::ExportRequestId id{requestId};
    if (findActive(id) == nullptr)
    {
        m_snapshot.activeRequests.push_back({id});
    }
    publishEvent(core::client::ExportRequestReceipt{id}, std::nullopt, std::nullopt, std::nullopt, std::nullopt);
}

// 목적: owner 누적 progress를 active snapshot과 event로 투영
// 입력: progress: request identity와 item 누적 상태
// 출력: matching active request 갱신과 progress event
void QtExportClientAdapter::recordProgress(const core::orchestration::ExportProgress& progress)
{
    const core::client::ExportRequestId id{progress.requestId};
    core::client::ActiveExportSnapshot* active = findActive(id);
    if (active == nullptr)
    {
        m_snapshot.activeRequests.push_back({id});
        active = &m_snapshot.activeRequests.back();
    }
    active->completedCount = toClientCount(progress.completedCount);
    active->totalCount = toClientCount(progress.totalCount);
    active->succeededCount = toClientCount(progress.succeededCount);
    active->failedCount = toClientCount(progress.failedCount);
    publishEvent(std::nullopt,
                 core::client::ExportProgress{id,
                                              active->completedCount,
                                              active->totalCount,
                                              active->succeededCount,
                                              active->failedCount,
                                              toClientString(progress.currentSourcePath)},
                 std::nullopt,
                 std::nullopt,
                 std::nullopt);
}

// 목적: owner completed report를 Qt-free exact terminal event로 투영
// 입력: result: item report와 request scheduling metric
// 출력: active request 제거와 completed event
void QtExportClientAdapter::recordCompleted(const core::orchestration::ExportResult& result)
{
    const core::client::ExportRequestId id{result.requestId};
    removeActive(id);
    publishEvent(std::nullopt,
                 std::nullopt,
                 core::client::ExportResult{id, toClientReport(result.report)},
                 std::nullopt,
                 std::nullopt);
}

// 목적: owner request-level failure를 ClientError exact terminal로 투영
// 입력: issue: request identity와 CoreError
// 출력: active request 제거와 failed event
void QtExportClientAdapter::recordFailed(const core::orchestration::ExportIssue& issue)
{
    const core::client::ExportRequestId id{issue.requestId};
    removeActive(id);
    publishEvent(std::nullopt,
                 std::nullopt,
                 std::nullopt,
                 core::client::ExportIssue{id, core::orchestration::toClientError(issue.error)},
                 std::nullopt);
}

// 목적: owner cancellation을 Qt-free exact terminal event로 투영
// 입력: requestId: cancelled aggregate identity
// 출력: active request 제거와 cancelled event
void QtExportClientAdapter::recordCancelled(const std::uint64_t requestId)
{
    const core::client::ExportRequestId id{requestId};
    removeActive(id);
    publishEvent(std::nullopt, std::nullopt, std::nullopt, std::nullopt, core::client::ExportCancellation{id});
}

// 목적: active request vector에서 identity가 일치하는 mutable snapshot 조회
// 입력: requestId: 찾을 aggregate identity
// 출력: matching snapshot pointer 또는 nullptr
core::client::ActiveExportSnapshot* QtExportClientAdapter::findActive(
    const core::client::ExportRequestId requestId) noexcept
{
    const auto active =
        std::ranges::find(m_snapshot.activeRequests, requestId, &core::client::ActiveExportSnapshot::requestId);
    return active == m_snapshot.activeRequests.end() ? nullptr : &*active;
}

// 목적: terminal request를 active snapshot에서 제거
// 입력: requestId: 제거할 aggregate identity
// 출력: snapshot에 해당 identity가 남지 않음
void QtExportClientAdapter::removeActive(const core::client::ExportRequestId requestId)
{
    std::erase_if(m_snapshot.activeRequests, [requestId](const core::client::ActiveExportSnapshot& active) {
        return active.requestId == requestId;
    });
}

// 목적: current snapshot과 transition payload를 모든 active subscription에 fan-out
// 입력: accepted/progress/completed/failed/cancelled: 이번 lifecycle transition payload
// 출력: subscription별 Qt queued callback 등록
void QtExportClientAdapter::publishEvent(std::optional<core::client::ExportRequestReceipt> accepted,
                                         std::optional<core::client::ExportProgress> progress,
                                         std::optional<core::client::ExportResult> completed,
                                         std::optional<core::client::ExportIssue> failed,
                                         std::optional<core::client::ExportCancellation> cancelled)
{
    if (m_shuttingDown)
    {
        return;
    }
    m_snapshot.scheduling = toClientMetrics(m_orchestrator->schedulingMetrics());
    const core::client::ExportEvent event{nextEventSequence(),
                                          false,
                                          m_snapshot,
                                          std::move(accepted),
                                          std::move(progress),
                                          std::move(completed),
                                          std::move(failed),
                                          std::move(cancelled)};
    auto subscription = m_subscriptions.begin();
    while (subscription != m_subscriptions.end())
    {
        const SubscriptionStatePtr state = subscription->lock();
        if (state == nullptr || !state->isActive())
        {
            subscription = m_subscriptions.erase(subscription);
            continue;
        }
        if (!enqueueEvent(state, event))
        {
            state->unsubscribe();
        }
        ++subscription;
    }
}

// 목적: 한 subscription에 immutable event를 Qt queued callback으로 등록
// 입력: state: subscription lifetime, event: 전달할 Export event
// 출력: 전달 불필요 또는 queue 성공이면 true
bool QtExportClientAdapter::enqueueEvent(const SubscriptionStatePtr& state, core::client::ExportEvent event)
{
    if (!state->isActive())
    {
        return true;
    }
    return QMetaObject::invokeMethod(
        this, [state, event = std::move(event)] { deliverEvent(state, event); }, Qt::QueuedConnection);
}

// 목적: queued immutable event를 active callback 하나에 예외 격리하여 전달
// 입력: state: subscription lifetime, event: 전달할 Export event
// 출력: 없음
void QtExportClientAdapter::deliverEvent(const SubscriptionStatePtr& state,
                                         const core::client::ExportEvent& event) noexcept
{
    try
    {
        const std::scoped_lock lock(state->mutex);
        if (!state->active)
        {
            return;
        }
        const core::client::ExportCallback callback = state->callback;
        callback(event);
    }
    catch (...)
    {
        // Consumer 예외는 Qt event loop와 다른 subscription delivery로 전파하지 않는다.
    }
}

}  // namespace flexraw::ui::export_
