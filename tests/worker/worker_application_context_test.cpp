#include <utility>

#include <QHostAddress>
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

}  // namespace
}  // namespace flexraw::worker::app
