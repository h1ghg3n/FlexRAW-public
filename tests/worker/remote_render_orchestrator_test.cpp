#include <memory>
#include <optional>
#include <stdexcept>

#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QUuid>

#include <gtest/gtest.h>

#include "remote_render_orchestrator.h"
#include "shared_storage_locator.h"

namespace flexraw::worker::client
{
namespace
{

// 목적: orchestrator mapping test에 사용할 source file 생성
// 입력: path: 기존 source root 아래 absolute path
// 출력: file 생성 성공 여부
[[nodiscard]] bool createSourceFile(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write("raw") == 3;
}

// 목적: orchestrator preflight fixture root에 schema 1 storage marker 생성
// 입력: rootPath/storageId: marker directory와 expected UUID
// 출력: marker file write 성공 여부
[[nodiscard]] bool writeMarker(const QString& rootPath, const QUuid& storageId)
{
    const QJsonObject object{{QStringLiteral("schema"), 1},
                             {QStringLiteral("storage_id"), storageId.toString(QUuid::WithoutBraces)}};
    QFile marker(QDir(rootPath).filePath(QString::fromLatin1(SharedStorageMarkerFileName)));
    return marker.open(QIODevice::WriteOnly) && marker.write(QJsonDocument(object).toJson(QJsonDocument::Compact)) > 0;
}

// 목적: fake remote execution에 사용할 valid operation 생성
// 입력: sourceStorageId/outputStorageId: profile identity, sourcePath/outputPath: absolute request path
// 출력: loopback profile과 처리값을 포함한 operation
[[nodiscard]] RemoteRenderOperation makeOperation(const QUuid& sourceStorageId,
                                                  const QUuid& outputStorageId,
                                                  const QString& sourcePath,
                                                  const QString& outputPath)
{
    RemoteRenderOperation operation;
    operation.profile.endpoint.host = QStringLiteral("127.0.0.1");
    operation.profile.endpoint.port = 47331;
    operation.profile.expectedSourceStorageId = sourceStorageId;
    operation.profile.expectedOutputStorageId = outputStorageId;
    operation.request.sourcePath = sourcePath;
    operation.request.outputPath = outputPath;
    operation.request.developParams.exposureEv = 0.5F;
    operation.request.outputOptions.jpegQuality = 92;
    return operation;
}

class RecordingRemoteExecutor final : public IRemoteRenderExecutor
{
public:
    // 목적: orchestrator가 전달한 relative request를 기록하고 성공 결과 반환
    // 입력: endpoint/request: 기록할 값, cancellationToken: 미사용
    // 출력: request output relative path를 포함한 성공 결과
    [[nodiscard]] RemoteRenderResult execute(const RemoteRenderEndpoint& endpoint,
                                             const RemoteRenderRequest& request,
                                             const core::types::CancellationToken&) const override
    {
        {
            QMutexLocker lock(&m_mutex);
            m_endpoint = endpoint;
            m_request = request;
        }
        return RemoteRenderResult::success({{request.outputRelativePath, 1234}, {}});
    }

    // 목적: worker thread가 기록한 request snapshot 반환
    // 입력: 없음
    // 출력: execute 전이면 빈 optional, 이후면 RemoteRenderRequest 복사본
    [[nodiscard]] std::optional<RemoteRenderRequest> request() const
    {
        QMutexLocker lock(&m_mutex);
        return m_request;
    }

private:
    mutable QMutex m_mutex;
    mutable RemoteRenderEndpoint m_endpoint;
    mutable std::optional<RemoteRenderRequest> m_request;
};

class BlockingRemoteExecutor final : public IRemoteRenderExecutor
{
public:
    // 목적: cancellation test가 executor 시작을 기다릴 수 있는 semaphore 노출
    // 입력: 없음
    // 출력: started semaphore 참조
    [[nodiscard]] QSemaphore& started() noexcept
    {
        return m_started;
    }

