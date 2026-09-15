#include <functional>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QTcpSocket>
#include <QThread>

#include <gtest/gtest.h>

#include "worker_server.h"

namespace flexraw::worker::network
{
namespace
{

// 목적: WorkerServer test에서 Qt event를 처리하며 조건 대기
// 입력: predicate: 완료 조건, timeoutMilliseconds: 최대 대기 시간
// 출력: timeout 전에 조건을 만족하면 true
[[nodiscard]] bool waitForServerCondition(const std::function<bool()>& predicate, const int timeoutMilliseconds = 3000)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMilliseconds)
    {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
    return predicate();
}

class NoopRenderWorkerRuntime final : public runtime::IRenderWorkerRuntime
{
public:
    // 목적: network-only target이 concrete Worker Runtime 없이 server를 조립할 수 있게 하는 fake port
    // 입력: command/completion: connection lifecycle test에서 사용하지 않는 Runtime 값
    // 출력: 호출 시 신규 작업을 받지 않는 고정 상태
    [[nodiscard]] runtime::RenderWorkerSubmitResult submit(runtime::RenderWorkerCommand command,
                                                           runtime::RenderJobCompletion completion) override
    {
        static_cast<void>(command);
        static_cast<void>(completion);
        return runtime::RenderWorkerSubmitResult::success(runtime::SubmitStatus::ShuttingDown);
    }

    // 목적: connection lifecycle test에서 들어올 수 있는 cancellation을 무해하게 처리
    // 입력: key: 사용하지 않는 Runtime identity
    // 출력: active 작업이 없으므로 false
    [[nodiscard]] bool cancel(const runtime::RenderJobKey key) override
    {
        static_cast<void>(key);
        return false;
    }

    // 목적: network-only health projection에 deterministic Runtime 관측값 제공
    // 입력: 없음
    // 출력: 신규 접수를 받지 않는 빈 snapshot
    [[nodiscard]] runtime::WorkerRuntimeSnapshot snapshot() const override
    {
        return {0, 0, 1, 2, {}, false};
    }
};

TEST(WorkerServerTest, EnforcesConnectionLimitAndClosesActiveSession)
{
    NoopRenderWorkerRuntime runtime;
    WorkerServerConfiguration configuration;
    configuration.maximumConnections = 1;
    WorkerServer server(runtime, configuration);
    ASSERT_TRUE(server.listen(QHostAddress::LocalHost, 0));

    QTcpSocket first;
    first.connectToHost(QHostAddress::LocalHost, server.serverPort());
    ASSERT_TRUE(waitForServerCondition(
        [&]() { return first.state() == QAbstractSocket::ConnectedState && server.sessionCount() == 1; }));

    QTcpSocket second;
    second.connectToHost(QHostAddress::LocalHost, server.serverPort());
    ASSERT_TRUE(waitForServerCondition([&]() { return second.state() == QAbstractSocket::UnconnectedState; }));
    EXPECT_EQ(1, server.sessionCount());

    server.close();
    EXPECT_FALSE(server.isListening());
    EXPECT_TRUE(waitForServerCondition([&]() { return first.state() == QAbstractSocket::UnconnectedState; }));
}

TEST(WorkerServerTest, DisconnectsIdleSessionAfterConfiguredTimeout)
{
    NoopRenderWorkerRuntime runtime;
    WorkerServerConfiguration configuration;
    configuration.session.inactivityTimeoutMilliseconds = 20;
    WorkerServer server(runtime, configuration);
    ASSERT_TRUE(server.listen(QHostAddress::LocalHost, 0));

    QTcpSocket client;
    client.connectToHost(QHostAddress::LocalHost, server.serverPort());
    ASSERT_TRUE(waitForServerCondition([&]() { return client.state() == QAbstractSocket::ConnectedState; }));

    EXPECT_TRUE(waitForServerCondition([&]() { return client.state() == QAbstractSocket::UnconnectedState; }));
}

}  // namespace
}  // namespace flexraw::worker::network
