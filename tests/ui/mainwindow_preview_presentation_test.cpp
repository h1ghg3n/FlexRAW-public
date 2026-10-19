#include <functional>
#include <memory>
#include <utility>

#include <QAction>
#include <QApplication>
#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QImage>
#include <QLabel>
#include <QMouseEvent>
#include <QSettings>
#include <QSlider>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QThread>

#include <gtest/gtest.h>

#include "catalog_editor_facade.h"
#include "catalog_orchestrator.h"
#include "catalog_session_orchestrator.h"
#include "catalog_thumbnail_orchestrator.h"
#include "catalog_thumbnail_pipeline.h"
#include "editor_orchestrator.h"
#include "export_orchestrator.h"
#include "export_pipeline.h"
#include "mainwindow.h"
#include "preview_orchestrator.h"
#include "preview_pipeline.h"
#include "preview_widget.h"
#include "qt_export_client_adapter.h"
#include "qt_export_settings_adapter.h"
#include "qt_source_resolution_event_source.h"
#include "qt_worker_profile_settings_adapter.h"
#include "test_path_identity_service.h"

namespace flexraw::ui::mainwindow
{
namespace
{

class MainWindowPreviewPipeline final : public core::orchestration::IPreviewPipeline
{
public:
    // 목적: MainWindow contract consumer test용 deterministic Preview frame과 analysis 생성
    // 입력: request/tier/cancellationToken: identity와 cancellation contract를 위한 context
    // 출력: known-color 2x1 image와 fixed histogram/clipping
    [[nodiscard]] core::orchestration::PreviewPipelineResult render(
        const core::orchestration::PreviewRequest& request,
        core::orchestration::PreviewTier,
        const core::types::CancellationToken& cancellationToken) override
    {
        if (cancellationToken.isCancellationRequested())
        {
            return core::orchestration::PreviewPipelineResult::failure(
                {core::types::ErrorCode::Cancelled, QStringLiteral("MainWindow Preview test request was cancelled.")});
        }
        QImage image(2, 1, QImage::Format_RGBA8888);
        image.setPixelColor(0, 0, QColor(10, 20, 30));
        image.setPixelColor(1, 0, QColor(40, 50, 60));
        if (request.params.exposureEv != 0.0F)
        {
            image.fill(Qt::blue);
        }
        core::develop::ImageHistogram histogram;
        histogram.red[10] = 2;
        histogram.pixelCount = 2;
        core::develop::ClippingSummary clipping;
        clipping.highlightPixelCount = request.params.exposureEv == 0.0F ? 0 : 2;
        clipping.pixelCount = 2;
        return core::orchestration::PreviewPipelineResult::success(
            {std::move(image), std::move(histogram), clipping, {}});
    }
};

// 목적: Qt event를 처리하며 MainWindow Preview presentation predicate를 bounded wait
// 입력: condition: widget 상태 predicate, timeoutMs: 제한 시간
// 출력: 제한 시간 안에 condition이 true이면 true
[[nodiscard]] bool waitForCondition(const std::function<bool()>& condition, int timeoutMs = 3000)
{
    QElapsedTimer timeout;
    timeout.start();
    while (timeout.elapsed() < timeoutMs)
    {
        if (condition())
        {
            return true;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
    return condition();
}

TEST(MainWindowPreviewPresentationTest, ConsumesFrameThroughQtFreePreviewContract)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString sourcePath = QDir(directory.path()).filePath(QStringLiteral("presented.jpg"));
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("presented.flexraw-catalog"));
    QImage source(2, 1, QImage::Format_RGB32);
    source.fill(Qt::white);
    ASSERT_TRUE(source.save(sourcePath));

    core::orchestration::PreviewOrchestrator previewOrchestrator(std::make_unique<MainWindowPreviewPipeline>());
    core::orchestration::CatalogOrchestrator catalogOrchestrator;
    ASSERT_TRUE(catalogOrchestrator.openCatalog(catalogPath).hasValue());
    core::orchestration::CatalogThumbnailOrchestrator catalogThumbnailOrchestrator(
        std::make_unique<core::orchestration::FileCatalogThumbnailPipeline>());
    core::orchestration::EditorOrchestrator editorOrchestrator(previewOrchestrator, catalogOrchestrator);
    core::orchestration::CatalogSessionOrchestrator catalogSessionOrchestrator(catalogOrchestrator, editorOrchestrator);
    core::orchestration::QtSourceResolutionEventSource sourceResolutionEventSource(catalogOrchestrator);
    facade::CatalogEditorFacade catalogEditorFacade(catalogOrchestrator,
                                                    catalogSessionOrchestrator,
                                                    catalogThumbnailOrchestrator,
                                                    editorOrchestrator,
                                                    sourceResolutionEventSource);
    core::orchestration::ExportOrchestrator exportOrchestrator(
        std::make_unique<core::orchestration::FileExportPipeline>(::flexraw::test::testPathIdentityService()));
    QSettings applicationSettings(QDir(directory.path()).filePath(QStringLiteral("settings.ini")),
                                  QSettings::IniFormat);
    settings::QtWorkerProfileSettingsAdapter workerProfiles(applicationSettings);
    settings::QtExportSettingsAdapter exportDefaults(applicationSettings);
    export_::QtExportClientAdapter exportAdapter(exportOrchestrator, workerProfiles);
    MainWindow window(
        catalogEditorFacade, exportAdapter, exportAdapter, exportDefaults, applicationSettings, workerProfiles);
    auto* previewWidget = window.findChild<editor::PreviewWidget*>();
    ASSERT_NE(nullptr, previewWidget);
    auto* highlightClippingIndicator = window.findChild<QLabel*>(QStringLiteral("highlightClippingIndicator"));
    ASSERT_NE(nullptr, highlightClippingIndicator);
    auto* exposureSlider = window.findChild<QSlider*>(QStringLiteral("exposureSlider"));
    ASSERT_NE(nullptr, exposureSlider);
    auto* fitAction = window.findChild<QAction*>(QStringLiteral("fitPreviewAction"));
    auto* previewPixelsAction = window.findChild<QAction*>(QStringLiteral("previewPixelsAction"));
    auto* zoomInAction = window.findChild<QAction*>(QStringLiteral("zoomPreviewInAction"));
    auto* zoomOutAction = window.findChild<QAction*>(QStringLiteral("zoomPreviewOutAction"));
    auto* beforeAction = window.findChild<QAction*>(QStringLiteral("beforePreviewAction"));
    auto* splitAction = window.findChild<QAction*>(QStringLiteral("splitPreviewAction"));
    ASSERT_NE(nullptr, fitAction);
    ASSERT_NE(nullptr, previewPixelsAction);
    ASSERT_NE(nullptr, zoomInAction);
    ASSERT_NE(nullptr, zoomOutAction);
    ASSERT_NE(nullptr, beforeAction);
    ASSERT_NE(nullptr, splitAction);
    EXPECT_FALSE(fitAction->shortcut().isEmpty());
    EXPECT_FALSE(previewPixelsAction->shortcut().isEmpty());
    EXPECT_FALSE(zoomInAction->shortcuts().isEmpty());
    EXPECT_FALSE(zoomOutAction->shortcuts().isEmpty());
    const core::catalog::CatalogEntry entry{
        {sourcePath,
         QStringLiteral("jpg"),
         QStringLiteral("presented.jpg"),
         core::types::SupportedFileKind::RasterImage},
        core::types::FileScanStatus::Ready,
    };

    ASSERT_TRUE(catalogEditorFacade.setPreviewViewport({{640, 480}}).hasValue());
    ASSERT_TRUE(
        catalogEditorFacade
            .activateSource(
                {sourcePath.toUtf8().toStdString(), "jpg", "presented.jpg", core::client::CatalogFileKind::RasterImage})
            .hasValue());

    ASSERT_TRUE(waitForCondition([previewWidget] {
        return previewWidget->text().isEmpty() && !previewWidget->pixmap(Qt::ReturnByValue).isNull();
    }));
    EXPECT_EQ(QStringLiteral("H 0.0%"), highlightClippingIndicator->text());
    EXPECT_TRUE(window.statusBar()->currentMessage().contains(QStringLiteral("presented.jpg")));

    const QPoint sample(previewWidget->width() / 4, previewWidget->height() / 2);
    const QColor fittedColor = previewWidget->pixmap(Qt::ReturnByValue).toImage().pixelColor(sample);
    previewPixelsAction->trigger();
    EXPECT_NE(previewWidget->pixmap(Qt::ReturnByValue).toImage().pixelColor(sample), fittedColor);
    fitAction->trigger();
    EXPECT_EQ(previewWidget->pixmap(Qt::ReturnByValue).toImage().pixelColor(sample), fittedColor);

    const QPoint exposurePosition(exposureSlider->width() - 2, exposureSlider->height() / 2);
    const QPoint exposureGlobalPosition = exposureSlider->mapToGlobal(exposurePosition);
    QMouseEvent exposurePress(QEvent::MouseButtonPress,
                              QPointF(exposurePosition),
                              QPointF(exposureGlobalPosition),
                              Qt::LeftButton,
                              Qt::LeftButton,
                              Qt::NoModifier);
    QApplication::sendEvent(exposureSlider, &exposurePress);
    QMouseEvent exposureRelease(QEvent::MouseButtonRelease,
                                QPointF(exposurePosition),
                                QPointF(exposureGlobalPosition),
                                Qt::LeftButton,
                                Qt::NoButton,
                                Qt::NoModifier);
    QApplication::sendEvent(exposureSlider, &exposureRelease);
    EXPECT_GT(catalogEditorFacade.editorSnapshot().params.exposureEv, 0.0F);
    ASSERT_TRUE(waitForCondition([previewWidget, sample] {
        return previewWidget->pixmap(Qt::ReturnByValue).toImage().pixelColor(sample) == QColor(Qt::blue);
    }));
    ASSERT_TRUE(waitForCondition(
        [highlightClippingIndicator] { return highlightClippingIndicator->text() == QStringLiteral("H 100.0%"); }));
    splitAction->trigger();
    ASSERT_TRUE(waitForCondition([previewWidget, fittedColor, sample] {
        return previewWidget->pixmap(Qt::ReturnByValue).toImage().pixelColor(sample) == fittedColor;
    }));
    EXPECT_EQ(
        previewWidget->pixmap(Qt::ReturnByValue).toImage().pixelColor({previewWidget->width() * 3 / 4, sample.y()}),
        QColor(Qt::blue));
    beforeAction->trigger();
    EXPECT_TRUE(beforeAction->isChecked());
    EXPECT_FALSE(splitAction->isChecked());
    beforeAction->trigger();
    EXPECT_EQ(previewWidget->pixmap(Qt::ReturnByValue).toImage().pixelColor(sample), QColor(Qt::blue));
}

}  // namespace
}  // namespace flexraw::ui::mainwindow
