#include <atomic>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <gtest/gtest.h>

#include "catalog_orchestrator.h"
#include "editor_client_projection.h"
#include "editor_orchestrator.h"
#include "preview_orchestrator.h"
#include "preview_pipeline.h"
#include "test_application.h"

namespace flexraw::core::orchestration
{
namespace
{

// 목적: catalog-backed editor test가 import할 최소 raster source 생성
// 입력: path: 생성할 source 경로, contents: fingerprint에 사용할 byte 내용
// 출력: source file을 완전히 기록했으면 true
[[nodiscard]] bool writeCatalogSource(const QString& path, const QByteArray& contents)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(contents) == contents.size() &&
           file.flush();
}

// 목적: Qt event를 처리하며 catalog fingerprint update 완료를 bounded wait
// 입력: updateCount: signal handler가 증가시키는 count, expectedCount: 목표 update 수, timeoutMs: 제한 시간
// 출력: 제한 시간 안에 목표 update 수에 도달하면 true
[[nodiscard]] bool waitForCatalogUpdate(const int& updateCount, int expectedCount, int timeoutMs = 3000)
{
    QElapsedTimer timeout;
    timeout.start();

    while (timeout.elapsed() < timeoutMs)
    {
        if (updateCount >= expectedCount)
        {
            return true;
        }

        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }

    return updateCount >= expectedCount;
}

class EditorTestCatalog final
{
public:
    // 목적: transient Editor test를 stable PhotoId activation 환경으로 전환할 임시 Catalog 구성
    // 입력: 없음
    // 출력: temp directory와 open CatalogOrchestrator를 소유하는 test fixture
    EditorTestCatalog()
    {
        if (m_directory.isValid())
        {
            const QString catalogPath = QDir(m_directory.path()).filePath(QStringLiteral("editor.flexraw-catalog"));
            m_isValid = m_orchestrator.openCatalog(catalogPath).hasValue();
        }
    }

    // 목적: 임시 Catalog와 database session 생성 성공 여부 조회
    // 입력: 없음
    // 출력: source 생성과 activation test를 실행할 수 있으면 true
    [[nodiscard]] bool isValid() const noexcept
    {
        return m_directory.isValid() && m_isValid;
    }

    // 목적: EditorOrchestrator constructor에 주입할 Catalog use case 반환
    // 입력: 없음
    // 출력: fixture lifetime 동안 유효한 CatalogOrchestrator 참조
    [[nodiscard]] CatalogOrchestrator& orchestrator() noexcept
    {
        return m_orchestrator;
    }

    // 목적: activation test용 실제 source file과 scan entry 생성
    // 입력: fileName: temp directory의 파일명, kind: preview request에 사용할 file kind
    // 출력: file 생성 성공 시 CatalogEntry, 실패 시 빈 값
    [[nodiscard]] std::optional<catalog::CatalogEntry> createEntry(const QString& fileName,
                                                                   types::SupportedFileKind kind)
    {
        const QString sourcePath = QDir(m_directory.path()).filePath(fileName);
        if (!writeCatalogSource(sourcePath, fileName.toUtf8()))
        {
            return std::nullopt;
        }

        return catalog::CatalogEntry{types::makeFileDescriptor(QFileInfo(sourcePath), kind),
                                     types::FileScanStatus::Ready};
    }

private:
    QTemporaryDir m_directory;
    CatalogOrchestrator m_orchestrator;
    bool m_isValid{false};
};

class RecordingPreviewPipeline final : public IPreviewPipeline
{
public:
    // 목적: worker request를 기록하고 deterministic frame을 즉시 반환
    // 입력: request: 기록할 preview snapshot, tier: 사용하지 않는 raster tier, cancellationToken: 사용하지 않는 token
    // 출력: 1x1 검정 test frame
    [[nodiscard]] PreviewPipelineResult render(const PreviewRequest& request,
                                               PreviewTier,
                                               const types::CancellationToken&) override
    {
        {
            const std::scoped_lock lock(m_mutex);
            m_requests.push_back(request);
        }
        requestRecorded.release();

        QImage image(1, 1, QImage::Format_RGBA8888);
        image.fill(Qt::black);
        develop::ImageHistogram histogram;
        histogram.pixelCount = 1;
        develop::ClippingSummary clipping;
        clipping.pixelCount = 1;
        return PreviewPipelineResult::success({image, histogram, clipping, {}});
    }

    // 목적: worker가 기록한 request snapshot 반환
    // 입력: 없음
    // 출력: thread-safe request 복사본
    [[nodiscard]] std::vector<PreviewRequest> requests() const
    {
        const std::scoped_lock lock(m_mutex);
        return m_requests;
    }

    QSemaphore requestRecorded;

private:
    mutable std::mutex m_mutex;
    std::vector<PreviewRequest> m_requests;
};

// 목적: Qt timer event를 처리하면서 recording pipeline의 request 수가 목표에 도달할 때까지 대기
// 입력: pipeline: 관찰할 recording pipeline, expectedCount: 목표 request 수, timeoutMs: 제한 시간
// 출력: 제한 시간 안에 목표 수에 도달하면 true
[[nodiscard]] bool waitForRequestCount(const RecordingPreviewPipeline& pipeline,
                                       std::size_t expectedCount,
                                       int timeoutMs = 2000)
{
    QElapsedTimer timeout;
    timeout.start();

    while (timeout.elapsed() < timeoutMs)
    {
        if (pipeline.requests().size() >= expectedCount)
        {
            return true;
        }

        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }

    return pipeline.requests().size() >= expectedCount;
}

class BlockingEditorPreviewPipeline final : public IPreviewPipeline
{
public:
    // 목적: 첫 editor preview를 test release 전까지 worker thread에서 대기
    // 입력: request: 사용하지 않는 preview snapshot, tier: 사용하지 않는 raster tier, cancellationToken: 미사용
    // 출력: 1x1 검정 test frame
    [[nodiscard]] PreviewPipelineResult render(const PreviewRequest&,
                                               PreviewTier,
                                               const types::CancellationToken&) override
    {
        if (m_renderCount.fetch_add(1, std::memory_order_relaxed) == 0)
        {
            firstRenderStarted.release();
            allowFirstRenderToFinish.acquire();
        }

        QImage image(1, 1, QImage::Format_RGBA8888);
        image.fill(Qt::black);
        develop::ImageHistogram histogram;
        histogram.pixelCount = 1;
        develop::ClippingSummary clipping;
        clipping.pixelCount = 1;
        return PreviewPipelineResult::success({image, histogram, clipping, {}});
    }