    // 목적: cancellation token 요청까지 대기한 뒤 Cancelled 결과 반환
    // 입력: endpoint/request: 미사용, cancellationToken: 종료 조건
    // 출력: cancellation terminal failure
    [[nodiscard]] RemoteRenderResult execute(const RemoteRenderEndpoint&,
                                             const RemoteRenderRequest&,
                                             const core::types::CancellationToken& cancellationToken) const override
    {
        m_started.release();
        while (!cancellationToken.isCancellationRequested())
        {
            QThread::msleep(1);
        }
        return RemoteRenderResult::failure(
            {RemoteRenderErrorCode::Cancelled,
             {core::types::ErrorCode::Cancelled, QStringLiteral("Fake remote render cancelled.")},
             {},
             std::nullopt});
    }

private:
    mutable QSemaphore m_started;
};

TEST(RemoteRenderOrchestratorTest, MapsRequestOffThreadAndRestoresLocalArtifactPath)
{
    QTemporaryDir sourceRoot;
    QTemporaryDir outputRoot;
    ASSERT_TRUE(sourceRoot.isValid());
    ASSERT_TRUE(outputRoot.isValid());
    ASSERT_TRUE(QDir(sourceRoot.path()).mkpath(QStringLiteral("photos")));
    ASSERT_TRUE(QDir(outputRoot.path()).mkpath(QStringLiteral("exports")));
    const QUuid sourceStorageId = QUuid::createUuid();
    const QUuid outputStorageId = QUuid::createUuid();
    ASSERT_TRUE(writeMarker(sourceRoot.path(), sourceStorageId));
    ASSERT_TRUE(writeMarker(outputRoot.path(), outputStorageId));
    const QString sourcePath = QDir(sourceRoot.path()).filePath(QStringLiteral("photos/input.ARW"));
    const QString outputPath = QDir(outputRoot.path()).filePath(QStringLiteral("exports/output.jpg"));
    ASSERT_TRUE(createSourceFile(sourcePath));

    auto executor = std::make_unique<RecordingRemoteExecutor>();
    RecordingRemoteExecutor* const probe = executor.get();
    RemoteRenderOrchestrator orchestrator(std::move(executor));
    std::optional<RemoteRenderCompletion> completion;
    QEventLoop eventLoop;
    QTimer::singleShot(5000, &eventLoop, &QEventLoop::quit);
    QObject::connect(&orchestrator,
                     &RemoteRenderOrchestrator::remoteRenderCompleted,
                     &eventLoop,
                     [&](const RemoteRenderCompletion& value) {
                         completion = value;
                         eventLoop.quit();
                     });

    const RemoteRenderSubmissionResult submitted =
        orchestrator.submit(makeOperation(sourceStorageId, outputStorageId, sourcePath, outputPath));
    ASSERT_TRUE(submitted.hasValue());
    eventLoop.exec();

    ASSERT_TRUE(completion.has_value());
    EXPECT_EQ(submitted.value(), completion->requestId);
    EXPECT_EQ(outputPath, completion->result.artifact.outputPath);
    EXPECT_EQ(1234U, completion->result.artifact.byteSize);
    const std::optional<RemoteRenderRequest> recorded = probe->request();
    ASSERT_TRUE(recorded.has_value());
    EXPECT_EQ(QStringLiteral("photos/input.ARW"), recorded->sourceRelativePath);
    EXPECT_EQ(QStringLiteral("exports/output.jpg"), recorded->outputRelativePath);
    EXPECT_FLOAT_EQ(0.5F, recorded->developParams.exposureEv);
    EXPECT_EQ(92, recorded->outputOptions.jpegQuality);
}

TEST(RemoteRenderOrchestratorTest, RejectsInvalidLocalMappingBeforeExecutorStarts)
{
    QTemporaryDir sourceRoot;
    QTemporaryDir outputRoot;
    ASSERT_TRUE(sourceRoot.isValid());
    ASSERT_TRUE(outputRoot.isValid());
    const QUuid sourceStorageId = QUuid::createUuid();
    const QUuid outputStorageId = QUuid::createUuid();
    ASSERT_TRUE(writeMarker(sourceRoot.path(), sourceStorageId));
    ASSERT_TRUE(writeMarker(outputRoot.path(), outputStorageId));
    auto executor = std::make_unique<RecordingRemoteExecutor>();
    RecordingRemoteExecutor* const probe = executor.get();
    RemoteRenderOrchestrator orchestrator(std::move(executor));

    const RemoteRenderSubmissionResult submitted =
        orchestrator.submit(makeOperation(sourceStorageId,
                                          outputStorageId,
                                          QDir(sourceRoot.path()).filePath(QStringLiteral("missing.ARW")),
                                          QDir(outputRoot.path()).filePath(QStringLiteral("output.jpg"))));

    ASSERT_TRUE(submitted.hasError());
    EXPECT_EQ(RemoteRenderPreflightErrorCode::PathMappingFailed, submitted.error().code);
    EXPECT_EQ(core::types::ErrorCode::NotFound, submitted.error().cause.code);
    EXPECT_FALSE(probe->request().has_value());
}

TEST(RemoteRenderOrchestratorTest, RejectsMissingMarkerBeforeExecutorStarts)
{
    QTemporaryDir sourceRoot;
    QTemporaryDir outputRoot;
    ASSERT_TRUE(sourceRoot.isValid());
    ASSERT_TRUE(outputRoot.isValid());
    const QUuid sourceStorageId = QUuid::createUuid();
    const QUuid outputStorageId = QUuid::createUuid();
    ASSERT_TRUE(writeMarker(outputRoot.path(), outputStorageId));
    const QString sourcePath = QDir(sourceRoot.path()).filePath(QStringLiteral("input.ARW"));
    ASSERT_TRUE(createSourceFile(sourcePath));
    auto executor = std::make_unique<RecordingRemoteExecutor>();
    RecordingRemoteExecutor* const probe = executor.get();
    RemoteRenderOrchestrator orchestrator(std::move(executor));

    const RemoteRenderSubmissionResult submitted = orchestrator.submit(makeOperation(
        sourceStorageId, outputStorageId, sourcePath, QDir(outputRoot.path()).filePath(QStringLiteral("output.jpg"))));

    ASSERT_TRUE(submitted.hasError());
    EXPECT_EQ(RemoteRenderPreflightErrorCode::NoStorageMarker, submitted.error().code);
    EXPECT_FALSE(probe->request().has_value());
}

TEST(RemoteRenderOrchestratorTest, RejectsSourceAndOutputStorageMismatchBeforeExecutorStarts)
{
    QTemporaryDir sourceRoot;
    QTemporaryDir outputRoot;
    ASSERT_TRUE(sourceRoot.isValid());
    ASSERT_TRUE(outputRoot.isValid());
    const QUuid sourceStorageId = QUuid::createUuid();
    const QUuid outputStorageId = QUuid::createUuid();
    ASSERT_TRUE(writeMarker(sourceRoot.path(), sourceStorageId));
    ASSERT_TRUE(writeMarker(outputRoot.path(), outputStorageId));
    const QString sourcePath = QDir(sourceRoot.path()).filePath(QStringLiteral("input.ARW"));
    const QString outputPath = QDir(outputRoot.path()).filePath(QStringLiteral("output.jpg"));
    ASSERT_TRUE(createSourceFile(sourcePath));
    auto executor = std::make_unique<RecordingRemoteExecutor>();
    RecordingRemoteExecutor* const probe = executor.get();
    RemoteRenderOrchestrator orchestrator(std::move(executor));

    const RemoteRenderSubmissionResult sourceMismatch =
        orchestrator.submit(makeOperation(QUuid::createUuid(), outputStorageId, sourcePath, outputPath));
    const RemoteRenderSubmissionResult outputMismatch =
        orchestrator.submit(makeOperation(sourceStorageId, QUuid::createUuid(), sourcePath, outputPath));

    ASSERT_TRUE(sourceMismatch.hasError());
    EXPECT_EQ(RemoteRenderPreflightErrorCode::StorageMismatch, sourceMismatch.error().code);
    ASSERT_TRUE(outputMismatch.hasError());
    EXPECT_EQ(RemoteRenderPreflightErrorCode::StorageMismatch, outputMismatch.error().code);
    EXPECT_FALSE(probe->request().has_value());
}

TEST(RemoteRenderOrchestratorTest, PreservesMapperValidationAfterMarkerPreflight)
{
    QTemporaryDir sourceRoot;
    QTemporaryDir outputRoot;
    ASSERT_TRUE(sourceRoot.isValid());
    ASSERT_TRUE(outputRoot.isValid());
    const QUuid sourceStorageId = QUuid::createUuid();
    const QUuid outputStorageId = QUuid::createUuid();
    ASSERT_TRUE(writeMarker(sourceRoot.path(), sourceStorageId));
    ASSERT_TRUE(writeMarker(outputRoot.path(), outputStorageId));
    const QString sourcePath = QDir(sourceRoot.path()).filePath(QStringLiteral("input.ARW"));
    const QString outputPath = QDir(outputRoot.path()).filePath(QStringLiteral("existing-directory.jpg"));
    ASSERT_TRUE(createSourceFile(sourcePath));
    ASSERT_TRUE(QDir(outputRoot.path()).mkdir(QStringLiteral("existing-directory.jpg")));
    auto executor = std::make_unique<RecordingRemoteExecutor>();
    RecordingRemoteExecutor* const probe = executor.get();
    RemoteRenderOrchestrator orchestrator(std::move(executor));

    const RemoteRenderSubmissionResult submitted =
        orchestrator.submit(makeOperation(sourceStorageId, outputStorageId, sourcePath, outputPath));

    ASSERT_TRUE(submitted.hasError());
    EXPECT_EQ(RemoteRenderPreflightErrorCode::PathMappingFailed, submitted.error().code);
    EXPECT_FALSE(probe->request().has_value());
}

TEST(RemoteRenderOrchestratorTest, CancelsAcceptedOperationOnce)
{
    QTemporaryDir sourceRoot;
    QTemporaryDir outputRoot;
    ASSERT_TRUE(sourceRoot.isValid());
    ASSERT_TRUE(outputRoot.isValid());
    const QUuid sourceStorageId = QUuid::createUuid();
    const QUuid outputStorageId = QUuid::createUuid();
    ASSERT_TRUE(writeMarker(sourceRoot.path(), sourceStorageId));
    ASSERT_TRUE(writeMarker(outputRoot.path(), outputStorageId));
    const QString sourcePath = QDir(sourceRoot.path()).filePath(QStringLiteral("input.ARW"));
    const QString outputPath = QDir(outputRoot.path()).filePath(QStringLiteral("output.jpg"));
    ASSERT_TRUE(createSourceFile(sourcePath));
    auto executor = std::make_unique<BlockingRemoteExecutor>();
    BlockingRemoteExecutor* const probe = executor.get();
    RemoteRenderOrchestrator orchestrator(std::move(executor));
    std::optional<core::types::RequestId> cancelled;
    QObject::connect(&orchestrator,
                     &RemoteRenderOrchestrator::remoteRenderCancelled,
                     [&](const core::types::RequestId requestId) { cancelled = requestId; });

    const RemoteRenderSubmissionResult submitted =
        orchestrator.submit(makeOperation(sourceStorageId, outputStorageId, sourcePath, outputPath));
    ASSERT_TRUE(submitted.hasValue());
    ASSERT_TRUE(probe->started().tryAcquire(1, 5000));

    EXPECT_TRUE(orchestrator.cancel(submitted.value()));
    EXPECT_FALSE(orchestrator.cancel(submitted.value()));
    ASSERT_TRUE(cancelled.has_value());
    EXPECT_EQ(submitted.value(), *cancelled);
}

TEST(RemoteRenderOrchestratorTest, RejectsNullExecutorAndInvalidConcurrency)
{
    EXPECT_THROW(RemoteRenderOrchestrator(nullptr), std::invalid_argument);
    EXPECT_THROW(RemoteRenderOrchestrator(std::make_unique<RecordingRemoteExecutor>(), 0), std::invalid_argument);
}

}  // namespace
}  // namespace flexraw::worker::client
