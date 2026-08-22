#include "export_orchestrator.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include <QDir>
#include <QMetaObject>
#include <QMetaType>
#include <QScopedValueRollback>
#include <QSet>
#include <QThread>

#include "develop.h"
#include "export.h"
#include "log.h"
#include "system_memory_probe.h"

namespace flexraw::core::orchestration
{
namespace
{

constexpr int DefaultMaximumConcurrentExportJobs = 2;
constexpr int ResourceRecheckMilliseconds = 1000;

// 목적: file export request의 project-owned value contract 검증
// 입력: request: 검증할 단일 file 요청
// 출력: 유효하면 빈 error, 유효하지 않으면 구조화된 error
[[nodiscard]] std::optional<types::CoreError> validateFileRequest(const ExportFileRequest& request)
{
    if (request.source.path.trimmed().isEmpty() || request.outputPath.trimmed().isEmpty())
    {
        return types::CoreError{
            types::ErrorCode::InvalidArgument,
            QStringLiteral("Export source and output paths must not be empty."),
        };
    }
    if (request.source.kind != types::SupportedFileKind::Raw &&
        request.source.kind != types::SupportedFileKind::RasterImage)
    {
        return types::CoreError{
            types::ErrorCode::UnsupportedFormat,
            QStringLiteral("Export source kind is unsupported."),
        };
    }
    if (request.source.kind == types::SupportedFileKind::RasterImage && !request.catalogPath.isEmpty())
    {
        return types::CoreError{
            types::ErrorCode::InvalidArgument,
            QStringLiteral("A catalog can only supply develop state for RAW export."),
        };
    }

    const export_::RasterExportResult optionsValidation = export_::validateRasterExportOptions(request.options);
    if (optionsValidation.hasError())
    {
        return optionsValidation.error();
    }
    if (request.developParams.has_value())
    {
        const develop::DevelopParamsValidationResult validation =
            develop::validateDevelopParams(*request.developParams);
        if (validation.hasError())
        {
            return validation.error();
        }
    }
    return std::nullopt;
}

// 목적: batch export request의 project-owned value contract 검증
// 입력: request: 검증할 folder batch 요청
// 출력: 유효하면 빈 error, 유효하지 않으면 구조화된 error
[[nodiscard]] std::optional<types::CoreError> validateBatchRequest(const ExportBatchRequest& request)
{
    if (request.inputFolderPath.trimmed().isEmpty() || request.outputFolderPath.trimmed().isEmpty())
    {
        return types::CoreError{
            types::ErrorCode::InvalidArgument,
            QStringLiteral("Batch export input and output folders must not be empty."),
        };
    }
    if (request.workerCount < 0)
    {
        return types::CoreError{
            types::ErrorCode::InvalidArgument,
            QStringLiteral("Batch export worker count must not be negative."),
        };
    }

    const export_::RasterExportResult optionsValidation = export_::validateRasterExportOptions(request.options);
    if (optionsValidation.hasError())
    {
        return optionsValidation.error();
    }
    if (request.developParams.has_value())
    {
        const develop::DevelopParamsValidationResult validation =
            develop::validateDevelopParams(*request.developParams);
        if (validation.hasError())
        {
            return validation.error();
        }
    }
    return std::nullopt;
}

// 목적: 명시적 file item 목록의 비어 있지 않은 구성과 각 item contract 검증
// 입력: request: GUI selection 등에서 조립한 file export item 목록
// 출력: 유효하면 빈 error, 첫 invalid item이 있으면 구조화된 error
[[nodiscard]] std::optional<types::CoreError> validateItemListRequest(const ExportItemListRequest& request)
{
    if (request.items.isEmpty())
    {
        return types::CoreError{
            types::ErrorCode::InvalidArgument,
            QStringLiteral("Export item list must not be empty."),
        };
    }

    QSet<QString> outputPaths;
    for (const ExportFileRequest& item : request.items)
    {
        if (const std::optional<types::CoreError> error = validateFileRequest(item); error.has_value())
        {
            return error;
        }
        const QString outputPath = QDir::cleanPath(item.outputPath).toCaseFolded();
        if (outputPaths.contains(outputPath))
        {
            return types::CoreError{
                types::ErrorCode::Conflict,
                QStringLiteral("Export item output paths must be unique."),
            };
        }
        outputPaths.insert(outputPath);
    }
    return std::nullopt;
}

// 목적: export request variant를 concrete request validation에 전달
// 입력: request: file 또는 batch export 요청
// 출력: 유효하면 빈 error, 유효하지 않으면 구조화된 error
[[nodiscard]] std::optional<types::CoreError> validateRequest(const ExportRequest& request)
{
    return std::visit(
        [](const auto& typedRequest) -> std::optional<types::CoreError> {
            using RequestType = std::decay_t<decltype(typedRequest)>;
            if constexpr (std::is_same_v<RequestType, ExportFileRequest>)
            {
                return validateFileRequest(typedRequest);
            }
            else if constexpr (std::is_same_v<RequestType, ExportBatchRequest>)
            {
                return validateBatchRequest(typedRequest);
            }
            else
            {
                return validateItemListRequest(typedRequest);
            }
        },
        request);
}

// 목적: manual Remote target의 최소 연결 identity 유효성 검사
// 입력: target: host/port와 storage profile 값
// 출력: network dispatch 후보가 될 수 있으면 true
[[nodiscard]] bool isUsableRemoteTarget(const ExportRemoteTarget& target)
{
    return !target.host.trimmed().isEmpty() && target.port != 0;
}

// 목적: placement option의 즉시 검증 가능한 RemoteOnly 요구사항 확인
// 입력: placement: policy와 optional manual Remote target, hasRemotePort: adapter 주입 여부
// 출력: 유효하면 빈 error, RemoteOnly dependency/target 오류면 구조화된 error
[[nodiscard]] std::optional<types::CoreError> validatePlacement(const ExportPlacementOptions& placement,
                                                                const bool hasRemotePort)
{
    if (placement.policy == ExportPlacementPolicy::RemoteOnly &&
        (!hasRemotePort || !placement.remoteTarget.has_value()))
    {
        return types::CoreError{types::ErrorCode::InvalidArgument,
                                QStringLiteral("Remote-only export requires a Remote execution target.")};
    }
    if (placement.policy == ExportPlacementPolicy::RemoteOnly && placement.remoteTarget.has_value() &&
        !isUsableRemoteTarget(*placement.remoteTarget))
    {
        return types::CoreError{types::ErrorCode::InvalidArgument,
                                QStringLiteral("Remote export target host and port are invalid.")};
    }
    return std::nullopt;
}

// 목적: application-wide Local export slot 수의 안정적인 compatibility 기본값 계산
// 입력: requestedCount: caller가 지정한 값, 0 이하면 자동값
// 출력: 하나 이상의 Local worker 수
[[nodiscard]] int resolveMaximumConcurrentJobs(const int requestedCount)
{
    if (requestedCount > 0)
    {
        return requestedCount;
    }
    return std::max(1, std::min(DefaultMaximumConcurrentExportJobs, QThread::idealThreadCount()));
}

// 목적: legacy constructor 값을 resource check 없는 Local-only scheduler 설정으로 변환
// 입력: requestedCount: caller 지정 또는 자동 Local slot 수
// 출력: 기존 동시성 동작을 보존하는 설정
[[nodiscard]] ExportSchedulingConfiguration makeLegacyConfiguration(const int requestedCount)
{
    ExportSchedulingConfiguration configuration;
    configuration.localSlotLimit = resolveMaximumConcurrentJobs(requestedCount);
    configuration.remoteSlotLimit = 1;
    configuration.enforceLocalResourceReserve = false;
    return configuration;
}

// 목적: scheduler configuration의 bounded positive invariant 확인
// 입력: configuration: slot/retry/resource/cooldown 값
// 출력: 유효하지 않으면 invalid_argument throw
void validateConfiguration(const ExportSchedulingConfiguration& configuration)
{
    if (configuration.localSlotLimit <= 0 || configuration.remoteSlotLimit <= 0 ||
        configuration.maximumRemoteDispatchAttempts <= 0 || configuration.reservedLogicalProcessors < 0 ||
        configuration.memoryClaimPerJobBytes == 0 || configuration.serverBusyCooldown.count() < 0 ||
        configuration.resourceBusyCooldown.count() < 0 || configuration.connectionFailedCooldown.count() < 0)
    {
        throw std::invalid_argument("Export scheduling configuration is invalid.");
    }
}

// 목적: worker 경계 밖으로 전달된 C++ exception을 CoreError로 변환
// 입력: message: exception 상세 또는 fallback 설명
// 출력: terminal unknown error
[[nodiscard]] types::CoreError makeUnhandledError(QString message)
{
    return {types::ErrorCode::Unknown, std::move(message)};
}

// 목적: terminal item failure를 source/output identity와 함께 생성
// 입력: item: immutable export intent, error: technical cause, kind: placement failure 의미
// 출력: failed ExportItemResult
[[nodiscard]] ExportItemResult makeFailedItem(const PreparedExportItem& item,
                                              types::CoreError error,
                                              const ExportItemFailureKind kind)
{
    return {item.request.source.path, item.request.outputPath, false, std::move(error), kind};
}

// 목적: batch request가 지정한 per-request Local concurrency cap 추출
// 입력: request: file 또는 batch export request
// 출력: 0이면 global cap 사용, 양수이면 batch worker cap
[[nodiscard]] int requestedLocalLimit(const ExportRequest& request)
{
    if (const auto* batch = std::get_if<ExportBatchRequest>(&request))
    {
        return batch->workerCount;
    }
    return 0;
}

// 목적: steady-clock duration을 QTimer가 받는 bounded millisecond로 변환
// 입력: duration: future deadline까지 남은 구간
// 출력: 1 이상 int max 이하 millisecond
[[nodiscard]] int boundedTimerMilliseconds(const std::chrono::steady_clock::duration duration)
{
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(duration);
    const auto rounded = milliseconds.count() + (milliseconds < duration ? 1 : 0);
    return static_cast<int>(std::clamp<std::int64_t>(rounded, 1, std::numeric_limits<int>::max()));
}

}  // namespace

// 목적: Local-only compatibility용 application-scoped export scheduler 조립
// 입력: pipeline: thread-safe pipeline, maximumConcurrentJobs: Local slot 수, parent: Qt parent 객체
// 출력: 기존 caller와 호환되는 ExportOrchestrator 객체
ExportOrchestrator::ExportOrchestrator(std::unique_ptr<IExportPipeline> pipeline,
                                       const int maximumConcurrentJobs,
                                       QObject* parent)
    : ExportOrchestrator(std::move(pipeline), {}, nullptr, makeLegacyConfiguration(maximumConcurrentJobs), parent)
{}

// 목적: Local/Remote execution port와 resource-aware scheduling policy 조립
// 입력: pipeline/remotePort/memoryProbe: injected dependency, configuration: slot/retry/reserve 정책
// 출력: LocalOnly/RemoteOnly/Auto 실행이 가능한 ExportOrchestrator 객체
ExportOrchestrator::ExportOrchestrator(std::unique_ptr<IExportPipeline> pipeline,
                                       std::unique_ptr<IRemoteExportExecutionPort> remotePort,
                                       const platform::ISystemMemoryProbe* memoryProbe,
                                       ExportSchedulingConfiguration configuration,
                                       QObject* parent)
    : QObject(parent),
      m_pipeline(std::move(pipeline)),
      m_remotePort(std::move(remotePort)),
      m_memoryProbe(memoryProbe),
      m_configuration(std::move(configuration))
{
    if (m_pipeline == nullptr)
    {
        throw std::invalid_argument("Export pipeline must not be null.");
    }
    validateConfiguration(m_configuration);

    qRegisterMetaType<ExportResult>();
    qRegisterMetaType<ExportProgress>();
    qRegisterMetaType<ExportIssue>();
    m_preparationPool.setObjectName(QStringLiteral("ExportPreparationPool"));
    m_preparationPool.setMaxThreadCount(1);
    m_localPool.setObjectName(QStringLiteral("ExportLocalPool"));
    m_localPool.setMaxThreadCount(m_configuration.localSlotLimit);
    m_remotePool.setObjectName(QStringLiteral("ExportRemotePool"));
    m_remotePool.setMaxThreadCount(m_configuration.remoteSlotLimit);
    m_cooldownTimer.setSingleShot(true);
    m_resourceTimer.setSingleShot(true);
    connect(&m_cooldownTimer, &QTimer::timeout, this, &ExportOrchestrator::pump);
    connect(&m_resourceTimer, &QTimer::timeout, this, &ExportOrchestrator::pump);
}

// 목적: 새 request를 차단하고 active job을 취소한 뒤 모든 worker pool 종료 대기
// 입력: 없음
// 출력: 없음
ExportOrchestrator::~ExportOrchestrator()
{
    m_acceptingRequests = false;
    m_cooldownTimer.stop();
    m_resourceTimer.stop();
    for (const ActiveExportRequest& request : std::as_const(m_activeRequests))
    {
        request.cancellationSource.requestCancellation();
    }
    m_preparationPool.waitForDone();
    m_localPool.waitForDone();
    m_remotePool.waitForDone();
    m_jobs.clear();
    m_activeRequests.clear();
}

// 목적: export request를 검증하고 placement별 item scheduling lifecycle 생성
// 입력: request: file 또는 batch 요청, placement: LocalOnly/RemoteOnly/Auto와 optional target
// 출력: 수락된 RequestId 또는 즉시 validation 오류
ExportSubmissionResult ExportOrchestrator::submitExport(ExportRequest request, ExportPlacementOptions placement)
{
    if (!m_acceptingRequests)
    {
        return ExportSubmissionResult::failure(
            {types::ErrorCode::Unknown, QStringLiteral("Export orchestrator is shutting down.")});
    }
    if (const std::optional<types::CoreError> error = validateRequest(request); error.has_value())
    {
        return ExportSubmissionResult::failure(*error);
    }
    if (const std::optional<types::CoreError> error = validatePlacement(placement, m_remotePort != nullptr);
        error.has_value())
    {
        return ExportSubmissionResult::failure(*error);
    }
    if (placement.policy == ExportPlacementPolicy::Auto &&
        (m_remotePort == nullptr || !placement.remoteTarget.has_value() ||
         !isUsableRemoteTarget(*placement.remoteTarget)))
    {
        placement.remoteTarget.reset();
    }
    if (m_nextRequestId == std::numeric_limits<types::RequestId>::max())
    {
        return ExportSubmissionResult::failure(
            {types::ErrorCode::Unknown, QStringLiteral("Export request ID space is exhausted.")});
    }

    const types::RequestId requestId = m_nextRequestId++;
    ActiveExportRequest active;
    active.placement = std::move(placement);
    active.requestedLocalLimit = requestedLocalLimit(request);
    m_activeRequests.insert(requestId, std::move(active));
    startPreparation(requestId, std::move(request));
    return ExportSubmissionResult::success(requestId);
}

// 목적: request preparation을 전용 worker에서 시작
// 입력: requestId: aggregate identity, request: immutable file 또는 batch 값
// 출력: queued preparation task
void ExportOrchestrator::startPreparation(const types::RequestId requestId, ExportRequest request)
{
    const auto active = m_activeRequests.constFind(requestId);
    if (active == m_activeRequests.cend())
    {
        return;
    }
    const types::CancellationToken cancellationToken = active->cancellationSource.token();
    IExportPipeline* const pipeline = m_pipeline.get();
    m_preparationPool.start([this, pipeline, requestId, request = std::move(request), cancellationToken]() mutable {
        ExportPreparationResult result = ExportPreparationResult::failure(
            makeUnhandledError(QStringLiteral("Export preparation did not produce a result.")));
        try
        {
            result = pipeline->prepare(request, cancellationToken);
        }
        catch (const std::exception& error)
        {
            result = ExportPreparationResult::failure(
                makeUnhandledError(QStringLiteral("Unhandled export preparation exception: %1").arg(error.what())));
        }
        catch (...)
        {
            result = ExportPreparationResult::failure(
                makeUnhandledError(QStringLiteral("Unhandled non-standard export preparation exception.")));
        }
        (void)QMetaObject::invokeMethod(
            this,
            [this, requestId, result = std::move(result)]() mutable { handlePrepared(requestId, std::move(result)); },
            Qt::QueuedConnection);
    });
}

// 목적: preparation 결과를 item queue 또는 request terminal event로 변환
// 입력: requestId: aggregate identity, result: resolved item 목록 또는 준비 오류
// 출력: pending job 생성과 scheduling pump 실행 가능
void ExportOrchestrator::handlePrepared(const types::RequestId requestId, ExportPreparationResult result)
{
    auto active = m_activeRequests.find(requestId);
    if (active == m_activeRequests.end())
    {
        return;
    }
    active->preparing = false;
    if (active->cancellationPublished)
    {
        cleanupCancelledRequest(requestId);
        return;
    }
    if (result.hasError())
    {
        const types::CoreError error = result.error();
        m_activeRequests.erase(active);
        if (error.code == types::ErrorCode::Cancelled)
        {
            emit exportCancelled(requestId);
        }
        else
        {
            emit exportFailed({requestId, error});
        }
        return;
    }

    const qsizetype itemCount = result.value().items.size();
    const auto unsignedItemCount = static_cast<std::uint64_t>(itemCount);
    if (unsignedItemCount > std::numeric_limits<ExportJobId>::max() - m_nextJobId)
    {
        const types::CoreError error{types::ErrorCode::Unknown, QStringLiteral("Export job ID space is exhausted.")};
        m_activeRequests.erase(active);
        emit exportFailed({requestId, error});
        return;
    }

    active->report.totalCount = static_cast<int>(itemCount);
    active->report.items.resize(itemCount);
    active->scheduling.submitted = static_cast<std::uint64_t>(itemCount);
    m_lifetimeMetrics.submitted += static_cast<std::uint64_t>(itemCount);
    QList<ExportJobId> preparationFailures;
    for (qsizetype index = 0; index < itemCount; ++index)
    {
        ExportJob job;
        job.jobId = m_nextJobId++;
        job.requestId = requestId;
        job.enqueueSequence = job.jobId;
        job.itemIndex = static_cast<int>(index);
        job.item = std::move(result.value().items[index]);
        job.remoteEligible = m_remotePort != nullptr && active->placement.remoteTarget.has_value();
        const ExportJobId jobId = job.jobId;
        const bool preparationFailed = job.item.preparationError.has_value();
        m_jobs.insert(jobId, std::move(job));
        m_enqueueOrder.push_back(jobId);
        if (preparationFailed)
        {
            preparationFailures.push_back(jobId);
        }
    }

    for (const ExportJobId jobId : preparationFailures)
    {
        const auto job = m_jobs.constFind(jobId);
        if (job != m_jobs.cend())
        {
            completeJob(jobId,
                        makeFailedItem(job->item, *job->item.preparationError, ExportItemFailureKind::Execution));
        }
    }
    finishRequestIfReady(requestId);
    pump();
}

// 목적: Remote reservation을 먼저 수행하고 남은 Local slot을 채움
// 입력: 없음
// 출력: bounded worker task 제출과 wakeup timer 갱신
void ExportOrchestrator::pump()
{
    if (m_pumpActive || !m_acceptingRequests)
    {
        return;
    }
    QScopedValueRollback guard(m_pumpActive, true);

    if (!m_remoteDispatchJobId.has_value() && m_remotePort != nullptr &&
        m_remoteInFlight < m_configuration.remoteSlotLimit)
    {
        if (const std::optional<ExportJobId> candidate = findRemoteCandidate(std::chrono::steady_clock::now());
            candidate.has_value())
        {
            startRemote(*candidate);
        }
    }

    int localLimit = availableLocalSlotLimit();
    while (m_localInFlight < localLimit)
    {
        const std::optional<ExportJobId> candidate = findLocalCandidate();
        if (!candidate.has_value())
        {
            break;
        }
        startLocal(*candidate);
        localLimit = availableLocalSlotLimit();
    }
    scheduleWakeups();
}

// 목적: stable enqueue order에서 현재 Remote dispatch 가능한 oldest job 탐색
// 입력: now: cooldown 비교용 monotonic 시각
// 출력: candidate JobId 또는 없음
std::optional<ExportOrchestrator::ExportJobId> ExportOrchestrator::findRemoteCandidate(
    const std::chrono::steady_clock::time_point now) const
{
    for (const ExportJobId jobId : m_enqueueOrder)
    {
        const auto job = m_jobs.constFind(jobId);
        if (job == m_jobs.cend() || job->state != JobState::Queued || !job->remoteEligible ||
            job->remoteAttempts >= m_configuration.maximumRemoteDispatchAttempts || job->remoteDeferUntil > now)
        {
            continue;
        }
        const auto request = m_activeRequests.constFind(job->requestId);
        if (request == m_activeRequests.cend() || request->cancellationPublished ||
            request->placement.policy == ExportPlacementPolicy::LocalOnly ||
            !request->placement.remoteTarget.has_value())
        {
            continue;
        }
        return jobId;
    }
    return std::nullopt;
}

// 목적: stable enqueue order에서 현재 Local slot을 받을 oldest job 탐색
// 입력: 없음
// 출력: candidate JobId 또는 없음
std::optional<ExportOrchestrator::ExportJobId> ExportOrchestrator::findLocalCandidate() const
{
    for (const ExportJobId jobId : m_enqueueOrder)
    {
        const auto job = m_jobs.constFind(jobId);
        if (job == m_jobs.cend() || job->state != JobState::Queued)
        {
            continue;
        }
        const auto request = m_activeRequests.constFind(job->requestId);
        if (request == m_activeRequests.cend() || request->cancellationPublished ||
            request->placement.policy == ExportPlacementPolicy::RemoteOnly)
        {
            continue;
        }
        if (request->requestedLocalLimit > 0 && request->localInFlight >= request->requestedLocalLimit)
        {
            continue;
        }
        return jobId;
    }
    return std::nullopt;
}

// 목적: Queued item 하나를 Local owner에게 제출
// 입력: jobId: claim할 pending job
// 출력: Local slot/attempt 증가와 background item 실행
void ExportOrchestrator::startLocal(const ExportJobId jobId)
{
    auto job = m_jobs.find(jobId);
    if (job == m_jobs.end() || job->state != JobState::Queued)
    {
        return;
    }
    auto request = m_activeRequests.find(job->requestId);
    if (request == m_activeRequests.end() || request->cancellationPublished)
    {
        return;
    }

    job->state = JobState::RunningLocal;
    job->owner = JobOwner::Local;
    ++job->localAttempts;
    ++m_localInFlight;
    ++request->localInFlight;
    incrementMetric(job->requestId, &ExportSchedulingMetrics::localDispatchAttempts);

    const PreparedExportItem item = job->item;
    const types::CancellationToken cancellationToken = request->cancellationSource.token();
    IExportPipeline* const pipeline = m_pipeline.get();
    m_localPool.start([this, pipeline, jobId, item, cancellationToken]() mutable {
        ExportItemResult result =
            makeFailedItem(item,
                           makeUnhandledError(QStringLiteral("Local export did not produce a result.")),
                           ExportItemFailureKind::Execution);
        try
        {
            result = pipeline->executeItem(item, cancellationToken);
        }
        catch (const std::exception& error)
        {
            result = makeFailedItem(
                item,
                makeUnhandledError(QStringLiteral("Unhandled local export exception: %1").arg(error.what())),
                ExportItemFailureKind::Execution);
        }
        catch (...)
        {
            result =
                makeFailedItem(item,
                               makeUnhandledError(QStringLiteral("Unhandled non-standard local export exception.")),
                               ExportItemFailureKind::Execution);
        }
        (void)QMetaObject::invokeMethod(
            this,
            [this, jobId, result = std::move(result)]() mutable { handleLocalFinished(jobId, std::move(result)); },
            Qt::QueuedConnection);
    });
}

// 목적: Queued item 하나를 직렬 Remote handshake에 제출
// 입력: jobId: reserve할 pending job
// 출력: DispatchingRemote 전환과 background adapter 실행
void ExportOrchestrator::startRemote(const ExportJobId jobId)
{
    auto job = m_jobs.find(jobId);
    if (job == m_jobs.end() || job->state != JobState::Queued || m_remotePort == nullptr)
    {
        return;
    }
    auto request = m_activeRequests.find(job->requestId);
    if (request == m_activeRequests.end() || request->cancellationPublished ||
        !request->placement.remoteTarget.has_value())
    {
        return;
    }

    job->state = JobState::DispatchingRemote;
    job->owner.reset();
    job->remoteAccepted = false;
    ++job->remoteAttempts;
    m_remoteDispatchJobId = jobId;
    incrementMetric(job->requestId, &ExportSchedulingMetrics::remoteDispatchAttempts);

    const ExportRemoteTarget target = *request->placement.remoteTarget;
    const PreparedExportItem item = job->item;
    const types::CancellationToken cancellationToken = request->cancellationSource.token();
    IRemoteExportExecutionPort* const remotePort = m_remotePort.get();
    m_remotePool.start([this, remotePort, jobId, target, item, cancellationToken]() mutable {
        const RemoteExportAcceptedCallback accepted = [this, jobId] {
            (void)QMetaObject::invokeMethod(this, [this, jobId] { handleRemoteAccepted(jobId); }, Qt::QueuedConnection);
        };
        RemoteExportExecutionResult result = RemoteExportExecutionResult::failure(
            {RemoteExportFailureCode::ProtocolViolation,
             makeUnhandledError(QStringLiteral("Remote export did not produce a result.")),
             {},
             false});
        try
        {
            result = remotePort->execute(target, item, cancellationToken, accepted);
        }
        catch (const std::exception& error)
        {
            result = RemoteExportExecutionResult::failure(
                {RemoteExportFailureCode::ProtocolViolation,
                 makeUnhandledError(QStringLiteral("Unhandled Remote export exception: %1").arg(error.what())),
                 {},
                 false});
        }
        catch (...)
        {
            result = RemoteExportExecutionResult::failure(
                {RemoteExportFailureCode::ProtocolViolation,
                 makeUnhandledError(QStringLiteral("Unhandled non-standard Remote export exception.")),
                 {},
                 false});
        }
        (void)QMetaObject::invokeMethod(
            this,
            [this, jobId, result = std::move(result)]() mutable { handleRemoteFinished(jobId, std::move(result)); },
            Qt::QueuedConnection);
    });
}

// 목적: Local worker 결과의 slot과 request aggregate 갱신
// 입력: jobId: internal item identity, result: item success/failure
// 출력: terminal item 또는 cancellation cleanup 후 pump 실행
void ExportOrchestrator::handleLocalFinished(const ExportJobId jobId, ExportItemResult result)
{
    auto job = m_jobs.find(jobId);
    if (job == m_jobs.end())
    {
        return;
    }
    const types::RequestId requestId = job->requestId;
    if (job->owner == JobOwner::Local)
    {
        m_localInFlight = std::max(0, m_localInFlight - 1);
        auto request = m_activeRequests.find(requestId);
        if (request != m_activeRequests.end())
        {
            request->localInFlight = std::max(0, request->localInFlight - 1);
        }
        job->owner.reset();
    }

    auto request = m_activeRequests.find(requestId);
    if (request == m_activeRequests.end() || request->cancellationPublished)
    {
        removeJob(jobId);
        cleanupCancelledRequest(requestId);
        pump();
        return;
    }

    incrementMetric(requestId, &ExportSchedulingMetrics::localExecuted);
    if (!result.succeeded && result.error.code == types::ErrorCode::Cancelled)
    {
        (void)cancelExport(requestId);
        removeJob(jobId);
        cleanupCancelledRequest(requestId);
        pump();
        return;
    }
    if (!result.succeeded && result.failureKind == ExportItemFailureKind::None)
    {
        result.failureKind = ExportItemFailureKind::Execution;
    }
    completeJob(jobId, std::move(result));
    pump();
}

// 목적: 유효한 JobAccepted를 Remote ownership과 slot으로 반영
// 입력: jobId: dispatch 중인 item identity
// 출력: handshake 해제, RunningRemote 전환과 다음 pump
void ExportOrchestrator::handleRemoteAccepted(const ExportJobId jobId)
{
    auto job = m_jobs.find(jobId);
    if (job == m_jobs.end() || job->remoteAccepted)
    {
        return;
    }
    if (m_remoteDispatchJobId == jobId)
    {
        m_remoteDispatchJobId.reset();
    }
    job->remoteAccepted = true;
    job->owner = JobOwner::Remote;
    ++m_remoteInFlight;
    const auto request = m_activeRequests.constFind(job->requestId);
    if (request != m_activeRequests.cend() && !request->cancellationPublished)
    {
        job->state = JobState::RunningRemote;
    }
    pump();
}

// 목적: normalized Remote 결과를 retry/fallback/terminal policy로 해석
// 입력: jobId: internal item identity, result: adapter success 또는 typed failure
// 출력: Remote slot 해제와 item requeue/terminal 처리
void ExportOrchestrator::handleRemoteFinished(const ExportJobId jobId, RemoteExportExecutionResult result)
{
    auto job = m_jobs.find(jobId);
    if (job == m_jobs.end())
    {
        return;
    }
    const types::RequestId requestId = job->requestId;
    if (m_remoteDispatchJobId == jobId)
    {
        m_remoteDispatchJobId.reset();
    }
    if (job->remoteAccepted)
    {
        m_remoteInFlight = std::max(0, m_remoteInFlight - 1);
    }
    job->owner.reset();

    auto request = m_activeRequests.find(requestId);
    if (request == m_activeRequests.end() || request->cancellationPublished)
    {
        removeJob(jobId);
        cleanupCancelledRequest(requestId);
        pump();
        return;
    }

    if (result.hasValue())
    {
        if (!job->remoteAccepted)
        {
            completeJob(jobId,
                        makeFailedItem(job->item,
                                       {types::ErrorCode::Unknown,
                                        QStringLiteral("Remote success arrived before Worker acceptance.")},
                                       ExportItemFailureKind::Ambiguous));
        }
        else
        {
            incrementMetric(requestId, &ExportSchedulingMetrics::remoteExecuted);
            completeJob(jobId, std::move(result.value()));
        }
        pump();
        return;
    }

    const RemoteExportFailure error = result.error();
    switch (error.code)
    {
    case RemoteExportFailureCode::Ineligible:
        job->remoteEligible = false;
        if (request->placement.policy == ExportPlacementPolicy::Auto)
        {
            job->state = JobState::Queued;
        }
        else
        {
            completeJob(jobId, makeFailedItem(job->item, error.cause, ExportItemFailureKind::Eligibility));
        }
        break;
    case RemoteExportFailureCode::ServerBusy:
        ++job->dispatchRejections;
        incrementMetric(requestId, &ExportSchedulingMetrics::serverBusyCount);
        requeueRemote(jobId, m_configuration.serverBusyCooldown, error.cause);
        break;
    case RemoteExportFailureCode::ResourceBusy:
        ++job->dispatchRejections;
        incrementMetric(requestId, &ExportSchedulingMetrics::resourceBusyCount);
        requeueRemote(jobId, error.retryAfter.value_or(m_configuration.resourceBusyCooldown), error.cause);
        break;
    case RemoteExportFailureCode::ConnectionFailed:
        incrementMetric(requestId, &ExportSchedulingMetrics::connectionFailedCount);
        requeueRemote(jobId, m_configuration.connectionFailedCooldown, error.cause);
        break;
    case RemoteExportFailureCode::RenderFailed:
        if (job->remoteAccepted)
        {
            incrementMetric(requestId, &ExportSchedulingMetrics::remoteExecuted);
        }
        completeJob(jobId, makeFailedItem(job->item, error.cause, ExportItemFailureKind::Execution));
        break;
    case RemoteExportFailureCode::ConnectionLost:
    case RemoteExportFailureCode::TimedOut:
    case RemoteExportFailureCode::ProtocolViolation:
        completeJob(jobId, makeFailedItem(job->item, error.cause, ExportItemFailureKind::Ambiguous));
        break;
    case RemoteExportFailureCode::Cancelled:
        (void)cancelExport(requestId);
        removeJob(jobId);
        cleanupCancelledRequest(requestId);
        break;
    }
    pump();
}

// 목적: NotStarted Remote rejection을 cooldown과 attempt 제한을 적용해 재등록
// 입력: jobId: 대상 item, delay: Remote target 재허용 지연, cause: exhaustion 진단
// 출력: Auto requeue 또는 RemoteOnly/attempt exhaustion terminal 처리
void ExportOrchestrator::requeueRemote(const ExportJobId jobId,
                                       const std::chrono::milliseconds delay,
                                       const types::CoreError& cause)
{
    auto job = m_jobs.find(jobId);
    if (job == m_jobs.end())
    {
        return;
    }
    const auto request = m_activeRequests.constFind(job->requestId);
    if (request == m_activeRequests.cend())
    {
        return;
    }
    if (request->placement.policy != ExportPlacementPolicy::Auto)
    {
        completeJob(jobId, makeFailedItem(job->item, cause, ExportItemFailureKind::Execution));
        return;
    }

    job->state = JobState::Queued;
    job->owner.reset();
    job->remoteAccepted = false;
    if (job->remoteAttempts >= m_configuration.maximumRemoteDispatchAttempts)
    {
        job->remoteEligible = false;
        return;
    }
    job->remoteDeferUntil = std::chrono::steady_clock::now() + delay;
}

// 목적: item 결과를 request report와 lifetime metric에 정확히 한 번 반영
// 입력: jobId: terminal item, result: 성공/실패와 failure kind
// 출력: progress 및 request completion event 가능
void ExportOrchestrator::completeJob(const ExportJobId jobId, ExportItemResult result)
{
    const auto job = m_jobs.constFind(jobId);
    if (job == m_jobs.cend())
    {
        return;
    }
    const types::RequestId requestId = job->requestId;
    const int itemIndex = job->itemIndex;
    const QString sourcePath = result.sourcePath.isEmpty() ? job->item.request.source.path : result.sourcePath;
    auto request = m_activeRequests.find(requestId);
    if (request == m_activeRequests.end() || request->cancellationPublished)
    {
        removeJob(jobId);
        cleanupCancelledRequest(requestId);
        return;
    }

    if (result.succeeded)
    {
        ++request->report.succeededCount;
        incrementMetric(requestId, &ExportSchedulingMetrics::succeeded);
    }
    else
    {
        ++request->report.failedCount;
        if (result.failureKind == ExportItemFailureKind::Ambiguous)
        {
            incrementMetric(requestId, &ExportSchedulingMetrics::ambiguous);
        }
        else
        {
            incrementMetric(requestId, &ExportSchedulingMetrics::failed);
        }
    }
    request->report.items[itemIndex] = std::move(result);
    ++request->completedCount;
    const ExportProgress progress{requestId,
                                  request->completedCount,
                                  request->report.totalCount,
                                  request->report.succeededCount,
                                  request->report.failedCount,
                                  sourcePath};
    removeJob(jobId);
    emit exportProgressed(progress);
    finishRequestIfReady(requestId);
}

// 목적: internal job을 queue/hash에서 제거
// 입력: jobId: 제거할 item identity
// 출력: enqueue order와 job storage 동시 정리
void ExportOrchestrator::removeJob(const ExportJobId jobId)
{
    m_jobs.remove(jobId);
    m_enqueueOrder.removeAll(jobId);
}

// 목적: 모든 item이 끝난 request를 report와 함께 완료
// 입력: requestId: aggregate identity
// 출력: exportCompleted signal 또는 cancelled request cleanup
void ExportOrchestrator::finishRequestIfReady(const types::RequestId requestId)
{
    auto request = m_activeRequests.find(requestId);
    if (request == m_activeRequests.end() || request->preparing || hasJobs(requestId))
    {
        return;
    }
    if (request->cancellationPublished)
    {
        m_activeRequests.erase(request);
        return;
    }
    if (request->completedCount != request->report.totalCount)
    {
        return;
    }

    request->report.scheduling = request->scheduling;
    ExportResult result{requestId, std::move(request->report)};
    m_activeRequests.erase(request);
    emit exportCompleted(result);
}

// 목적: 지정 export request의 queued/running item을 terminal cancellation으로 전환
// 입력: requestId: 취소할 export request 식별자
// 출력: active request를 취소했으면 true
bool ExportOrchestrator::cancelExport(const types::RequestId requestId)
{
    auto request = m_activeRequests.find(requestId);
    if (request == m_activeRequests.end() || request->cancellationPublished)
    {
        return false;
    }

    request->cancellationPublished = true;
    request->cancellationSource.requestCancellation();
    QList<ExportJobId> queuedJobs;
    std::uint64_t cancelledCount = 0;
    for (const ExportJobId jobId : std::as_const(m_enqueueOrder))
    {
        auto job = m_jobs.find(jobId);
        if (job == m_jobs.end() || job->requestId != requestId)
        {
            continue;
        }
        ++cancelledCount;
        if (job->state == JobState::Queued)
        {
            queuedJobs.push_back(jobId);
        }
        else
        {
            job->state = JobState::Cancelled;
        }
    }
    for (const ExportJobId jobId : queuedJobs)
    {
        removeJob(jobId);
    }
    request->scheduling.cancelled += cancelledCount;
    m_lifetimeMetrics.cancelled += cancelledCount;
    emit exportCancelled(requestId);
    cleanupCancelledRequest(requestId);
    pump();
    return true;
}

// 목적: cancelled request의 background cleanup 완료 여부 확인
// 입력: requestId: cancellation을 이미 publish한 aggregate identity
// 출력: 남은 preparation/job이 없으면 request storage 제거
void ExportOrchestrator::cleanupCancelledRequest(const types::RequestId requestId)
{
    auto request = m_activeRequests.find(requestId);
    if (request != m_activeRequests.end() && request->cancellationPublished && !request->preparing &&
        !hasJobs(requestId))
    {
        m_activeRequests.erase(request);
    }
}

// 목적: request에 속한 internal job 존재 여부 검사
// 입력: requestId: aggregate identity
// 출력: queued/running/cancel cleanup item이 하나라도 있으면 true
bool ExportOrchestrator::hasJobs(const types::RequestId requestId) const
{
    return std::any_of(
        m_jobs.cbegin(), m_jobs.cend(), [requestId](const ExportJob& job) { return job.requestId == requestId; });
}

// 목적: 현재 host resource reserve를 반영한 Local hard ceiling 계산
// 입력: 없음
// 출력: 새 Local item을 포함해 허용할 total Local in-flight 수
int ExportOrchestrator::availableLocalSlotLimit() const
{
    if (!m_configuration.enforceLocalResourceReserve)
    {
        return m_configuration.localSlotLimit;
    }
    if (m_memoryProbe == nullptr)
    {
        return 0;
    }
    const std::optional<platform::SystemMemorySnapshot> memory = m_memoryProbe->snapshot();
    if (!memory.has_value() || memory->availableBytes <= m_configuration.memoryReserveBytes)
    {
        return 0;
    }

    const int cpuSlots = std::max(0, QThread::idealThreadCount() - m_configuration.reservedLogicalProcessors);
    const std::uint64_t memorySlots =
        (memory->availableBytes - m_configuration.memoryReserveBytes) / m_configuration.memoryClaimPerJobBytes;
    const int boundedMemorySlots = static_cast<int>(
        std::min<std::uint64_t>(memorySlots, static_cast<std::uint64_t>(std::numeric_limits<int>::max())));
    return std::max(0, std::min({m_configuration.localSlotLimit, cpuSlots, boundedMemorySlots}));
}

// 목적: cooldown/resource wait 중인 pending item을 위한 non-blocking wakeup 예약
// 입력: 없음
// 출력: earliest cooldown 또는 resource 재확인 timer 설정
void ExportOrchestrator::scheduleWakeups()
{
    m_cooldownTimer.stop();
    const auto now = std::chrono::steady_clock::now();
    std::optional<std::chrono::steady_clock::time_point> earliest;
    bool hasQueuedLocal = false;
    for (const ExportJobId jobId : m_enqueueOrder)
    {
        const auto job = m_jobs.constFind(jobId);
        if (job == m_jobs.cend() || job->state != JobState::Queued)
        {
            continue;
        }
        const auto request = m_activeRequests.constFind(job->requestId);
        if (request == m_activeRequests.cend() || request->cancellationPublished)
        {
            continue;
        }
        if (request->placement.policy != ExportPlacementPolicy::RemoteOnly)
        {
            hasQueuedLocal = true;
        }
        if (job->remoteEligible && request->placement.policy != ExportPlacementPolicy::LocalOnly &&
            job->remoteAttempts < m_configuration.maximumRemoteDispatchAttempts && job->remoteDeferUntil > now &&
            (!earliest.has_value() || job->remoteDeferUntil < *earliest))
        {
            earliest = job->remoteDeferUntil;
        }
    }
    if (earliest.has_value())
    {
        m_cooldownTimer.start(boundedTimerMilliseconds(*earliest - now));
    }

    if (hasQueuedLocal && availableLocalSlotLimit() <= m_localInFlight)
    {
        if (!m_resourceTimer.isActive())
        {
            m_resourceTimer.start(ResourceRecheckMilliseconds);
        }
    }
    else
    {
        m_resourceTimer.stop();
    }
}

// 목적: request와 lifetime metric의 동일 counter를 함께 증가
// 입력: requestId: aggregate identity, member: 증가할 metric field, amount: 증가량
// 출력: request/lifetime snapshot 갱신
void ExportOrchestrator::incrementMetric(const types::RequestId requestId,
                                         std::uint64_t ExportSchedulingMetrics::* const member,
                                         const std::uint64_t amount)
{
    m_lifetimeMetrics.*member += amount;
    auto request = m_activeRequests.find(requestId);
    if (request != m_activeRequests.end())
    {
        request->scheduling.*member += amount;
    }
}

// 목적: application lifetime 동안 누적된 placement metric snapshot 반환
// 입력: 없음
// 출력: item terminal/dispatch/rejection 누적 counter
ExportSchedulingMetrics ExportOrchestrator::schedulingMetrics() const
{
    return m_lifetimeMetrics;
}

}  // namespace flexraw::core::orchestration