    QSemaphore firstRenderStarted;
    QSemaphore allowFirstRenderToFinish;

private:
    std::atomic_int m_renderCount{0};
};

TEST(EditorClientProjectionTest, PreservesEveryDevelopParameter)
{
    types::DevelopParams params;
    params.exposureEv = 0.1F;
    params.contrast = 0.2F;
    params.highlights = 0.3F;
    params.shadows = 0.4F;
    params.whites = 0.5F;
    params.blacks = 0.6F;
    params.saturation = 0.7F;
    params.vibrance = 0.8F;
    params.whiteBalanceMode = types::WhiteBalanceMode::Custom;
    params.whiteBalanceTemperatureKelvin = 7200.0F;
    params.whiteBalanceTint = 0.9F;
    params.clarity = 1.0F;
    params.dehaze = 1.1F;
    params.sharpeningAmount = 1.2F;
    params.sharpeningRadius = 1.3F;
    params.sharpeningDetail = 1.4F;
    params.sharpeningMasking = 1.5F;
    params.luminanceNoiseReduction = 1.6F;
    params.colorNoiseReduction = 1.7F;
    params.toneCurveShadows = 1.8F;
    params.toneCurveDarks = 1.9F;
    params.toneCurveLights = 2.0F;
    params.toneCurveHighlights = 2.1F;
    params.pointCurveBlack = 2.2F;
    params.pointCurveShadows = 2.3F;
    params.pointCurveMidtones = 2.4F;
    params.pointCurveHighlights = 2.5F;
    params.pointCurveWhite = 2.6F;

    EXPECT_EQ(params, fromClientDevelopParams(toClientDevelopParams(params)));
}

TEST(EditorOrchestratorTest, SavesAndRestoresCatalogDevelopStateAcrossSessions)
{
    (void)test::application();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString sourcePath = QDir(directory.path()).filePath(QStringLiteral("persisted.jpg"));
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("library.flexraw-catalog"));
    ASSERT_TRUE(writeCatalogSource(sourcePath, QByteArray("persisted-source")));
    types::PhotoId photoId;
    types::DevelopParams editedParams;
    editedParams.exposureEv = 1.1F;
    editedParams.contrast = 0.2F;

    {
        CatalogOrchestrator catalogOrchestrator;
        ASSERT_TRUE(catalogOrchestrator.openCatalog(catalogPath).hasValue());
        const CatalogImportResult imported = catalogOrchestrator.importFolder(directory.path());
        ASSERT_TRUE(imported.hasValue());
        ASSERT_EQ(1, imported.value().photoIds.size());
        photoId = imported.value().photoIds.front();
        auto pipeline = std::make_unique<RecordingPreviewPipeline>();
        PreviewOrchestrator previewOrchestrator(std::move(pipeline));
        EditorOrchestrator editorOrchestrator(previewOrchestrator, catalogOrchestrator);

        const EditorStateResult selected = editorOrchestrator.selectCatalogPhoto(photoId, QSize{640, 480});
        ASSERT_TRUE(selected.hasValue());
        EXPECT_EQ(photoId.value, selected.value().photo.photoId.value);
        EXPECT_EQ(0U, selected.value().persistedRevision);
        EXPECT_FALSE(selected.value().dirty);
        EXPECT_TRUE(selected.value().sourceProcessingAllowed);
        EXPECT_TRUE(selected.value().photo.transientKey.isEmpty());
        ASSERT_TRUE(editorOrchestrator.updateDevelopParams(editedParams, QSize{640, 480}));
        EXPECT_TRUE(editorOrchestrator.state().dirty);

        const EditorStateResult saved = editorOrchestrator.saveCurrentPhoto();
        ASSERT_TRUE(saved.hasValue());
        EXPECT_EQ(1U, saved.value().persistedRevision);
        EXPECT_FALSE(saved.value().dirty);
    }

    {
        CatalogOrchestrator catalogOrchestrator;
        ASSERT_TRUE(catalogOrchestrator.openCatalog(catalogPath).hasValue());
        auto pipeline = std::make_unique<RecordingPreviewPipeline>();
        PreviewOrchestrator previewOrchestrator(std::move(pipeline));
        EditorOrchestrator editorOrchestrator(previewOrchestrator, catalogOrchestrator);

        const EditorStateResult restored = editorOrchestrator.selectCatalogPhoto(photoId, QSize{640, 480});
        ASSERT_TRUE(restored.hasValue());
        EXPECT_EQ(editedParams, restored.value().params);
        EXPECT_EQ(1U, restored.value().persistedRevision);
        EXPECT_EQ(0U, restored.value().photo.developRevision);
        EXPECT_FALSE(restored.value().dirty);
    }
}

