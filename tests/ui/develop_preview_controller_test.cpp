#include <QColor>
#include <QEventLoop>
#include <QImage>
#include <QObject>
#include <QTimer>

#include <gtest/gtest.h>

#include "develop_preview_controller.h"

namespace flexraw::ui::editor
{
namespace
{

TEST(DevelopPreviewControllerTest, AppliesUpdatedParamsToSourcePreview)
{
    DevelopPreviewController controller;
    QImage renderedImage;
    QEventLoop eventLoop;
    QTimer timeoutTimer;
    timeoutTimer.setSingleShot(true);
    QObject::connect(&timeoutTimer, &QTimer::timeout, &eventLoop, &QEventLoop::quit);
    QObject::connect(&controller,
                     &DevelopPreviewController::previewReady,
                     [&renderedImage, &eventLoop](const QImage& image,
                                                  const core::develop::ImageHistogram&,
                                                  const core::develop::ClippingSummary&) {
                         renderedImage = image;
                         eventLoop.quit();
                     });

    QImage sourceImage(8, 8, QImage::Format_RGBA8888);
    sourceImage.fill(QColor(64, 64, 64));
    controller.setSourceImage(sourceImage);
    timeoutTimer.start(5000);
    eventLoop.exec();
    ASSERT_FALSE(renderedImage.isNull());
    const int originalRed = renderedImage.pixelColor(0, 0).red();

    core::types::DevelopParams params;
    params.exposureEv = 1.0F;
    controller.setParams(params);
    timeoutTimer.start(5000);
    eventLoop.exec();

    EXPECT_GT(renderedImage.pixelColor(0, 0).red(), originalRed);
}

}  // namespace
}  // namespace flexraw::ui::editor
