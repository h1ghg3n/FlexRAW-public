#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QMutex>
#include <QMutexLocker>
#include <QSemaphore>
#include <QThread>

#include <gtest/gtest.h>

#include "export_execution_port.h"
#include "export_orchestrator.h"
#include "export_pipeline.h"
#include "qt_export_client_adapter.h"

namespace flexraw::ui::export_
{
namespace
{

// 목적: Qt event를 처리하며 adapter lifecycle 조건을 bounded wait
// 입력: predicate: 완료 조건, timeoutMilliseconds: 최대 대기 시간
// 출력: 제한 시간 안에 조건을 만족하면 true
[[nodiscard]] bool waitForCondition(const std::function<bool()>& predicate, const int timeoutMilliseconds = 5000)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMilliseconds)
    {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    return predicate();
}

// 목적: adapter test용 valid Qt-free single-file Export command 생성
// 입력: placement: Local/Remote/Auto policy와 optional profile identity
// 출력: deterministic RAW source/output과 raster option을 가진 command
[[nodiscard]] core::client::SubmitExportCommand makeCommand(core::client::ExportPlacementOptions placement = {})
{
    core::client::ExportFileRequest request;
    request.source = {"C:/photos/input.arw", "arw", "input.arw", core::client::CatalogFileKind::Raw};
    request.outputLocator = "C:/exports/output.jpg";
    request.developParams = core::client::EditorDevelopParams{};
    request.developParams->exposureEv = 0.75F;
    request.options.jpegQuality = 87;
    return {core::client::ExportRequest{std::move(request)}, std::move(placement)};
}

class WorkerProfileClientStub final : public core::client::IWorkerProfileClient
{
public:
    // 목적: adapter profile resolution test에 저장 순서가 보존된 snapshot 제공
    // 입력: 없음
    // 출력: configured profile vector
    [[nodiscard]] core::client::WorkerProfileListResult listWorkerProfiles() override
    {
        return core::client::WorkerProfileListResult::success(profiles);
    }

    // 목적: test 범위 밖 profile 생성 거부
    // 입력: command: 미사용 create command
    // 출력: InvalidArgument test error
    [[nodiscard]] core::client::WorkerProfileResult createWorkerProfile(
        const core::client::CreateWorkerProfileCommand&) override
    {
        return unsupported();
    }

    // 목적: test 범위 밖 profile 갱신 거부
    // 입력: command: 미사용 update command
    // 출력: InvalidArgument test error
    [[nodiscard]] core::client::WorkerProfileResult updateWorkerProfile(
        const core::client::UpdateWorkerProfileCommand&) override
    {
        return unsupported();
    }

    // 목적: test 범위 밖 profile 제거 거부
    // 입력: id: 미사용 profile identity
    // 출력: InvalidArgument test error
    [[nodiscard]] core::client::WorkerProfileRemoveResult removeWorkerProfile(
        const core::client::WorkerProfileId&) override
    {
        return core::client::WorkerProfileRemoveResult::failure(
            {core::client::ClientErrorCode::InvalidArgument, "Worker profile mutation is unavailable in this test."});
    }

    // 목적: test 범위 밖 enabled 변경 거부
    // 입력: command: 미사용 enabled command
    // 출력: InvalidArgument test error
    [[nodiscard]] core::client::WorkerProfileResult setWorkerProfileEnabled(
        const core::client::SetWorkerProfileEnabledCommand&) override
    {
        return unsupported();
    }

    std::vector<core::client::WorkerProfileSnapshot> profiles;

private:
    // 목적: 반복되는 unsupported profile mutation result 생성
    // 입력: 없음
    // 출력: InvalidArgument WorkerProfileResult
    [[nodiscard]] static core::client::WorkerProfileResult unsupported()
    {
        return core::client::WorkerProfileResult::failure(
            {core::client::ClientErrorCode::InvalidArgument, "Worker profile mutation is unavailable in this test."});
    }
};

class BlockingExportPipeline final : public core::orchestration::IExportPipeline
{
public:
    // 목적: adapter test가 active/cancellation 시점을 제어하도록 item 실행 차단
    // 입력: request: projected request, cancellationToken: cooperative cancel, progress: 미사용
    // 출력: release 후 success report 또는 cancellation error
    [[nodiscard]] core::orchestration::ExportPipelineResult execute(
        const core::orchestration::ExportRequest& request,
        const core::types::CancellationToken& cancellationToken,
        const core::orchestration::ExportProgressCallback&) const override
    {
        const auto fileRequest = std::get<core::orchestration::ExportFileRequest>(request);
        {
            QMutexLocker lock(&m_mutex);
            m_request = fileRequest;
        }
        m_started.release();
        while (!m_release.tryAcquire(1, 5))
        {
            if (cancellationToken.isCancellationRequested())
            {
                return core::orchestration::ExportPipelineResult::failure(
                    {core::types::ErrorCode::Cancelled, QStringLiteral("Cancelled by adapter test.")});
            }
        }
        core::orchestration::ExportReport report;
        report.totalCount = 1;
        report.succeededCount = 1;
        report.items.push_back({fileRequest.source.path, fileRequest.outputPath, true, {}});
        return core::orchestration::ExportPipelineResult::success(std::move(report));
    }