TEST(EditorOrchestratorTest, PreservesDirtyStateWhenPersistedRevisionConflicts)
{
    (void)test::application();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString sourcePath = QDir(directory.path()).filePath(QStringLiteral("conflict.jpg"));
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("library.flexraw-catalog"));
    ASSERT_TRUE(writeCatalogSource(sourcePath, QByteArray("conflict-source")));
    CatalogOrchestrator catalogOrchestrator;
    ASSERT_TRUE(catalogOrchestrator.openCatalog(catalogPath).hasValue());
    const CatalogImportResult imported = catalogOrchestrator.importFolder(directory.path());
    ASSERT_TRUE(imported.hasValue());
    ASSERT_EQ(1, imported.value().photoIds.size());
    const types::PhotoId photoId = imported.value().photoIds.front();
    auto pipeline = std::make_unique<RecordingPreviewPipeline>();
    PreviewOrchestrator previewOrchestrator(std::move(pipeline));
    EditorOrchestrator editorOrchestrator(previewOrchestrator, catalogOrchestrator);
    ASSERT_TRUE(editorOrchestrator.selectCatalogPhoto(photoId, QSize{640, 480}).hasValue());
    types::DevelopParams localParams;
    localParams.exposureEv = 0.7F;
    ASSERT_TRUE(editorOrchestrator.updateDevelopParams(localParams, QSize{640, 480}));
    types::DevelopParams externalParams;
    externalParams.exposureEv = -0.4F;
    ASSERT_TRUE(catalogOrchestrator.saveDevelopState(photoId, externalParams, 0).hasValue());

    const EditorStateResult saved = editorOrchestrator.saveCurrentPhoto();

    ASSERT_TRUE(saved.hasError());
    EXPECT_EQ(types::ErrorCode::Conflict, saved.error().code);
    EXPECT_EQ(localParams, editorOrchestrator.state().params);
    EXPECT_EQ(0U, editorOrchestrator.state().persistedRevision);
    EXPECT_TRUE(editorOrchestrator.state().dirty);
}

TEST(EditorOrchestratorTest, BlocksSourceDependentEditingWhenCatalogSourceIsMissing)
{
    (void)test::application();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString sourcePath = QDir(directory.path()).filePath(QStringLiteral("missing.jpg"));
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("library.flexraw-catalog"));
    ASSERT_TRUE(writeCatalogSource(sourcePath, QByteArray("missing-source")));
    CatalogOrchestrator catalogOrchestrator;
    int updateCount = 0;
    QObject::connect(&catalogOrchestrator, &CatalogOrchestrator::sourceBindingUpdated, [&updateCount](const auto&) {
        ++updateCount;
    });
    ASSERT_TRUE(catalogOrchestrator.openCatalog(catalogPath).hasValue());
    const CatalogImportResult imported = catalogOrchestrator.importFolder(directory.path());
    ASSERT_TRUE(imported.hasValue());
    ASSERT_EQ(1, imported.value().photoIds.size());
    ASSERT_TRUE(waitForCatalogUpdate(updateCount, 1));
    ASSERT_TRUE(QFile::remove(sourcePath));
    auto pipeline = std::make_unique<RecordingPreviewPipeline>();
    RecordingPreviewPipeline* const pipelinePointer = pipeline.get();
    PreviewOrchestrator previewOrchestrator(std::move(pipeline));
    EditorOrchestrator editorOrchestrator(previewOrchestrator, catalogOrchestrator);

    const EditorStateResult selected =
        editorOrchestrator.selectCatalogPhoto(imported.value().photoIds.front(), QSize{640, 480});

    ASSERT_TRUE(selected.hasValue());
    EXPECT_TRUE(selected.value().hasSelection);
    EXPECT_FALSE(selected.value().sourceProcessingAllowed);
    ASSERT_TRUE(selected.value().sourceState.has_value());
    EXPECT_EQ(catalog::SourceBindingState::Missing, *selected.value().sourceState);
    EXPECT_TRUE(selected.value().sourceResolution.canRelinkSource);
    EXPECT_FALSE(selected.value().sourceResolution.canAcceptReplacement);
    types::DevelopParams params;
    params.exposureEv = 0.5F;
    EXPECT_FALSE(editorOrchestrator.updateDevelopParams(params, QSize{640, 480}));
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    EXPECT_TRUE(pipelinePointer->requests().empty());
}

TEST(EditorOrchestratorTest, RejectsSaveAfterCatalogSessionChanges)
{
    (void)test::application();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString sourcePath = QDir(directory.path()).filePath(QStringLiteral("session.jpg"));
    const QString firstCatalogPath = QDir(directory.path()).filePath(QStringLiteral("first.flexraw-catalog"));
    const QString secondCatalogPath = QDir(directory.path()).filePath(QStringLiteral("second.flexraw-catalog"));
    ASSERT_TRUE(writeCatalogSource(sourcePath, QByteArray("session-source")));
    CatalogOrchestrator catalogOrchestrator;
    ASSERT_TRUE(catalogOrchestrator.openCatalog(firstCatalogPath).hasValue());
    const CatalogImportResult firstImport = catalogOrchestrator.importFolder(directory.path());
    ASSERT_TRUE(firstImport.hasValue());
    ASSERT_EQ(1, firstImport.value().photoIds.size());
    auto pipeline = std::make_unique<RecordingPreviewPipeline>();
    PreviewOrchestrator previewOrchestrator(std::move(pipeline));
    EditorOrchestrator editorOrchestrator(previewOrchestrator, catalogOrchestrator);
    ASSERT_TRUE(
        editorOrchestrator.selectCatalogPhoto(firstImport.value().photoIds.front(), QSize{640, 480}).hasValue());
    types::DevelopParams params;
    params.exposureEv = 0.6F;
    ASSERT_TRUE(editorOrchestrator.updateDevelopParams(params, QSize{640, 480}));

    (void)catalogOrchestrator.closeCatalog();
    ASSERT_TRUE(catalogOrchestrator.openCatalog(secondCatalogPath).hasValue());
    const CatalogImportResult secondImport = catalogOrchestrator.importFolder(directory.path());
    ASSERT_TRUE(secondImport.hasValue());
    ASSERT_EQ(1, secondImport.value().photoIds.size());
    const EditorStateResult saved = editorOrchestrator.saveCurrentPhoto();

    ASSERT_TRUE(saved.hasError());
    EXPECT_EQ(types::ErrorCode::Conflict, saved.error().code);
    const CatalogPhotoStateResult secondState = catalogOrchestrator.resolvePhoto(secondImport.value().photoIds.front());
    ASSERT_TRUE(secondState.hasValue());
    EXPECT_EQ(0U, secondState.value().persistedRevision);
}

