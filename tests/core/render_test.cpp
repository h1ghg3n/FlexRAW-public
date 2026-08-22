#include <limits>

#include <QDir>
#include <QTemporaryDir>

#include <gtest/gtest.h>

#include "local_render_executor.h"
#include "resolved_render_pipeline.h"

namespace flexraw::core::render
{
namespace
{

// 목적: 실제 RAW fixture 없이 validation/cancellation 경계를 검증할 기본 요청 생성
// 입력: directory: 유효한 output parent를 제공할 임시 directory
// 출력: 존재하지 않는 RAW source와 유효한 output을 가진 resolved request
[[nodiscard]] ResolvedRenderRequest makeRequest(const QTemporaryDir& directory)
{
    return {
        QDir(directory.path()).filePath(QStringLiteral("missing.cr3")),
        QDir(directory.path()).filePath(QStringLiteral("output.jpg")),
        {},
        {},
    };
}

class RecordingRenderPipeline final : public IResolvedRenderPipeline
{
public:
    // 목적: LocalRenderExecutor가 request와 cancellation state를 그대로 위임하는지 기록
    // 입력: request: 전달된 값, cancellationToken: 전달된 중단 상태
    // 출력: 고정 artifact의 성공 결과
    [[nodiscard]] ResolvedRenderPipelineResult execute(const ResolvedRenderRequest& request,
                                                       const types::CancellationToken& cancellationToken) const override
    {
        called = true;
        observedSourcePath = request.sourcePath;
        observedCancellation = cancellationToken.isCancellationRequested();
        return ResolvedRenderPipelineResult::success({{QStringLiteral("artifact.jpg"), 42}, {}});
    }

    mutable bool called{false};
    mutable bool observedCancellation{false};
    mutable QString observedSourcePath;
};

TEST(ResolvedRenderPipelineTest, RejectsInvalidDevelopParamsBeforeDecode)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    ResolvedRenderRequest request = makeRequest(directory);
    request.developParams.exposureEv = std::numeric_limits<float>::infinity();
    const types::CancellationSource cancellation;

    const ResolvedRenderPipelineResult result = ResolvedRenderPipeline{}.execute(request, cancellation.token());

    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(result.error().cause.code, types::ErrorCode::InvalidArgument);
    EXPECT_EQ(result.error().stats.decodeNanoseconds, 0U);
}

TEST(ResolvedRenderPipelineTest, RejectsInvalidOutputOptionsBeforeDecode)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    ResolvedRenderRequest request = makeRequest(directory);
    request.outputOptions.jpegQuality = 0;
    const types::CancellationSource cancellation;

    const ResolvedRenderPipelineResult result = ResolvedRenderPipeline{}.execute(request, cancellation.token());

    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(result.error().cause.code, types::ErrorCode::InvalidArgument);
    EXPECT_EQ(result.error().stats.decodeNanoseconds, 0U);
}

TEST(ResolvedRenderPipelineTest, RejectsInvalidOutputPathBeforeDecode)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    ResolvedRenderRequest request = makeRequest(directory);
    request.outputPath = QDir(directory.path()).filePath(QStringLiteral("missing/output.jpg"));
    const types::CancellationSource cancellation;

    const ResolvedRenderPipelineResult result = ResolvedRenderPipeline{}.execute(request, cancellation.token());

    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(result.error().cause.code, types::ErrorCode::NotFound);
    EXPECT_EQ(result.error().stats.decodeNanoseconds, 0U);
}

TEST(ResolvedRenderPipelineTest, StopsBeforeDecodeWhenAlreadyCancelled)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const ResolvedRenderRequest request = makeRequest(directory);
    types::CancellationSource cancellation;
    cancellation.requestCancellation();

    const ResolvedRenderPipelineResult result = ResolvedRenderPipeline{}.execute(request, cancellation.token());

    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(result.error().cause.code, types::ErrorCode::Cancelled);
    EXPECT_EQ(result.error().stats.decodeNanoseconds, 0U);
}

TEST(ResolvedRenderPipelineTest, ReportsDecodeFailureWithStageStats)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const ResolvedRenderRequest request = makeRequest(directory);
    const types::CancellationSource cancellation;

    const ResolvedRenderPipelineResult result = ResolvedRenderPipeline{}.execute(request, cancellation.token());

    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(result.error().cause.code, types::ErrorCode::NotFound);
    EXPECT_GE(result.error().stats.totalNanoseconds, result.error().stats.decodeNanoseconds);
}

TEST(LocalRenderExecutorTest, DelegatesResolvedRequestOnCallerThread)
{
    RecordingRenderPipeline pipeline;
    const LocalRenderExecutor executor(pipeline);
    ResolvedRenderRequest request;
    request.sourcePath = QStringLiteral("source.cr3");
    const types::CancellationSource cancellation;

    const ResolvedRenderPipelineResult result = executor.execute(request, cancellation.token());

    ASSERT_TRUE(result.hasValue());
    EXPECT_TRUE(pipeline.called);
    EXPECT_EQ(pipeline.observedSourcePath, request.sourcePath);
    EXPECT_FALSE(pipeline.observedCancellation);
    EXPECT_EQ(result.value().artifact.byteSize, 42U);
}

}  // namespace
}  // namespace flexraw::core::render