    // 목적: background item 실행 시작 여부를 test thread에 제공
    // 입력: 없음
    // 출력: started semaphore 참조
    [[nodiscard]] QSemaphore& started() const
    {
        return m_started;
    }

    // 목적: 차단된 item execution을 success terminal로 진행
    // 입력: 없음
    // 출력: release semaphore 증가
    void release() const
    {
        m_release.release();
    }

    // 목적: adapter가 projection한 existing file request snapshot 조회
    // 입력: 없음
    // 출력: execution 전이면 빈 값, 이후면 recorded request
    [[nodiscard]] std::optional<core::orchestration::ExportFileRequest> request() const
    {
        QMutexLocker lock(&m_mutex);
        return m_request;
    }

private:
    mutable QMutex m_mutex;
    mutable QSemaphore m_started;
    mutable QSemaphore m_release;
    mutable std::optional<core::orchestration::ExportFileRequest> m_request;
};

class RecordingRemotePort final : public core::orchestration::IRemoteExportExecutionPort
{
public:
    // 목적: resolved Worker endpoint를 기록하고 deterministic Remote success 반환
    // 입력: target/item/token: owner 실행 값, accepted: ownership callback
    // 출력: input/output intent가 보존된 successful item result
    [[nodiscard]] core::orchestration::RemoteExportExecutionResult execute(
        const core::orchestration::ExportRemoteTarget& target,
        const core::orchestration::PreparedExportItem& item,
        const core::types::CancellationToken&,
        const core::orchestration::RemoteExportAcceptedCallback& accepted) const override
    {
        {
            QMutexLocker lock(&m_mutex);
            m_target = target;
        }
        accepted();
        return core::orchestration::RemoteExportExecutionResult::success(
            {item.request.source.path, item.request.outputPath, true, {}});
    }