TEST(EditorOrchestratorTest, ResumesPreviewAfterExplicitReplacementAcceptance)
{
    (void)test::application();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString sourcePath = QDir(directory.path()).filePath(QStringLiteral("replacement.jpg"));
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("library.flexraw-catalog"));
    ASSERT_TRUE(writeCatalogSource(sourcePath, QByteArray("original-source")));
    CatalogOrchestrator catalogOrchestrator;
    int updateCount = 0;
    QObject::connect(&catalogOrchestrator, &CatalogOrchestrator::sourceBindingUpdated, [&updateCount](const auto&) {
        ++updateCount;
    });
    ASSERT_TRUE(catalogOrchestrator.openCatalog(catalogPath).hasValue());
    const CatalogImportResult imported = catalogOrchestrator.importFolder(directory.path());
    ASSERT_TRUE(imported.hasValue());
    ASSERT_EQ(1, imported.value().photoIds.size());
    ASSERT_TRUE(waitForCatalogUpdate(updateCount, 1));
    const types::PhotoId photoId = imported.value().photoIds.front();
    ASSERT_TRUE(writeCatalogSource(sourcePath, QByteArray("replacement-source-with-different-size")));
    auto pipeline = std::make_unique<RecordingPreviewPipeline>();
    RecordingPreviewPipeline* const pipelinePointer = pipeline.get();
    PreviewOrchestrator previewOrchestrator(std::move(pipeline));
    EditorOrchestrator editorOrchestrator(previewOrchestrator, catalogOrchestrator);

    const EditorStateResult selected = editorOrchestrator.selectCatalogPhoto(photoId, QSize{640, 480});
    ASSERT_TRUE(selected.hasValue());
    EXPECT_FALSE(selected.value().sourceProcessingAllowed);
    ASSERT_TRUE(waitForCatalogUpdate(updateCount, 2));
    ASSERT_TRUE(editorOrchestrator.state().sourceState.has_value());
    EXPECT_EQ(catalog::SourceBindingState::ReplacementDetected, *editorOrchestrator.state().sourceState);
    EXPECT_TRUE(editorOrchestrator.state().sourceResolution.canAcceptReplacement);
    EXPECT_TRUE(editorOrchestrator.state().sourceResolution.canRegisterReplacementAsNew);
    EXPECT_TRUE(editorOrchestrator.state().sourceResolution.canRelinkSource);
    ASSERT_TRUE(catalogOrchestrator.acceptReplacement(photoId).hasValue());
    ASSERT_TRUE(waitForCatalogUpdate(updateCount, 3));
    ASSERT_TRUE(waitForRequestCount(*pipelinePointer, 1));

    const EditorState resumed = editorOrchestrator.state();
    EXPECT_TRUE(resumed.sourceProcessingAllowed);
    ASSERT_TRUE(resumed.sourceState.has_value());
    EXPECT_EQ(catalog::SourceBindingState::Available, *resumed.sourceState);
    EXPECT_FALSE(resumed.sourceResolution.canAcceptReplacement);
    EXPECT_FALSE(resumed.sourceResolution.canRegisterReplacementAsNew);
    EXPECT_FALSE(resumed.sourceResolution.canRelinkSource);
    const std::vector<PreviewRequest> requests = pipelinePointer->requests();
    ASSERT_EQ(1U, requests.size());
    EXPECT_EQ(photoId.value, requests.front().photo.photoId.value);
    EXPECT_TRUE(requests.front().photo.transientKey.isEmpty());
}

TEST(EditorOrchestratorTest, PreservesStableSelectionWhenFolderActivationRegistrationFails)
{
    (void)test::application();
    EditorTestCatalog catalog;
    ASSERT_TRUE(catalog.isValid());
    auto pipeline = std::make_unique<RecordingPreviewPipeline>();
    PreviewOrchestrator previewOrchestrator(std::move(pipeline));
    EditorOrchestrator editorOrchestrator(previewOrchestrator, catalog.orchestrator());
    const std::optional<catalog::CatalogEntry> firstEntry =
        catalog.createEntry(QStringLiteral("first.bmp"), types::SupportedFileKind::RasterImage);
    const std::optional<catalog::CatalogEntry> secondEntry =
        catalog.createEntry(QStringLiteral("second.bmp"), types::SupportedFileKind::RasterImage);
    ASSERT_TRUE(firstEntry.has_value());
    ASSERT_TRUE(secondEntry.has_value());
    const EditorStateResult first = editorOrchestrator.activatePhoto(*firstEntry, QSize{640, 480});
    ASSERT_TRUE(first.hasValue());
    const types::PhotoId firstPhotoId = first.value().photo.photoId;
    (void)catalog.orchestrator().closeCatalog();

    const EditorStateResult failed = editorOrchestrator.activatePhoto(*secondEntry, QSize{640, 480});

    ASSERT_TRUE(failed.hasError());
    EXPECT_EQ(types::ErrorCode::Conflict, failed.error().code);
    EXPECT_EQ(firstPhotoId.value, editorOrchestrator.state().photo.photoId.value);
    EXPECT_TRUE(editorOrchestrator.state().photo.transientKey.isEmpty());
}

