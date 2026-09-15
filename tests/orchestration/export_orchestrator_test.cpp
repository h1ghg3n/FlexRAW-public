#include <algorithm>
#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QImage>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <gtest/gtest.h>

#include "catalog_database.h"
#include "export_orchestrator.h"
#include "export_pipeline.h"
#include "test_application.h"
#include "test_path_identity_service.h"

namespace flexraw::core::orchestration
{
namespace
{

// 목적: worker 동작을 기다리는 동안 Qt queued callback도 함께 처리
// 입력: predicate: 완료 조건, timeoutMilliseconds: 최대 대기 시간
// 출력: 제한 시간 안에 조건이 충족되면 true
[[nodiscard]] bool waitForCondition(const std::function<bool()>& predicate, const int timeoutMilliseconds = 5000)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMilliseconds)
    {
        if (predicate())
        {
            return true;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    return predicate();
}

// 목적: fake pipeline contract test용 단일 raster export 요청 생성
// 입력: sourcePath: request와 fake 동작을 구분할 source 식별자
// 출력: 즉시 validation을 통과하는 ExportRequest 값
[[nodiscard]] ExportRequest makeFileRequest(const QString& sourcePath)
{
    const types::FileDescriptor source{
        sourcePath,
        QStringLiteral("jpg"),
        sourcePath,
        types::SupportedFileKind::RasterImage,
    };
    return ExportFileRequest{source, sourcePath + QStringLiteral(".out.jpg"), {}, {}, {}};
}

// 목적: FileExportPipeline path preparation test용 작은 raster source 생성
// 입력: path: 저장할 PNG file 경로
// 출력: image plugin 저장 성공 여부
[[nodiscard]] bool writeRasterSource(const QString& path)
{
    QImage image(QSize(2, 2), QImage::Format_RGB32);
    image.fill(Qt::darkGreen);
    return image.save(path);
}

// 목적: fake pipeline 성공 report 생성
// 입력: sourcePath: 결과 item의 source identity
// 출력: 한 item이 성공한 ExportPipelineResult
[[nodiscard]] ExportPipelineResult makeSuccessfulResult(const QString& sourcePath)
{
    ExportReport report;
    report.totalCount = 1;
    report.succeededCount = 1;
    report.items.push_back({sourcePath, sourcePath + QStringLiteral(".out.jpg"), true, {}});
    return ExportPipelineResult::success(std::move(report));
}

class RecordingExportPipeline final : public IExportPipeline
{
public:
    // 목적: worker thread와 progress callback 동작을 기록하는 deterministic export 실행
    // 입력: request: source identity용 요청, cancellationToken: 미사용, progress: 검증할 callback
    // 출력: 한 item 성공 report
    [[nodiscard]] ExportPipelineResult execute(const ExportRequest& request,
                                               const types::CancellationToken&,
                                               const ExportProgressCallback& progress) const override
    {
        executionThread.store(QThread::currentThread(), std::memory_order_relaxed);
        const QString sourcePath = std::get<ExportFileRequest>(request).source.path;
        progress({1, 1, 1, 0, sourcePath});
        return makeSuccessfulResult(sourcePath);
    }

    mutable std::atomic<QThread*> executionThread{nullptr};
};

class CountingExportPipeline final : public IExportPipeline
{
public:
    // 목적: submit validation이 invalid request를 pipeline에 전달하지 않는지 기록
    // 입력: request/token/progress: 사용하지 않는 contract 값
    // 출력: 호출되면 빈 성공 report
    [[nodiscard]] ExportPipelineResult execute(const ExportRequest&,
                                               const types::CancellationToken&,
                                               const ExportProgressCallback&) const override
    {
        callCount.fetch_add(1, std::memory_order_relaxed);
        return ExportPipelineResult::success({});
    }

    mutable std::atomic_int callCount{0};
};

class UnavailablePathIdentityService final : public platform::IPathIdentityService
{
public:
    // 목적: OS별 path identity를 확정할 수 없는 Platform 경계 재현
    // 입력: absolutePath: 판정을 거부할 미사용 절대 경로
    // 출력: identity unavailable을 뜻하는 nullopt
    [[nodiscard]] std::optional<platform::TransientPathKey> comparisonKey(const std::filesystem::path&) const override
    {
        return std::nullopt;
    }
};

struct CancellationProbe
{
    QSemaphore blockedJobStarted;
    QSemaphore cancelledJobFinished;
};

class IndependentlyCancellableExportPipeline final : public IExportPipeline
{
public:
    // 목적: request cancellation 격리 검증용 shared synchronization state 저장
    // 입력: probe: worker 시작과 종료를 관찰할 semaphore 묶음
    // 출력: deterministic fake pipeline 객체
    explicit IndependentlyCancellableExportPipeline(std::shared_ptr<CancellationProbe> probe)
        : m_probe(std::move(probe))
    {}

    // 목적: source별로 blocking과 즉시 성공 동작을 나눠 request cancellation 격리 검증
    // 입력: request: 동작을 선택할 source, cancellationToken: blocked job 종료 조건, progress: 미사용
    // 출력: blocked job은 Cancelled, 나머지는 성공 report
    [[nodiscard]] ExportPipelineResult execute(const ExportRequest& request,
                                               const types::CancellationToken& cancellationToken,
                                               const ExportProgressCallback&) const override
    {
        const QString sourcePath = std::get<ExportFileRequest>(request).source.path;
        if (sourcePath != QStringLiteral("blocked.jpg"))
        {
            return makeSuccessfulResult(sourcePath);
        }

        m_probe->blockedJobStarted.release();
        while (!cancellationToken.isCancellationRequested())
        {
            QThread::msleep(1);
        }
        m_probe->cancelledJobFinished.release();
        return ExportPipelineResult::failure({types::ErrorCode::Cancelled, QStringLiteral("Fake export cancelled.")});
    }

private:
    std::shared_ptr<CancellationProbe> m_probe;
};

class FirstPreparationBlockingPipeline final : public IExportPipeline
{
public:
    // 목적: 첫 preparation만 test가 해제할 때까지 차단하고 이후 request는 즉시 준비
    // 입력: request: single-file Export intent, cancellationToken: 첫 request 취소 상태
    // 출력: 취소된 첫 preparation 또는 실행 가능한 단일 item
    [[nodiscard]] ExportPreparationResult prepare(const ExportRequest& request,
                                                  const types::CancellationToken& cancellationToken) const override
    {
        const int callIndex = m_preparationCalls.fetch_add(1, std::memory_order_relaxed);
        if (callIndex == 0)
        {
            m_firstPreparationStarted.release();
            m_firstPreparationRelease.acquire();
            if (cancellationToken.isCancellationRequested())
            {
                return ExportPreparationResult::failure(
                    {types::ErrorCode::Cancelled, QStringLiteral("Fake preparation cancelled.")});
            }
        }

        const auto* const fileRequest = std::get_if<ExportFileRequest>(&request);
        if (fileRequest == nullptr)
        {
            return ExportPreparationResult::failure(
                {types::ErrorCode::InvalidArgument, QStringLiteral("Fake pipeline requires a file request.")});
        }
        return ExportPreparationResult::success(PreparedExport{{PreparedExportItem{*fileRequest, std::nullopt}}});
    }

    // 목적: 준비가 끝난 독립 request를 즉시 성공시켜 callback drain 순서 증명
    // 입력: item: 준비된 file intent, cancellationToken: cooperative cancellation 상태
    // 출력: 취소 또는 성공 item result
    [[nodiscard]] ExportItemResult executeItem(const PreparedExportItem& item,
                                               const types::CancellationToken& cancellationToken) const override
    {
        m_executionCalls.fetch_add(1, std::memory_order_relaxed);
        if (cancellationToken.isCancellationRequested())
        {
            return {item.request.source.path,
                    item.request.outputPath,
                    false,
                    {types::ErrorCode::Cancelled, QStringLiteral("Fake execution cancelled.")},
                    ExportItemFailureKind::Execution};
        }
        return {item.request.source.path, item.request.outputPath, true, {}, ExportItemFailureKind::None};
    }

    // 목적: item API만 사용하는 test fake에서 legacy entry point 호출 방지
    // 입력: request/token/progress: 사용하지 않음
    // 출력: 호출되면 식별 가능한 failure
    [[nodiscard]] ExportPipelineResult execute(const ExportRequest&,
                                               const types::CancellationToken&,
                                               const ExportProgressCallback&) const override
    {
        return ExportPipelineResult::failure(
            {types::ErrorCode::Unknown, QStringLiteral("Legacy execute must not be used by this test.")});
    }

    // 목적: 첫 preparation이 worker에서 시작됐는지 조회
    // 입력: 없음
    // 출력: started semaphore 참조
    [[nodiscard]] QSemaphore& firstPreparationStarted() const
    {
        return m_firstPreparationStarted;
    }

    // 목적: 차단된 첫 preparation을 취소 결과 반환 지점까지 진행
    // 입력: 없음
    // 출력: release semaphore 증가
    void releaseFirstPreparation() const
    {
        m_firstPreparationRelease.release();
    }

    // 목적: 실제 item execution 횟수 조회
    // 입력: 없음
    // 출력: thread-safe execution count
    [[nodiscard]] int executionCalls() const
    {
        return m_executionCalls.load(std::memory_order_relaxed);
    }

private:
    mutable std::atomic_int m_preparationCalls{0};
    mutable std::atomic_int m_executionCalls{0};
    mutable QSemaphore m_firstPreparationStarted;
    mutable QSemaphore m_firstPreparationRelease;
};

TEST(ExportOrchestratorTest, RejectsNullPipeline)
{
    EXPECT_THROW(ExportOrchestrator(nullptr), std::invalid_argument);
}

TEST(ExportOrchestratorTest, RunsPipelineOffThreadAndPublishesProgressAndCompletion)
{
    QCoreApplication& application = test::application();
    auto pipeline = std::make_unique<RecordingExportPipeline>();
    RecordingExportPipeline* const pipelineProbe = pipeline.get();
    ExportOrchestrator orchestrator(std::move(pipeline));
    QEventLoop eventLoop;
    QTimer timeoutTimer;
    timeoutTimer.setSingleShot(true);
    QObject::connect(&timeoutTimer, &QTimer::timeout, &eventLoop, &QEventLoop::quit);
    std::vector<ExportProgress> progressEvents;
    std::optional<ExportResult> completedResult;
    QThread* completionThread = nullptr;
    QObject::connect(&orchestrator, &ExportOrchestrator::exportProgressed, [&](const ExportProgress& progress) {
        progressEvents.push_back(progress);
    });
    QObject::connect(&orchestrator, &ExportOrchestrator::exportCompleted, [&](const ExportResult& result) {
        completedResult = result;
        completionThread = QThread::currentThread();
        eventLoop.quit();
    });

    const ExportSubmissionResult submitted = orchestrator.submitExport(makeFileRequest(QStringLiteral("photo.jpg")));
    ASSERT_TRUE(submitted.hasValue());
    timeoutTimer.start(5000);
    eventLoop.exec();

    ASSERT_TRUE(completedResult.has_value());
    EXPECT_EQ(submitted.value(), completedResult->requestId);
    EXPECT_EQ(1, completedResult->report.succeededCount);
    ASSERT_EQ(1U, progressEvents.size());
    EXPECT_EQ(submitted.value(), progressEvents.front().requestId);
    EXPECT_EQ(1, progressEvents.front().completedCount);
    EXPECT_NE(application.thread(), pipelineProbe->executionThread.load(std::memory_order_relaxed));
    EXPECT_EQ(application.thread(), completionThread);
}

TEST(ExportOrchestratorTest, RejectsInvalidRequestBeforeQueueing)
{
    (void)test::application();
    auto pipeline = std::make_unique<CountingExportPipeline>();
    CountingExportPipeline* const pipelineProbe = pipeline.get();
    ExportOrchestrator orchestrator(std::move(pipeline));
    ExportRequest request = ExportFileRequest{};

    const ExportSubmissionResult submitted = orchestrator.submitExport(std::move(request));

    ASSERT_TRUE(submitted.hasError());
    EXPECT_EQ(types::ErrorCode::InvalidArgument, submitted.error().code);
    EXPECT_EQ(0, pipelineProbe->callCount.load(std::memory_order_relaxed));
}

TEST(ExportOrchestratorTest, RejectsEmptyItemListBeforeQueueing)
{
    (void)test::application();
    auto pipeline = std::make_unique<CountingExportPipeline>();
    CountingExportPipeline* const pipelineProbe = pipeline.get();
    ExportOrchestrator orchestrator(std::move(pipeline));
    ExportRequest request = ExportItemListRequest{};

    const ExportSubmissionResult submitted = orchestrator.submitExport(std::move(request));

    ASSERT_TRUE(submitted.hasError());
    EXPECT_EQ(types::ErrorCode::InvalidArgument, submitted.error().code);
    EXPECT_EQ(0, pipelineProbe->callCount.load(std::memory_order_relaxed));
}

TEST(ExportOrchestratorTest, RejectsDuplicateItemListOutputsBeforeQueueing)
{
    (void)test::application();
    auto pipeline = std::make_unique<CountingExportPipeline>();
    CountingExportPipeline* const pipelineProbe = pipeline.get();
    ExportOrchestrator orchestrator(std::move(pipeline));
    ExportFileRequest first = std::get<ExportFileRequest>(makeFileRequest(QStringLiteral("first.jpg")));
    ExportFileRequest second = std::get<ExportFileRequest>(makeFileRequest(QStringLiteral("second.jpg")));
    second.outputPath = first.outputPath;
    ExportRequest request = ExportItemListRequest{{first, second}};

    const ExportSubmissionResult submitted = orchestrator.submitExport(std::move(request));

    ASSERT_TRUE(submitted.hasError());
    EXPECT_EQ(types::ErrorCode::Conflict, submitted.error().code);
    EXPECT_EQ(0, pipelineProbe->callCount.load(std::memory_order_relaxed));
}

TEST(FileExportPipelineTest, UsesDefaultDevelopForUnregisteredFolderSelectionWhenAllowed)
{
    (void)test::application();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("managed.flexraw-catalog"));
    ASSERT_TRUE(catalog::CatalogDatabase::open(catalogPath).hasValue());
    ExportFileRequest item{
        {QDir(directory.path()).filePath(QStringLiteral("unregistered.arw")),
         QStringLiteral("arw"),
         QStringLiteral("unregistered.arw"),
         types::SupportedFileKind::Raw},
        QDir(directory.path()).filePath(QStringLiteral("unregistered.jpg")),
        catalogPath,
        std::nullopt,
        {},
        true,
    };
    FileExportPipeline pipeline(::flexraw::test::testPathIdentityService());
    types::CancellationSource cancellation;

    const ExportPreparationResult prepared = pipeline.prepare(ExportItemListRequest{{item}}, cancellation.token());

    ASSERT_TRUE(prepared.hasValue());
    ASSERT_EQ(1, prepared.value().items.size());
    EXPECT_TRUE(prepared.value().items.constFirst().request.catalogPath.isEmpty());
    ASSERT_TRUE(prepared.value().items.constFirst().request.developParams.has_value());
    EXPECT_EQ(types::DevelopParams{}, *prepared.value().items.constFirst().request.developParams);
}

TEST(FileExportPipelineTest, RejectsItemOutputThatTargetsAnotherItemSource)
{
    (void)test::application();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString firstSource = QDir(directory.path()).filePath(QStringLiteral("first.png"));
    const QString secondSource = QDir(directory.path()).filePath(QStringLiteral("second.png"));
    ASSERT_TRUE(writeRasterSource(firstSource));
    ASSERT_TRUE(writeRasterSource(secondSource));
    ExportFileRequest first = std::get<ExportFileRequest>(makeFileRequest(firstSource));
    ExportFileRequest second = std::get<ExportFileRequest>(makeFileRequest(secondSource));
    first.outputPath = secondSource;
    FileExportPipeline pipeline(::flexraw::test::testPathIdentityService());
    types::CancellationSource cancellation;

    const ExportPreparationResult prepared =
        pipeline.prepare(ExportItemListRequest{{first, second}}, cancellation.token());

    ASSERT_TRUE(prepared.hasError());
    EXPECT_EQ(types::ErrorCode::Conflict, prepared.error().code);
}

TEST(FileExportPipelineTest, RejectsBatchGeneratedOutputThatTargetsScannedSource)
{
    (void)test::application();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    ASSERT_TRUE(writeRasterSource(QDir(directory.path()).filePath(QStringLiteral("photo.png"))));
    ASSERT_TRUE(writeRasterSource(QDir(directory.path()).filePath(QStringLiteral("photo-png.png"))));
    ExportBatchRequest request;
    request.inputFolderPath = directory.path();
    request.outputFolderPath = directory.path();
    request.options.format = export_::RasterExportFormat::Png;
    FileExportPipeline pipeline(::flexraw::test::testPathIdentityService());
    types::CancellationSource cancellation;

    const ExportPreparationResult prepared = pipeline.prepare(request, cancellation.token());

    ASSERT_TRUE(prepared.hasError());
    EXPECT_EQ(types::ErrorCode::Conflict, prepared.error().code);
}

TEST(FileExportPipelineTest, RejectsHardLinkOutputAliasWhenSupported)
{
    (void)test::application();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString sourcePath = QDir(directory.path()).filePath(QStringLiteral("source.png"));
    const QString outputAliasPath = QDir(directory.path()).filePath(QStringLiteral("alias.png"));
    ASSERT_TRUE(writeRasterSource(sourcePath));
    std::error_code linkError;
    std::filesystem::create_hard_link(QFileInfo(sourcePath).filesystemAbsoluteFilePath(),
                                      QFileInfo(outputAliasPath).filesystemAbsoluteFilePath(),
                                      linkError);
    if (linkError)
    {
        GTEST_SKIP() << "Hard link creation is unavailable: " << linkError.message();
    }
    ExportFileRequest request = std::get<ExportFileRequest>(makeFileRequest(sourcePath));
    request.outputPath = outputAliasPath;
    FileExportPipeline pipeline(::flexraw::test::testPathIdentityService());
    types::CancellationSource cancellation;

    const ExportPreparationResult prepared = pipeline.prepare(request, cancellation.token());

    ASSERT_TRUE(prepared.hasError());
    EXPECT_EQ(types::ErrorCode::Conflict, prepared.error().code);
}

TEST(FileExportPipelineTest, RejectsProspectiveOutputCollisionUnderCaseInsensitivePolicy)
{
    (void)test::application();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString firstSource = QDir(directory.path()).filePath(QStringLiteral("first.png"));
    const QString secondSource = QDir(directory.path()).filePath(QStringLiteral("second.png"));
    ASSERT_TRUE(writeRasterSource(firstSource));
    ASSERT_TRUE(writeRasterSource(secondSource));
    ExportFileRequest first = std::get<ExportFileRequest>(makeFileRequest(firstSource));
    ExportFileRequest second = std::get<ExportFileRequest>(makeFileRequest(secondSource));
    first.outputPath = QDir(directory.path()).filePath(QStringLiteral("Result.JPG"));
    second.outputPath = QDir(directory.path()).filePath(QStringLiteral("result.jpg"));
    FileExportPipeline pipeline(::flexraw::test::caseInsensitiveTestPathIdentityService());
    types::CancellationSource cancellation;

    const ExportPreparationResult prepared =
        pipeline.prepare(ExportItemListRequest{{first, second}}, cancellation.token());

    ASSERT_TRUE(prepared.hasError());
    EXPECT_EQ(types::ErrorCode::Conflict, prepared.error().code);
}

TEST(FileExportPipelineTest, AllowsCaseDistinctProspectiveOutputsUnderExactPolicy)
{
    (void)test::application();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString firstSource = QDir(directory.path()).filePath(QStringLiteral("first.png"));
    const QString secondSource = QDir(directory.path()).filePath(QStringLiteral("second.png"));
    ASSERT_TRUE(writeRasterSource(firstSource));
    ASSERT_TRUE(writeRasterSource(secondSource));
    ExportFileRequest first = std::get<ExportFileRequest>(makeFileRequest(firstSource));
    ExportFileRequest second = std::get<ExportFileRequest>(makeFileRequest(secondSource));
    first.outputPath = QDir(directory.path()).filePath(QStringLiteral("Result.JPG"));
    second.outputPath = QDir(directory.path()).filePath(QStringLiteral("result.jpg"));
    FileExportPipeline pipeline(::flexraw::test::testPathIdentityService());
    types::CancellationSource cancellation;

    const ExportPreparationResult prepared =
        pipeline.prepare(ExportItemListRequest{{first, second}}, cancellation.token());

    ASSERT_TRUE(prepared.hasValue());
    ASSERT_EQ(2, prepared.value().items.size());
}

TEST(FileExportPipelineTest, FailsClosedWhenProspectivePathIdentityIsUnavailable)
{
    (void)test::application();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString sourcePath = QDir(directory.path()).filePath(QStringLiteral("source.png"));
    ASSERT_TRUE(writeRasterSource(sourcePath));
    const UnavailablePathIdentityService pathIdentityService;
    FileExportPipeline pipeline(pathIdentityService);
    types::CancellationSource cancellation;

    const ExportPreparationResult prepared = pipeline.prepare(makeFileRequest(sourcePath), cancellation.token());

    ASSERT_TRUE(prepared.hasError());
    EXPECT_EQ(types::ErrorCode::Unknown, prepared.error().code);
}

TEST(ExportOrchestratorTest, RejectsInvalidExportOptionsBeforeQueueing)
{
    (void)test::application();
    auto pipeline = std::make_unique<CountingExportPipeline>();
    CountingExportPipeline* const pipelineProbe = pipeline.get();
    ExportOrchestrator orchestrator(std::move(pipeline));
    ExportRequest request = makeFileRequest(QStringLiteral("photo.jpg"));
    std::get<ExportFileRequest>(request).options.jpegQuality = 0;

    const ExportSubmissionResult submitted = orchestrator.submitExport(std::move(request));

    ASSERT_TRUE(submitted.hasError());
    EXPECT_EQ(types::ErrorCode::InvalidArgument, submitted.error().code);
    EXPECT_EQ(0, pipelineProbe->callCount.load(std::memory_order_relaxed));
}

TEST(ExportOrchestratorTest, CancelsOneRequestWithoutSuppressingIndependentCompletion)
{
    (void)test::application();
    auto probe = std::make_shared<CancellationProbe>();
    ExportOrchestrator orchestrator(std::make_unique<IndependentlyCancellableExportPipeline>(probe), 2);
    QEventLoop eventLoop;
    QTimer timeoutTimer;
    timeoutTimer.setSingleShot(true);
    QObject::connect(&timeoutTimer, &QTimer::timeout, &eventLoop, &QEventLoop::quit);
    std::vector<types::RequestId> completedRequests;
    std::vector<types::RequestId> cancelledRequests;
    QObject::connect(&orchestrator, &ExportOrchestrator::exportCompleted, [&](const ExportResult& result) {
        completedRequests.push_back(result.requestId);
        eventLoop.quit();
    });
    QObject::connect(&orchestrator, &ExportOrchestrator::exportCancelled, [&](types::RequestId requestId) {
        cancelledRequests.push_back(requestId);
    });

    const ExportSubmissionResult blocked = orchestrator.submitExport(makeFileRequest(QStringLiteral("blocked.jpg")));
    ASSERT_TRUE(blocked.hasValue());
    ASSERT_TRUE(waitForCondition([&]() { return probe->blockedJobStarted.tryAcquire(1); }));
    const ExportSubmissionResult independent =
        orchestrator.submitExport(makeFileRequest(QStringLiteral("independent.jpg")));
    ASSERT_TRUE(independent.hasValue());
    EXPECT_TRUE(orchestrator.cancelExport(blocked.value()));
    timeoutTimer.start(5000);
    eventLoop.exec();

    ASSERT_EQ(1U, cancelledRequests.size());
    EXPECT_EQ(blocked.value(), cancelledRequests.front());
    ASSERT_EQ(1U, completedRequests.size());
    EXPECT_EQ(independent.value(), completedRequests.front());
    EXPECT_TRUE(waitForCondition([&]() { return probe->cancelledJobFinished.tryAcquire(1); }));
}

TEST(ExportOrchestratorTest, CancellationDuringPreparationDrainsWithoutLateEvents)
{
    (void)test::application();
    auto pipeline = std::make_unique<FirstPreparationBlockingPipeline>();
    FirstPreparationBlockingPipeline* const pipelineProbe = pipeline.get();
    ExportOrchestrator orchestrator(std::move(pipeline), 1);
    std::vector<ExportProgress> progressEvents;
    std::vector<ExportResult> completedEvents;
    std::vector<ExportIssue> failedEvents;
    std::vector<types::RequestId> cancelledEvents;
    QObject::connect(&orchestrator, &ExportOrchestrator::exportProgressed, [&](const ExportProgress& progress) {
        progressEvents.push_back(progress);
    });
    QObject::connect(&orchestrator, &ExportOrchestrator::exportCompleted, [&](const ExportResult& result) {
        completedEvents.push_back(result);
    });
    QObject::connect(&orchestrator, &ExportOrchestrator::exportFailed, [&](const ExportIssue& issue) {
        failedEvents.push_back(issue);
    });
    QObject::connect(&orchestrator, &ExportOrchestrator::exportCancelled, [&](const types::RequestId requestId) {
        cancelledEvents.push_back(requestId);
    });

    const ExportSubmissionResult cancelled = orchestrator.submitExport(makeFileRequest(QStringLiteral("blocked.jpg")));
    ASSERT_TRUE(cancelled.hasValue());
    ASSERT_TRUE(waitForCondition([&]() { return pipelineProbe->firstPreparationStarted().tryAcquire(1); }));
    ASSERT_TRUE(orchestrator.cancelExport(cancelled.value()));
    const ExportSubmissionResult independent =
        orchestrator.submitExport(makeFileRequest(QStringLiteral("independent.jpg")));
    ASSERT_TRUE(independent.hasValue());

    pipelineProbe->releaseFirstPreparation();
    ASSERT_TRUE(waitForCondition([&]() {
        return std::ranges::any_of(completedEvents,
                                   [&](const ExportResult& result) { return result.requestId == independent.value(); });
    }));
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);