    // 목적: background Remote execution이 받은 endpoint snapshot 조회
    // 입력: 없음
    // 출력: execution 전이면 빈 값, 이후면 recorded target
    [[nodiscard]] std::optional<core::orchestration::ExportRemoteTarget> target() const
    {
        QMutexLocker lock(&m_mutex);
        return m_target;
    }

private:
    mutable QMutex m_mutex;
    mutable std::optional<core::orchestration::ExportRemoteTarget> m_target;
};

class PreExecutionRejectingRemotePort final : public core::orchestration::IRemoteExportExecutionPort
{
public:
    // 목적: Worker acceptance 전 ServerBusy를 반환해 public DispatchExhausted projection 검증
    // 입력: target/item/token/accepted: 사용하지 않는 Remote execution contract 값
    // 출력: 실행이 시작되지 않은 structured rejection
    [[nodiscard]] core::orchestration::RemoteExportExecutionResult execute(
        const core::orchestration::ExportRemoteTarget&,
        const core::orchestration::PreparedExportItem&,
        const core::types::CancellationToken&,
        const core::orchestration::RemoteExportAcceptedCallback&) const override
    {
        return core::orchestration::RemoteExportExecutionResult::failure(
            {core::orchestration::RemoteExportFailureCode::ServerBusy,
             {core::types::ErrorCode::Unknown, QStringLiteral("Fake Worker is busy.")},
             {},
             false});
    }
};

TEST(QtExportClientAdapterTest, PublishesOrderedLifecycleAndProjectsRequest)
{
    auto pipeline = std::make_unique<BlockingExportPipeline>();
    BlockingExportPipeline* const pipelineProbe = pipeline.get();
    core::orchestration::ExportOrchestrator orchestrator(std::move(pipeline), 1);
    WorkerProfileClientStub profiles;
    QtExportClientAdapter adapter(orchestrator, profiles);
    std::vector<core::client::ExportEvent> events;
    const core::client::ExportSubscriptionResult subscribed =
        adapter.subscribeToExports([&events](const core::client::ExportEvent& event) { events.push_back(event); });
    ASSERT_TRUE(subscribed.hasValue());

    const core::client::ExportSubmissionResult submitted = adapter.submitExport(makeCommand());
    ASSERT_TRUE(submitted.hasValue());
    ASSERT_TRUE(waitForCondition([&] { return pipelineProbe->started().available() > 0; }));
    ASSERT_TRUE(pipelineProbe->started().tryAcquire());
    ASSERT_TRUE(waitForCondition([&] {
        return std::ranges::any_of(events,
                                   [](const core::client::ExportEvent& event) { return event.accepted.has_value(); });
    }));
    const core::client::ExportSnapshotResult active = adapter.exportSnapshot(submitted.value().requestId);
    ASSERT_TRUE(active.hasValue());
    ASSERT_EQ(1U, active.value().activeRequests.size());
    const std::optional<core::orchestration::ExportFileRequest> projected = pipelineProbe->request();
    ASSERT_TRUE(projected.has_value());
    EXPECT_EQ(QStringLiteral("C:/photos/input.arw"), projected->source.path);
    EXPECT_EQ(QStringLiteral("C:/exports/output.jpg"), projected->outputPath);
    ASSERT_TRUE(projected->developParams.has_value());
    EXPECT_FLOAT_EQ(0.75F, projected->developParams->exposureEv);
    EXPECT_EQ(87, projected->options.jpegQuality);

    pipelineProbe->release();
    ASSERT_TRUE(waitForCondition([&] {
        return std::ranges::any_of(events,
                                   [](const core::client::ExportEvent& event) { return event.completed.has_value(); });
    }));
    ASSERT_TRUE(events.front().initial);
    for (std::size_t index = 1; index < events.size(); ++index)
    {
        EXPECT_LT(events[index - 1].sequence.value, events[index].sequence.value);
    }
    const auto completed = std::ranges::find_if(
        events, [](const core::client::ExportEvent& event) { return event.completed.has_value(); });
    ASSERT_NE(events.end(), completed);
    EXPECT_EQ(1U, completed->completed->report.succeededCount);
    EXPECT_TRUE(completed->snapshot.activeRequests.empty());
    EXPECT_TRUE(adapter.exportSnapshot(submitted.value().requestId).hasError());
}

TEST(QtExportClientAdapterTest, ResolvesWorkerProfileOnlyInsideAdapter)
{
    auto remotePort = std::make_unique<RecordingRemotePort>();
    RecordingRemotePort* const remoteProbe = remotePort.get();
    core::orchestration::ExportSchedulingConfiguration configuration;
    configuration.localSlotLimit = 1;
    configuration.remoteSlotLimit = 1;
    configuration.enforceLocalResourceReserve = false;
    core::orchestration::ExportOrchestrator orchestrator(
        std::make_unique<BlockingExportPipeline>(), std::move(remotePort), nullptr, configuration);
    WorkerProfileClientStub profiles;
    profiles.profiles.push_back(
        {{"worker-a"}, "Remote Worker", "192.168.0.42", 49200, true, "source-a", "output-a"});
    QtExportClientAdapter adapter(orchestrator, profiles);
    bool completed = false;
    const core::client::ExportSubscriptionResult subscribed = adapter.subscribeToExports(
        [&completed](const core::client::ExportEvent& event) { completed = completed || event.completed.has_value(); });
    ASSERT_TRUE(subscribed.hasValue());

    const core::client::ExportSubmissionResult submitted = adapter.submitExport(
        makeCommand({core::client::ExportPlacementPolicy::RemoteOnly, core::client::WorkerProfileId{"worker-a"}}));
    ASSERT_TRUE(submitted.hasValue());
    ASSERT_TRUE(waitForCondition([&] { return completed; }));
    const std::optional<core::orchestration::ExportRemoteTarget> target = remoteProbe->target();
    ASSERT_TRUE(target.has_value());
    EXPECT_EQ(QStringLiteral("192.168.0.42"), target->host);
    EXPECT_EQ(49200, target->port);
    EXPECT_EQ(QStringLiteral("source-a"), target->expectedSourceStorageId);
    EXPECT_EQ(QStringLiteral("output-a"), target->expectedOutputStorageId);
}

TEST(QtExportClientAdapterTest, ProjectsRemoteOnlyPreExecutionRejectionAsCompletedDispatchExhausted)
{
    auto pipeline = std::make_unique<BlockingExportPipeline>();
    BlockingExportPipeline* const localProbe = pipeline.get();
    core::orchestration::ExportSchedulingConfiguration configuration;
    configuration.localSlotLimit = 1;
    configuration.remoteSlotLimit = 1;
    configuration.enforceLocalResourceReserve = false;
    core::orchestration::ExportOrchestrator orchestrator(
        std::move(pipeline), std::make_unique<PreExecutionRejectingRemotePort>(), nullptr, configuration);
    WorkerProfileClientStub profiles;
    profiles.profiles.push_back({{"worker-a"}, "Worker", "127.0.0.1", 47331, true, "source", "output"});
    QtExportClientAdapter adapter(orchestrator, profiles);
    std::vector<core::client::ExportEvent> events;
    const core::client::ExportSubscriptionResult subscribed =
        adapter.subscribeToExports([&events](const core::client::ExportEvent& event) { events.push_back(event); });
    ASSERT_TRUE(subscribed.hasValue());

    const core::client::ExportSubmissionResult submitted = adapter.submitExport(
        makeCommand({core::client::ExportPlacementPolicy::RemoteOnly, core::client::WorkerProfileId{"worker-a"}}));
    ASSERT_TRUE(submitted.hasValue());
    ASSERT_TRUE(waitForCondition([&] {
        return std::ranges::any_of(events,
                                   [](const core::client::ExportEvent& event) { return event.completed.has_value(); });
    }));

    const auto completed = std::ranges::find_if(
        events, [](const core::client::ExportEvent& event) { return event.completed.has_value(); });
    ASSERT_NE(events.end(), completed);
    EXPECT_EQ(1U, completed->completed->report.failedCount);
    ASSERT_EQ(1U, completed->completed->report.items.size());
    EXPECT_EQ(core::client::ExportFailureKind::DispatchExhausted,
              completed->completed->report.items.front().failureKind);
    EXPECT_EQ(0, std::ranges::count_if(events, [](const core::client::ExportEvent& event) {
                  return event.failed.has_value();
              }));
    EXPECT_EQ(0, localProbe->started().available());
    EXPECT_TRUE(completed->snapshot.activeRequests.empty());
}

TEST(QtExportClientAdapterTest, RejectsMissingRemoteProfileAndPublishesOneCancelledTerminal)
{
    auto pipeline = std::make_unique<BlockingExportPipeline>();
    BlockingExportPipeline* const pipelineProbe = pipeline.get();
    core::orchestration::ExportOrchestrator orchestrator(std::move(pipeline), 1);
    WorkerProfileClientStub profiles;
    QtExportClientAdapter adapter(orchestrator, profiles);
    const core::client::ExportSubmissionResult remote = adapter.submitExport(
        makeCommand({core::client::ExportPlacementPolicy::RemoteOnly, core::client::WorkerProfileId{"missing"}}));
    ASSERT_TRUE(remote.hasError());
    EXPECT_EQ(core::client::ClientErrorCode::NotFound, remote.error().code);

    std::vector<core::client::ExportEvent> events;
    const core::client::ExportSubscriptionResult subscribed =
        adapter.subscribeToExports([&events](const core::client::ExportEvent& event) { events.push_back(event); });
    ASSERT_TRUE(subscribed.hasValue());
    const core::client::ExportSubmissionResult local = adapter.submitExport(makeCommand());
    ASSERT_TRUE(local.hasValue());
    ASSERT_TRUE(waitForCondition([&] { return pipelineProbe->started().available() > 0; }));
    ASSERT_TRUE(pipelineProbe->started().tryAcquire());
    ASSERT_TRUE(adapter.cancelExport(local.value().requestId).hasValue());
    ASSERT_TRUE(waitForCondition([&] {
        return std::ranges::count_if(
                   events, [](const core::client::ExportEvent& event) { return event.cancelled.has_value(); }) == 1;
    }));
    EXPECT_EQ(0, std::ranges::count_if(events, [](const core::client::ExportEvent& event) {
                  return event.completed.has_value() || event.failed.has_value();
              }));
    const std::size_t deliveredCount = events.size();
    subscribed.value()->unsubscribe();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    EXPECT_EQ(deliveredCount, events.size());
    const core::client::ExportSnapshotResult snapshot = adapter.exportSnapshot();
    ASSERT_TRUE(snapshot.hasValue());
    EXPECT_TRUE(snapshot.value().activeRequests.empty());
}

TEST(QtExportClientAdapterTest, KeepsSubscriptionActiveWhileCancelledCallbackDrains)
{
    auto pipeline = std::make_unique<BlockingExportPipeline>();
    BlockingExportPipeline* const pipelineProbe = pipeline.get();
    core::orchestration::ExportOrchestrator orchestrator(std::move(pipeline), 1);
    WorkerProfileClientStub profiles;
    QtExportClientAdapter adapter(orchestrator, profiles);
    std::vector<core::client::ExportEvent> events;
    const core::client::ExportSubscriptionResult subscribed =
        adapter.subscribeToExports([&events](const core::client::ExportEvent& event) { events.push_back(event); });
    ASSERT_TRUE(subscribed.hasValue());

    const core::client::ExportSubmissionResult cancelled = adapter.submitExport(makeCommand());
    ASSERT_TRUE(cancelled.hasValue());
    ASSERT_TRUE(waitForCondition([&] { return pipelineProbe->started().available() > 0; }));
    ASSERT_TRUE(pipelineProbe->started().tryAcquire());
    ASSERT_TRUE(adapter.cancelExport(cancelled.value().requestId).hasValue());

    const core::client::ExportSubmissionResult independent = adapter.submitExport(makeCommand());
    ASSERT_TRUE(independent.hasValue());
    ASSERT_TRUE(waitForCondition([&] { return pipelineProbe->started().available() > 0; }));
    ASSERT_TRUE(pipelineProbe->started().tryAcquire());
    pipelineProbe->release();
    ASSERT_TRUE(waitForCondition([&] {
        return std::ranges::any_of(events, [&](const core::client::ExportEvent& event) {
            return event.completed.has_value() && event.completed->requestId == independent.value().requestId;
        });
    }));
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);