TEST(EditorOrchestratorTest, OwnsSelectionDirtyAndUndoRedoState)
{
    (void)test::application();
    EditorTestCatalog catalog;
    ASSERT_TRUE(catalog.isValid());
    auto pipeline = std::make_unique<RecordingPreviewPipeline>();
    PreviewOrchestrator previewOrchestrator(std::move(pipeline));
    EditorOrchestrator editorOrchestrator(previewOrchestrator, catalog.orchestrator());
    const std::optional<catalog::CatalogEntry> entry =
        catalog.createEntry(QStringLiteral("first.bmp"), types::SupportedFileKind::RasterImage);
    ASSERT_TRUE(entry.has_value());

    const EditorStateResult selectedResult = editorOrchestrator.activatePhoto(*entry, QSize{640, 480});
    ASSERT_TRUE(selectedResult.hasValue());
    const EditorState selected = selectedResult.value();
    EXPECT_TRUE(selected.hasSelection);
    EXPECT_TRUE(types::isValidPhotoId(selected.photo.photoId));
    EXPECT_TRUE(selected.photo.transientKey.isEmpty());
    EXPECT_EQ(selected.photo.developRevision, 0);
    EXPECT_FALSE(selected.dirty);
    EXPECT_FALSE(selected.canUndo);
    EXPECT_FALSE(selected.canRedo);

    editorOrchestrator.beginEdit();
    types::DevelopParams firstParams;
    firstParams.exposureEv = 1.0F;
    EXPECT_TRUE(editorOrchestrator.updateDevelopParams(firstParams, QSize{640, 480}));
    types::DevelopParams secondParams = firstParams;
    secondParams.contrast = 0.12F;
    EXPECT_TRUE(editorOrchestrator.updateDevelopParams(secondParams, QSize{640, 480}));
    editorOrchestrator.endEdit();

    const EditorState edited = editorOrchestrator.state();
    EXPECT_EQ(edited.params, secondParams);
    EXPECT_EQ(edited.photo.developRevision, 2);
    EXPECT_TRUE(edited.dirty);
    EXPECT_TRUE(edited.canUndo);
    EXPECT_FALSE(edited.canRedo);

    const std::optional<EditorState> undone = editorOrchestrator.undo(QSize{640, 480});
    ASSERT_TRUE(undone.has_value());
    EXPECT_EQ(undone->params, types::DevelopParams{});
    EXPECT_EQ(undone->photo.developRevision, 3);
    EXPECT_FALSE(undone->dirty);
    EXPECT_FALSE(undone->canUndo);
    EXPECT_TRUE(undone->canRedo);

    const std::optional<EditorState> redone = editorOrchestrator.redo(QSize{640, 480});
    ASSERT_TRUE(redone.has_value());
    EXPECT_EQ(redone->params, secondParams);
    EXPECT_EQ(redone->photo.developRevision, 4);
    EXPECT_TRUE(redone->dirty);
    EXPECT_TRUE(redone->canUndo);
    EXPECT_FALSE(redone->canRedo);

    const std::optional<catalog::CatalogEntry> secondEntry =
        catalog.createEntry(QStringLiteral("second.bmp"), types::SupportedFileKind::RasterImage);
    ASSERT_TRUE(secondEntry.has_value());
    const EditorStateResult secondResult = editorOrchestrator.activatePhoto(*secondEntry, QSize{640, 480});
    ASSERT_TRUE(secondResult.hasValue());
    const EditorState secondSelection = secondResult.value();
    EXPECT_FALSE(secondSelection.dirty);
    EXPECT_EQ(secondSelection.params, types::DevelopParams{});
    const EditorStateResult restoredResult = editorOrchestrator.activatePhoto(*entry, QSize{640, 480});
    ASSERT_TRUE(restoredResult.hasValue());
    const EditorState restoredSelection = restoredResult.value();
    EXPECT_EQ(restoredSelection.params, secondParams);
    EXPECT_TRUE(restoredSelection.dirty);

    editorOrchestrator.clearSelection();
    EXPECT_FALSE(editorOrchestrator.state().hasSelection);
}

