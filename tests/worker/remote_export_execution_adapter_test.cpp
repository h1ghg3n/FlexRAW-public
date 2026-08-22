#include <gtest/gtest.h>

#include <memory>
#include <utility>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QUuid>

#include "remote_export_execution_adapter.h"

namespace flexraw::worker::client
{
namespace
{

// 목적: test storage root에 v1 marker 기록
// 입력: root: marker parent, storageId: 기록할 logical storage UUID
// 출력: marker write 성공 여부
[[nodiscard]] bool writeMarker(const QString& root, const QUuid& storageId)
{
    QFile marker(root + QStringLiteral("/.flexraw-storage.json"));
    if (!marker.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        return false;
    }
    const QJsonObject object{{QStringLiteral("schema"), 1},
                             {QStringLiteral("storage_id"), storageId.toString(QUuid::WithoutBraces)}};
    return marker.write(QJsonDocument(object).toJson(QJsonDocument::Compact)) > 0;
}

class RecordingRemoteExecutor final : public IRemoteRenderExecutor
{
public:
    // 목적: legacy 3-argument 호출도 성공 결과로 기록
    // 입력: endpoint/request/token: Remote 실행 값
    // 출력: deterministic artifact
    [[nodiscard]] RemoteRenderResult execute(const RemoteRenderEndpoint& endpoint,
                                             const RemoteRenderRequest& request,
                                             const core::types::CancellationToken& cancellationToken) const override
    {
        return execute(endpoint, request, cancellationToken, {});
    }

    // 목적: adapter가 전달한 relative request와 accepted observer 기록
    // 입력: endpoint/request/token: 실행 값, accepted: ownership callback
    // 출력: deterministic artifact
    [[nodiscard]] RemoteRenderResult execute(const RemoteRenderEndpoint& endpoint,
                                             const RemoteRenderRequest& request,
                                             const core::types::CancellationToken& cancellationToken,
                                             const RemoteRenderAcceptedCallback& accepted) const override
    {
        static_cast<void>(cancellationToken);
        ++executeCount;
        lastEndpoint = endpoint;
        lastRequest = request;
        if (accepted)
        {
            accepted();
        }
        return RemoteRenderResult::success({{request.outputRelativePath, 42}, {}});
    }

    mutable int executeCount{0};
    mutable RemoteRenderEndpoint lastEndpoint;
    mutable RemoteRenderRequest lastRequest;
};

// 목적: absolute RAW item과 marker profile을 adapter test fixture로 조립
// 입력: sourcePath/outputPath: local path, sourceId/outputId: expected storage identity
// 출력: target과 prepared item pair
[[nodiscard]] std::pair<core::orchestration::ExportRemoteTarget, core::orchestration::PreparedExportItem>
makeOperation(const QString& sourcePath,
              const QString& outputPath,
              const QUuid& sourceId,
              const QUuid& outputId)
{
    core::orchestration::ExportRemoteTarget target;
    target.host = QStringLiteral("192.0.2.10");
    target.port = 47331;
    target.expectedSourceStorageId = sourceId.toString(QUuid::WithoutBraces);
    target.expectedOutputStorageId = outputId.toString(QUuid::WithoutBraces);
    core::orchestration::PreparedExportItem item{
        {core::types::makeFileDescriptor(QFileInfo(sourcePath), core::types::SupportedFileKind::Raw),
         outputPath,
         {},
         core::types::DevelopParams{},
         {}},
        {}};
    return {target, item};
}

TEST(RemoteExportExecutionAdapterTest, MapsSharedStorageAndRestoresDesktopOutputPath)
{
    QTemporaryDir sourceRoot;
    QTemporaryDir outputRoot;
    ASSERT_TRUE(sourceRoot.isValid());
    ASSERT_TRUE(outputRoot.isValid());
    const QUuid sourceId = QUuid::createUuid();
    const QUuid outputId = QUuid::createUuid();
    ASSERT_TRUE(writeMarker(sourceRoot.path(), sourceId));
    ASSERT_TRUE(writeMarker(outputRoot.path(), outputId));
    ASSERT_TRUE(QDir(sourceRoot.path()).mkpath(QStringLiteral("nested")));
    QFile source(sourceRoot.filePath(QStringLiteral("nested/사진.ARW")));
    ASSERT_TRUE(source.open(QIODevice::WriteOnly));
    source.write("raw");
    source.close();
    const QString outputPath = outputRoot.filePath(QStringLiteral("결과.jpg"));
    auto [target, item] = makeOperation(source.fileName(), outputPath, sourceId, outputId);
    auto executor = std::make_unique<RecordingRemoteExecutor>();
    RecordingRemoteExecutor* const recorded = executor.get();
    const RemoteExportExecutionAdapter adapter(std::move(executor));
    const core::types::CancellationSource cancellation;
    int acceptedCount = 0;

    const core::orchestration::RemoteExportExecutionResult result =
        adapter.execute(target, item, cancellation.token(), [&acceptedCount] { ++acceptedCount; });

    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(1, acceptedCount);
    EXPECT_EQ(1, recorded->executeCount);
    EXPECT_EQ(QStringLiteral("nested/사진.ARW"), recorded->lastRequest.sourceRelativePath);
    EXPECT_EQ(QStringLiteral("결과.jpg"), recorded->lastRequest.outputRelativePath);
    EXPECT_EQ(outputPath, result.value().outputPath);
}

TEST(RemoteExportExecutionAdapterTest, RejectsStorageMismatchBeforeExecutorStarts)
{
    QTemporaryDir sourceRoot;
    QTemporaryDir outputRoot;
    ASSERT_TRUE(sourceRoot.isValid());
    ASSERT_TRUE(outputRoot.isValid());
    const QUuid sourceId = QUuid::createUuid();
    const QUuid outputId = QUuid::createUuid();
    ASSERT_TRUE(writeMarker(sourceRoot.path(), sourceId));
    ASSERT_TRUE(writeMarker(outputRoot.path(), outputId));
    QFile source(sourceRoot.filePath(QStringLiteral("input.ARW")));
    ASSERT_TRUE(source.open(QIODevice::WriteOnly));
    source.write("raw");
    source.close();
    auto [target, item] =
        makeOperation(source.fileName(), outputRoot.filePath(QStringLiteral("output.jpg")), QUuid::createUuid(), outputId);
    auto executor = std::make_unique<RecordingRemoteExecutor>();
    RecordingRemoteExecutor* const recorded = executor.get();
    const RemoteExportExecutionAdapter adapter(std::move(executor));
    const core::types::CancellationSource cancellation;

    const core::orchestration::RemoteExportExecutionResult result =
        adapter.execute(target, item, cancellation.token(), {});

    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(core::orchestration::RemoteExportFailureCode::Ineligible, result.error().code);
    EXPECT_EQ(0, recorded->executeCount);
}

}  // namespace
}  // namespace flexraw::worker::client
