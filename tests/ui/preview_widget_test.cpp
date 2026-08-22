#include <QApplication>
#include <QImage>

#include <gtest/gtest.h>

#include "preview_widget.h"

namespace flexraw::ui::editor
{
namespace
{

TEST(PreviewWidgetTest, RendersPreviewAfterResize)
{
    PreviewWidget widget;
    widget.resize(960, 640);
    widget.show();

    QImage image(2400, 1600, QImage::Format_RGBA8888);
    image.fill(Qt::red);
    widget.showPreview(image);
    widget.resize(720, 480);
    QApplication::processEvents();

    EXPECT_FALSE(widget.pixmap(Qt::ReturnByValue).isNull());
}

TEST(PreviewWidgetTest, PublishesViewportSizeAfterResize)
{
    PreviewWidget widget;
    QSize observedSize;
    int resizeCount = 0;
    QObject::connect(&widget, &PreviewWidget::viewportSizeChanged, [&](const QSize& size) {
        observedSize = size;
        ++resizeCount;
    });
    widget.resize(960, 640);
    widget.show();
    QApplication::processEvents();

    widget.resize(720, 480);
    QApplication::processEvents();

    EXPECT_GT(resizeCount, 0);
    EXPECT_EQ(observedSize, QSize(720, 480));
}

}  // namespace
}  // namespace flexraw::ui::editor