TEST(EditorOrchestratorTest, ExecutesQtFreeEditorCommandsWithoutPreviewTarget)
{
    (void)test::application();
    EditorTestCatalog catalog;
    ASSERT_TRUE(catalog.isValid());
    auto pipeline = std::make_unique<RecordingPreviewPipeline>();
    RecordingPreviewPipeline* pipelineObserver = pipeline.get();
    PreviewOrchestrator previewOrchestrator(std::move(pipeline));
    EditorOrchestrator editorOrchestrator(previewOrchestrator, catalog.orchestrator());
    client::IEditorClient& editorClient = editorOrchestrator;
    const client::EditorResult missingSelection = editorClient.updateDevelopParams({});
    ASSERT_TRUE(missingSelection.hasError());
    EXPECT_EQ(client::ClientErrorCode::InvalidArgument, missingSelection.error().code);
    const client::EditorResult invalidSelection = editorClient.selectPhoto({client::ClientPhotoId{0}});
    ASSERT_TRUE(invalidSelection.hasError());
    EXPECT_EQ(client::ClientErrorCode::InvalidArgument, invalidSelection.error().code);
    const std::optional<catalog::CatalogEntry> entry =
        catalog.createEntry(QStringLiteral("headless.bmp"), types::SupportedFileKind::RasterImage);
    ASSERT_TRUE(entry.has_value());
    int sourceUpdateCount = 0;
    QObject::connect(&catalog.orchestrator(),
                     &CatalogOrchestrator::sourceBindingUpdated,
                     &catalog.orchestrator(),
                     [&sourceUpdateCount](const CatalogSourceUpdate&) { ++sourceUpdateCount; });
    const CatalogPhotoRegistrationResult registered = catalog.orchestrator().registerPhoto(*entry);
    ASSERT_TRUE(registered.hasValue());

    const client::EditorResult selected = editorClient.selectPhoto({client::ClientPhotoId{registered.value().value}});
    ASSERT_TRUE(selected.hasValue());
    EXPECT_TRUE(selected.value().hasSelection);
    EXPECT_EQ(registered.value().value, selected.value().photoId.value);
    EXPECT_FALSE(selected.value().adjustmentActive);
    client::EditorDevelopParams invalidParams = selected.value().params;
    invalidParams.exposureEv = std::numeric_limits<float>::infinity();
    const client::EditorResult invalidUpdate = editorClient.updateDevelopParams({invalidParams});
    ASSERT_TRUE(invalidUpdate.hasError());
    EXPECT_EQ(client::ClientErrorCode::InvalidArgument, invalidUpdate.error().code);
    EXPECT_EQ(selected.value().params, editorClient.editorSnapshot().params);
    ASSERT_TRUE(editorClient.beginAdjustment().hasValue());
    client::EditorDevelopParams firstParams = selected.value().params;
    firstParams.exposureEv = 0.7F;
    ASSERT_TRUE(editorClient.updateDevelopParams({firstParams}).hasValue());
    client::EditorDevelopParams secondParams = firstParams;
    secondParams.contrast = 0.2F;
    const client::EditorResult updated = editorClient.updateDevelopParams({secondParams});
    ASSERT_TRUE(updated.hasValue());
    EXPECT_TRUE(updated.value().adjustmentActive);
    const client::EditorResult adjusted = editorClient.endAdjustment();
    ASSERT_TRUE(adjusted.hasValue());
    EXPECT_FALSE(adjusted.value().adjustmentActive);
    EXPECT_TRUE(adjusted.value().dirty);
    EXPECT_TRUE(adjusted.value().canUndo);

    const client::EditorResult undone = editorClient.undoDevelop();
    ASSERT_TRUE(undone.hasValue());
    EXPECT_EQ(selected.value().params, undone.value().params);
    EXPECT_TRUE(undone.value().canRedo);
    const client::EditorResult redone = editorClient.redoDevelop();
    ASSERT_TRUE(redone.hasValue());
    EXPECT_EQ(secondParams, redone.value().params);
    const client::EditorResult saved = editorClient.saveDevelopState();
    ASSERT_TRUE(saved.hasValue());
    EXPECT_FALSE(saved.value().dirty);
    EXPECT_GT(saved.value().persistedRevision.value, 0U);

    ASSERT_TRUE(waitForCatalogUpdate(sourceUpdateCount, 1));
    EXPECT_TRUE(pipelineObserver->requests().empty());
}

TEST(EditorOrchestratorTest, RejectsInvalidParamsBeforeMutatingHistory)
{
    (void)test::application();
    EditorTestCatalog catalog;
    ASSERT_TRUE(catalog.isValid());
    auto pipeline = std::make_unique<RecordingPreviewPipeline>();
    PreviewOrchestrator previewOrchestrator(std::move(pipeline));
    EditorOrchestrator editorOrchestrator(previewOrchestrator, catalog.orchestrator());
    const std::optional<catalog::CatalogEntry> entry =
        catalog.createEntry(QStringLiteral("invalid.bmp"), types::SupportedFileKind::RasterImage);
    ASSERT_TRUE(entry.has_value());
    ASSERT_TRUE(editorOrchestrator.activatePhoto(*entry, QSize{640, 480}).hasValue());
    editorOrchestrator.beginEdit();
    types::DevelopParams invalidParams;
    invalidParams.contrast = 12.0F;

    EXPECT_FALSE(editorOrchestrator.updateDevelopParams(invalidParams, QSize{640, 480}));
    editorOrchestrator.endEdit();

    const EditorState currentState = editorOrchestrator.state();
    EXPECT_EQ(currentState.params, types::DevelopParams{});
    EXPECT_EQ(currentState.photo.developRevision, 0);
    EXPECT_FALSE(currentState.dirty);
    EXPECT_FALSE(currentState.canUndo);
    EXPECT_FALSE(currentState.canRedo);
}

