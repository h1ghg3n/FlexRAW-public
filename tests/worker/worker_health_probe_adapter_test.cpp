#include <chrono>
#include <future>
#include <utility>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QTemporaryDir>
#include <QThread>

#include <gtest/gtest.h>

#include "worker_application_context.h"
#include "worker_health_probe_adapter.h"
#include "worker_path_resolver.h"

namespace flexraw::worker::client
{
namespace
{

using namespace std::chrono_literals;

// 목적: 실제 Worker server event를 처리하며 health Adapter 결과 대기
// 입력: future: background TCP probe, timeoutMilliseconds: 최대 대기 시간
// 출력: timeout 전에 future가 ready이면 true
[[nodiscard]] bool waitForFuture(std::future<core::orchestration::WorkerHealthPortResult>& future,
                                 const int timeoutMilliseconds = 3000)
{
    QElapsedTimer timer;
    timer.start();
    while (future.wait_for(0ms) != std::future_status::ready && timer.elapsed() < timeoutMilliseconds)
    {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    return future.wait_for(0ms) == std::future_status::ready;
}

// 목적: temporary source/output root를 검증된 WorkerPathResolver로 변환
// 입력: sourceRoot/outputRoot: 존재하는 test directory
// 출력: 실제 Worker application context에 전달할 resolver
[[nodiscard]] runtime::WorkerPathResolver makeResolver(const QTemporaryDir& sourceRoot, const QTemporaryDir& outputRoot)
{
    runtime::WorkerPathResolver::CreateResult result =
        runtime::WorkerPathResolver::create({sourceRoot.path(), outputRoot.path()});
    EXPECT_TRUE(result.hasValue());
    return std::move(result.value());
}

TEST(WorkerHealthProbeAdapterTest, MapsActualWorkerRuntimeObservationToProductOperation)
{
    QTemporaryDir sourceRoot;
    QTemporaryDir outputRoot;
    ASSERT_TRUE(sourceRoot.isValid());
    ASSERT_TRUE(outputRoot.isValid());
    app::WorkerApplicationConfiguration configuration;
    configuration.maximumConcurrency = 3;
    configuration.queueCapacity = 5;
    app::WorkerApplicationContext context(makeResolver(sourceRoot, outputRoot), configuration);
    ASSERT_TRUE(context.listen(QHostAddress::LocalHost, 0)) << context.errorString().toStdString();
    const WorkerHealthProbeAdapter adapter;
    const core::orchestration::WorkerHealthProbeTarget target{"127.0.0.1", context.serverPort(), 1000ms, 1000ms};
    std::future<core::orchestration::WorkerHealthPortResult> future =
        std::async(std::launch::async, [&adapter, target]() { return adapter.probe(target); });

    ASSERT_TRUE(waitForFuture(future));
    const core::orchestration::WorkerHealthPortResult result = future.get();

    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(core::client::WorkerReachability::Reachable, result.value().reachability);
    EXPECT_EQ(core::client::WorkerCompatibility::Compatible, result.value().compatibility);
    EXPECT_EQ(core::client::WorkerServiceState::Ready, result.value().serviceState);
    ASSERT_TRUE(result.value().load.has_value());
    EXPECT_EQ(0U, result.value().load->runningJobs);
    EXPECT_EQ(0U, result.value().load->queuedJobs);
    EXPECT_EQ(3U, result.value().load->maximumConcurrentJobs);
    EXPECT_EQ(5U, result.value().load->queueCapacity);
    EXPECT_TRUE(result.value().roundTripMilliseconds.has_value());
}

}  // namespace
}  // namespace flexraw::worker::client