    EXPECT_EQ(1, std::ranges::count_if(events, [&](const core::client::ExportEvent& event) {
                  return event.cancelled.has_value() && event.cancelled->requestId == cancelled.value().requestId;
              }));
    EXPECT_EQ(0, std::ranges::count_if(events, [&](const core::client::ExportEvent& event) {
                  const bool progressed =
                      event.progress.has_value() && event.progress->requestId == cancelled.value().requestId;
                  const bool completed =
                      event.completed.has_value() && event.completed->requestId == cancelled.value().requestId;
                  const bool failed =
                      event.failed.has_value() && event.failed->requestId == cancelled.value().requestId;
                  return progressed || completed || failed;
              }));
    const core::client::ExportSnapshotResult snapshot = adapter.exportSnapshot();
    ASSERT_TRUE(snapshot.hasValue());
    EXPECT_TRUE(snapshot.value().activeRequests.empty());
}

TEST(QtExportClientAdapterTest, DestructionSuppressesInFlightCompletionForOutlivingSubscription)
{
    auto pipeline = std::make_unique<BlockingExportPipeline>();
    BlockingExportPipeline* const pipelineProbe = pipeline.get();
    core::orchestration::ExportOrchestrator orchestrator(std::move(pipeline), 1);
    WorkerProfileClientStub profiles;
    auto adapter = std::make_unique<QtExportClientAdapter>(orchestrator, profiles);
    std::vector<core::client::ExportEvent> events;
    const core::client::ExportSubscriptionResult subscribed =
        adapter->subscribeToExports([&events](const core::client::ExportEvent& event) { events.push_back(event); });
    ASSERT_TRUE(subscribed.hasValue());
    core::client::ExportSubscriptionHandle handle = subscribed.value();
    ASSERT_TRUE(waitForCondition([&events] { return events.size() == 1; }));

    const core::client::ExportSubmissionResult submitted = adapter->submitExport(makeCommand());
    ASSERT_TRUE(submitted.hasValue());
    ASSERT_TRUE(waitForCondition([&] { return pipelineProbe->started().available() > 0; }));
    ASSERT_TRUE(pipelineProbe->started().tryAcquire());
    ASSERT_TRUE(waitForCondition([&events] {
        return std::ranges::any_of(events,
                                   [](const core::client::ExportEvent& event) { return event.accepted.has_value(); });
    }));

    bool ownerCompleted = false;
    QObject::connect(&orchestrator,
                     &core::orchestration::ExportOrchestrator::exportCompleted,
                     &orchestrator,
                     [&ownerCompleted] { ownerCompleted = true; });
    const std::size_t deliveredBeforeDestruction = events.size();
    adapter.reset();
    EXPECT_FALSE(handle->isActive());

    pipelineProbe->release();
    ASSERT_TRUE(waitForCondition([&ownerCompleted] { return ownerCompleted; }));
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    EXPECT_EQ(deliveredBeforeDestruction, events.size());
}

}  // namespace
}  // namespace flexraw::ui::export_