TEST(EditorOrchestratorTest, DebouncesLatestParamsIntoPreviewSnapshot)
{
    (void)test::application();
    EditorTestCatalog catalog;
    ASSERT_TRUE(catalog.isValid());
    auto pipeline = std::make_unique<RecordingPreviewPipeline>();
    RecordingPreviewPipeline* const pipelinePointer = pipeline.get();
    PreviewOrchestrator previewOrchestrator(std::move(pipeline));
    EditorOrchestrator editorOrchestrator(previewOrchestrator, catalog.orchestrator());
    QEventLoop eventLoop;
    std::vector<PreviewResult> results;
    QObject::connect(&editorOrchestrator, &EditorOrchestrator::previewUpdated, [&](const PreviewResult& result) {
        results.push_back(result);
        eventLoop.quit();
    });
    QTimer::singleShot(2000, &eventLoop, &QEventLoop::quit);
    const std::optional<catalog::CatalogEntry> entry =
        catalog.createEntry(QStringLiteral("latest.cr3"), types::SupportedFileKind::Raw);
    ASSERT_TRUE(entry.has_value());
    ASSERT_TRUE(editorOrchestrator.activatePhoto(*entry, QSize{800, 600}).hasValue());
    types::DevelopParams params;
    params.exposureEv = 1.25F;
    ASSERT_TRUE(editorOrchestrator.updateDevelopParams(params, QSize{1024, 768}));

    eventLoop.exec();

    ASSERT_EQ(results.size(), 1U);
    const std::vector<PreviewRequest> requests = pipelinePointer->requests();
    ASSERT_EQ(requests.size(), 1U);
    EXPECT_TRUE(types::isValidPhotoId(requests.front().photo.photoId));
    EXPECT_TRUE(requests.front().photo.transientKey.isEmpty());
    EXPECT_EQ(requests.front().photo.developRevision, 1);
    EXPECT_EQ(requests.front().params, params);
    EXPECT_EQ(requests.front().targetSize, QSize(1024, 768));
    EXPECT_EQ(requests.front().progression, PreviewProgression::FinalOnly);
    EXPECT_EQ(requests.front().renderMode, PreviewRenderMode::Final);
    EXPECT_EQ(results.front().photo.developRevision, 1);
    EXPECT_EQ(results.front().tier, PreviewTier::Standard);
    EXPECT_EQ(results.front().renderMode, PreviewRenderMode::Final);
}

TEST(EditorOrchestratorTest, CoalescesResizeIntoPendingSelectionPreview)
{
    (void)test::application();
    EditorTestCatalog catalog;
    ASSERT_TRUE(catalog.isValid());
    auto pipeline = std::make_unique<RecordingPreviewPipeline>();
    RecordingPreviewPipeline* const pipelinePointer = pipeline.get();
    PreviewOrchestrator previewOrchestrator(std::move(pipeline));
    EditorOrchestrator editorOrchestrator(previewOrchestrator, catalog.orchestrator());
    const std::optional<catalog::CatalogEntry> entry =
        catalog.createEntry(QStringLiteral("resize-pending.bmp"), types::SupportedFileKind::RasterImage);
    ASSERT_TRUE(entry.has_value());

    ASSERT_TRUE(editorOrchestrator.activatePhoto(*entry, QSize{640, 480}).hasValue());
    EXPECT_TRUE(editorOrchestrator.updatePreviewTargetSize(QSize{1024, 768}));
    ASSERT_TRUE(waitForRequestCount(*pipelinePointer, 1));
    QThread::msleep(200);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);

    const std::vector<PreviewRequest> requests = pipelinePointer->requests();
    ASSERT_EQ(requests.size(), 1U);
    EXPECT_EQ(requests.front().targetSize, QSize(1024, 768));
    EXPECT_EQ(requests.front().progression, PreviewProgression::Progressive);
    EXPECT_EQ(requests.front().renderMode, PreviewRenderMode::Final);
}

TEST(EditorOrchestratorTest, DebouncesSettledResizeIntoFinalPreview)
{
    (void)test::application();
    EditorTestCatalog catalog;
    ASSERT_TRUE(catalog.isValid());
    auto pipeline = std::make_unique<RecordingPreviewPipeline>();
    RecordingPreviewPipeline* const pipelinePointer = pipeline.get();
    PreviewOrchestrator previewOrchestrator(std::move(pipeline));
    EditorOrchestrator editorOrchestrator(previewOrchestrator, catalog.orchestrator());
    const std::optional<catalog::CatalogEntry> entry =
        catalog.createEntry(QStringLiteral("resize-settled.bmp"), types::SupportedFileKind::RasterImage);
    ASSERT_TRUE(entry.has_value());
    ASSERT_TRUE(editorOrchestrator.activatePhoto(*entry, QSize{640, 480}).hasValue());
    ASSERT_TRUE(waitForRequestCount(*pipelinePointer, 1));

    EXPECT_FALSE(editorOrchestrator.updatePreviewTargetSize(QSize{}));
    EXPECT_FALSE(editorOrchestrator.updatePreviewTargetSize(QSize{640, 480}));
    EXPECT_TRUE(editorOrchestrator.updatePreviewTargetSize(QSize{800, 600}));
    EXPECT_TRUE(editorOrchestrator.updatePreviewTargetSize(QSize{1024, 768}));
    ASSERT_TRUE(waitForRequestCount(*pipelinePointer, 2));

    const std::vector<PreviewRequest> requests = pipelinePointer->requests();
    ASSERT_EQ(requests.size(), 2U);
    EXPECT_EQ(requests[1].targetSize, QSize(1024, 768));
    EXPECT_EQ(requests[1].progression, PreviewProgression::FinalOnly);
    EXPECT_EQ(requests[1].renderMode, PreviewRenderMode::Final);
    EXPECT_LT(requests[0].previewSequence, requests[1].previewSequence);
}