    EXPECT_EQ(1, pipelineProbe->executionCalls());
    EXPECT_EQ(1, std::ranges::count(cancelledEvents, cancelled.value()));
    EXPECT_EQ(0, std::ranges::count_if(progressEvents, [&](const ExportProgress& progress) {
                  return progress.requestId == cancelled.value();
              }));
    EXPECT_EQ(0, std::ranges::count_if(completedEvents, [&](const ExportResult& result) {
                  return result.requestId == cancelled.value();
              }));
    EXPECT_EQ(0, std::ranges::count_if(failedEvents, [&](const ExportIssue& issue) {
                  return issue.requestId == cancelled.value();
              }));
}

TEST(ExportOrchestratorTest, CancelsActivePipelineBeforeWaitingForShutdown)
{
    (void)test::application();
    auto probe = std::make_shared<CancellationProbe>();
    {
        ExportOrchestrator orchestrator(std::make_unique<IndependentlyCancellableExportPipeline>(probe));
        const ExportSubmissionResult submitted =
            orchestrator.submitExport(makeFileRequest(QStringLiteral("blocked.jpg")));
        ASSERT_TRUE(submitted.hasValue());
        ASSERT_TRUE(waitForCondition([&]() { return probe->blockedJobStarted.tryAcquire(1); }));
    }

    EXPECT_TRUE(probe->cancelledJobFinished.tryAcquire(1, 5000));
}

}  // namespace
}  // namespace flexraw::core::orchestration
