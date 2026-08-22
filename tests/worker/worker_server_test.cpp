#include <functional>
#include <stdexcept>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QHostAddress>
#include <QTcpSocket>
#include <QTemporaryDir>
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

class NoopRenderJobRunner final : public runtime::IRenderJobRunner
{
public:
    // 목적: WorkerServer connection lifecycle test용 즉시 취소 결과 반환
    // 입력: request/cancellationToken: 사용하지 않는 runner contract
    // 출력: 고정 Cancelled failure
    [[nodiscard]] runtime::RenderJobExecutionResult execute(
        const core::render::ResolvedRenderRequest& request,
        const core::types::CancellationToken& cancellationToken) const override
    {
        static_cast<void>(request);
        static_cast<void>(cancellationToken);
        return runtime::RenderJobExecutionResult::success(core::render::ResolvedRenderPipelineResult::failure(
            {{core::types::ErrorCode::Cancelled, QStringLiteral("not used")}, {}}));
    }
};

// 목적: temporary root pair로 WorkerPathResolver 생성
// 입력: sourceRoot/outputRoot: 기존 directory
// 출력: resolver 또는 setup 예외
[[nodiscard]] runtime::WorkerPathResolver makeServerResolver(const QTemporaryDir& sourceRoot,
                                                             const QTemporaryDir& outputRoot)
{
    runtime::WorkerPathResolver::CreateResult result =
        runtime::WorkerPathResolver::create({sourceRoot.path(), outputRoot.path()});
    if (result.hasError())
    {
        throw std::runtime_error(result.error().message.toStdString());
    }
    return result.value();
}

TEST(WorkerServerTest, EnforcesConnectionLimitAndClosesActiveSession)
{
    QTemporaryDir sourceRoot;
    QTemporaryDir outputRoot;
    ASSERT_TRUE(sourceRoot.isValid());
    ASSERT_TRUE(outputRoot.isValid());
    const runtime::WorkerPathResolver resolver = makeServerResolver(sourceRoot, outputRoot);
    NoopRenderJobRunner runner;
    runtime::JobScheduler scheduler(runner, 1, 0);
    WorkerServerConfiguration configuration;
    configuration.maximumConnections = 1;
    WorkerServer server(resolver, scheduler, configuration);
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
    QTemporaryDir sourceRoot;
    QTemporaryDir outputRoot;
    ASSERT_TRUE(sourceRoot.isValid());
    ASSERT_TRUE(outputRoot.isValid());
    const runtime::WorkerPathResolver resolver = makeServerResolver(sourceRoot, outputRoot);
    NoopRenderJobRunner runner;
    runtime::JobScheduler scheduler(runner, 1, 0);
    WorkerServerConfiguration configuration;
    configuration.session.inactivityTimeoutMilliseconds = 20;
    WorkerServer server(resolver, scheduler, configuration);
    ASSERT_TRUE(server.listen(QHostAddress::LocalHost, 0));

    QTcpSocket client;
    client.connectToHost(QHostAddress::LocalHost, server.serverPort());
    ASSERT_TRUE(waitForServerCondition([&]() { return client.state() == QAbstractSocket::ConnectedState; }));

    EXPECT_TRUE(waitForServerCondition([&]() { return client.state() == QAbstractSocket::UnconnectedState; }));
}

}  // namespace
}  // namespace flexraw::worker::network
