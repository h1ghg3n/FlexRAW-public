#include <utility>

#include <QFileInfo>
#include <QHostAddress>
#include <QProcess>
#include <QTcpServer>
#include <QTemporaryDir>

#include <gtest/gtest.h>

#include "worker_application_context.h"
#include "worker_path_resolver.h"

namespace flexraw::worker::app
{
namespace
{

// 목적: temporary source/output root를 검증된 WorkerPathResolver로 변환
// 입력: sourceRoot/outputRoot: 존재하는 test directory
// 출력: production context에 전달할 resolver
[[nodiscard]] runtime::WorkerPathResolver makeApplicationResolver(const QTemporaryDir& sourceRoot,
                                                                  const QTemporaryDir& outputRoot)
{
    runtime::WorkerPathResolver::CreateResult result =
        runtime::WorkerPathResolver::create({sourceRoot.path(), outputRoot.path()});
    EXPECT_TRUE(result.hasValue());
    return std::move(result.value());
}

TEST(WorkerApplicationContextTest, OwnsListeningServerAndBoundedSchedulerLifecycle)
{
    QTemporaryDir sourceRoot;
    QTemporaryDir outputRoot;
    ASSERT_TRUE(sourceRoot.isValid());
    ASSERT_TRUE(outputRoot.isValid());

    WorkerApplicationConfiguration configuration;
    configuration.maximumConcurrency = 2;
    configuration.queueCapacity = 3;
    WorkerApplicationContext context(makeApplicationResolver(sourceRoot, outputRoot), configuration);

    EXPECT_TRUE(context.runtimeSnapshot().accepting);
    ASSERT_TRUE(context.listen(QHostAddress::LocalHost, 0));
    EXPECT_GT(context.serverPort(), 0);
    EXPECT_FALSE(context.listen(QHostAddress::LocalHost, 0));

    context.shutdown();
    EXPECT_FALSE(context.runtimeSnapshot().accepting);
    EXPECT_FALSE(context.listen(QHostAddress::LocalHost, 0));
}

TEST(WorkerApplicationContextTest, ProcessReportsOccupiedPortAndExitsAfterSafeTeardown)
{
    QTemporaryDir sourceRoot;
    QTemporaryDir outputRoot;
    ASSERT_TRUE(sourceRoot.isValid());
    ASSERT_TRUE(outputRoot.isValid());

    QTcpServer occupiedPort;
    ASSERT_TRUE(occupiedPort.listen(QHostAddress::LocalHost, 0));
    ASSERT_GT(occupiedPort.serverPort(), 0);

    const QString executable = QString::fromUtf8(FLEXRAW_WORKER_EXECUTABLE_PATH);
    ASSERT_TRUE(QFileInfo::exists(executable));

    QProcess process;
    process.setProgram(executable);
    process.setArguments({QStringLiteral("--source-root"),
                          sourceRoot.path(),
                          QStringLiteral("--output-root"),
                          outputRoot.path(),
                          QStringLiteral("--listen-address"),
                          QStringLiteral("127.0.0.1"),
                          QStringLiteral("--port"),
                          QString::number(occupiedPort.serverPort())});
    process.start();
    ASSERT_TRUE(process.waitForStarted(5'000));
    ASSERT_TRUE(process.waitForFinished(10'000));

    EXPECT_EQ(process.exitStatus(), QProcess::NormalExit);
    EXPECT_EQ(process.exitCode(), 3);
    EXPECT_TRUE(process.readAllStandardError().contains("Unable to listen:"));
    EXPECT_TRUE(occupiedPort.isListening());
}

}  // namespace
}  // namespace flexraw::worker::app