TEST(EditorOrchestratorTest, ThrottlesInteractiveBurstAndSubmitsFinalOnEditEnd)
{
    (void)test::application();
    EditorTestCatalog catalog;
    ASSERT_TRUE(catalog.isValid());
    auto pipeline = std::make_unique<RecordingPreviewPipeline>();
    RecordingPreviewPipeline* const pipelinePointer = pipeline.get();
    PreviewOrchestrator previewOrchestrator(std::move(pipeline));
    EditorOrchestrator editorOrchestrator(previewOrchestrator, catalog.orchestrator());
    const std::optional<catalog::CatalogEntry> entry =
        catalog.createEntry(QStringLiteral("interactive.cr3"), types::SupportedFileKind::Raw);
    ASSERT_TRUE(entry.has_value());
    ASSERT_TRUE(editorOrchestrator.activatePhoto(*entry, QSize{1200, 800}).hasValue());
    editorOrchestrator.beginEdit();
    types::DevelopParams firstParams;
    firstParams.exposureEv = 0.25F;
    types::DevelopParams middleParams = firstParams;
    middleParams.exposureEv = 0.5F;
    types::DevelopParams latestParams = firstParams;
    latestParams.exposureEv = 0.75F;

    ASSERT_TRUE(editorOrchestrator.updateDevelopParams(firstParams, QSize{1200, 800}));
    ASSERT_TRUE(pipelinePointer->requestRecorded.tryAcquire(1, 2000));
    ASSERT_TRUE(editorOrchestrator.updateDevelopParams(middleParams, QSize{1200, 800}));
    ASSERT_TRUE(editorOrchestrator.updateDevelopParams(latestParams, QSize{1200, 800}));
    ASSERT_TRUE(waitForRequestCount(*pipelinePointer, 2));

    std::vector<PreviewRequest> requests = pipelinePointer->requests();
    ASSERT_EQ(requests.size(), 2U);
    EXPECT_EQ(requests[0].params, firstParams);
    EXPECT_EQ(requests[0].progression, PreviewProgression::FinalOnly);
    EXPECT_EQ(requests[0].renderMode, PreviewRenderMode::Interactive);
    EXPECT_EQ(requests[1].params, latestParams);
    EXPECT_EQ(requests[1].progression, PreviewProgression::FinalOnly);
    EXPECT_EQ(requests[1].renderMode, PreviewRenderMode::Interactive);
    EXPECT_LT(requests[0].previewSequence, requests[1].previewSequence);

    editorOrchestrator.endEdit();
    ASSERT_TRUE(waitForRequestCount(*pipelinePointer, 3));

    requests = pipelinePointer->requests();
    ASSERT_EQ(requests.size(), 3U);
    EXPECT_EQ(requests[2].params, latestParams);
    EXPECT_EQ(requests[2].targetSize, QSize(1200, 800));
    EXPECT_EQ(requests[2].progression, PreviewProgression::FinalOnly);
    EXPECT_EQ(requests[2].renderMode, PreviewRenderMode::Final);
    EXPECT_LT(requests[1].previewSequence, requests[2].previewSequence);
}

TEST(EditorOrchestratorTest, FiltersCancelledSelectionBeforePublishingLatestFrame)
{
    (void)test::application();
    EditorTestCatalog catalog;
    ASSERT_TRUE(catalog.isValid());
    auto pipeline = std::make_unique<BlockingEditorPreviewPipeline>();
    BlockingEditorPreviewPipeline* const pipelinePointer = pipeline.get();
    PreviewOrchestrator previewOrchestrator(std::move(pipeline));
    EditorOrchestrator editorOrchestrator(previewOrchestrator, catalog.orchestrator());
    std::vector<QString> publishedPaths;
    std::vector<types::RequestId> startedRequests;
    std::vector<types::RequestId> cancelledRequests;
    QObject::connect(&editorOrchestrator, &EditorOrchestrator::previewStarted, [&](types::RequestId requestId) {
        startedRequests.push_back(requestId);
    });
    QObject::connect(&editorOrchestrator, &EditorOrchestrator::previewCancelled, [&](types::RequestId requestId) {
        cancelledRequests.push_back(requestId);
    });
    QObject::connect(&editorOrchestrator, &EditorOrchestrator::previewUpdated, [&](const PreviewResult& result) {
        publishedPaths.push_back(result.sourcePath);
    });

    const std::optional<catalog::CatalogEntry> firstEntry =
        catalog.createEntry(QStringLiteral("first.bmp"), types::SupportedFileKind::RasterImage);
    const std::optional<catalog::CatalogEntry> secondEntry =
        catalog.createEntry(QStringLiteral("second.bmp"), types::SupportedFileKind::RasterImage);
    ASSERT_TRUE(firstEntry.has_value());
    ASSERT_TRUE(secondEntry.has_value());
    ASSERT_TRUE(editorOrchestrator.activatePhoto(*firstEntry, QSize{640, 480}).hasValue());
    QEventLoop startLoop;
    bool firstRenderStarted = false;
    QTimer pollTimer;
    QObject::connect(&pollTimer, &QTimer::timeout, [&] {
        if (pipelinePointer->firstRenderStarted.tryAcquire(1))
        {
            firstRenderStarted = true;
            startLoop.quit();
        }
    });
    pollTimer.start(10);
    QTimer::singleShot(2000, &startLoop, &QEventLoop::quit);
    startLoop.exec();

    if (!firstRenderStarted)
    {
        pipelinePointer->allowFirstRenderToFinish.release();
    }

    ASSERT_TRUE(firstRenderStarted);
    ASSERT_EQ(1U, startedRequests.size());
    const types::RequestId firstRequestId = startedRequests.front();

    ASSERT_TRUE(editorOrchestrator.activatePhoto(*secondEntry, QSize{640, 480}).hasValue());
    ASSERT_EQ(1U, cancelledRequests.size());
    EXPECT_EQ(firstRequestId, cancelledRequests.front());
    pipelinePointer->allowFirstRenderToFinish.release();
    QEventLoop resultLoop;
    QObject::connect(&editorOrchestrator, &EditorOrchestrator::previewUpdated, &resultLoop, &QEventLoop::quit);
    QTimer::singleShot(2000, &resultLoop, &QEventLoop::quit);
    resultLoop.exec();

    ASSERT_EQ(publishedPaths.size(), 1U);
    EXPECT_EQ(publishedPaths.front(), secondEntry->file.path);
}

}  // namespace
}  // namespace flexraw::core::orchestration
