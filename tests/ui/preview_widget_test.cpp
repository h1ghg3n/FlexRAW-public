#include <QApplication>
#include <QImage>
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>

#include <gtest/gtest.h>

#include "preview_widget.h"

namespace flexraw::ui::editor
{
namespace
{

// 목적: Preview frame 중앙과 가장자리의 표시 위치를 구별하는 image 생성
// 입력: 없음
// 출력: 중앙이 파란색이고 가장자리가 빨간색인 640x480 image
[[nodiscard]] QImage navigationImage()
{
    QImage image(640, 480, QImage::Format_RGB32);
    image.fill(Qt::red);
    QPainter painter(&image);
    painter.fillRect(160, 120, 320, 240, Qt::blue);
    return image;
}

// 목적: Qt test dependency 없이 PreviewWidget에 pointer event 전달
// 입력: widget: event 대상, type/position/button/buttons: pointer event 정보
// 출력: synchronous event handler 실행
void sendMouseEvent(
    PreviewWidget& widget, QEvent::Type type, const QPoint& position, Qt::MouseButton button, Qt::MouseButtons buttons)
{
    QMouseEvent event(type, QPointF(position), QPointF(widget.mapToGlobal(position)), button, buttons, Qt::NoModifier);
    QApplication::sendEvent(&widget, &event);
}

// 목적: widget의 현재 표시 frame에서 지정한 pixel 색 확인
// 입력: widget: PreviewWidget, position: widget pixel 좌표
// 출력: 현재 화면의 pixel 색
[[nodiscard]] QColor displayedColor(const PreviewWidget& widget, const QPoint& position)
{
    const QPixmap pixmap = widget.pixmap(Qt::ReturnByValue);
    return pixmap.toImage().pixelColor(position);
}

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

TEST(PreviewWidgetTest, FitsThenShowsPreviewPixelsAtPhysical100Percent)
{
    PreviewWidget widget;
    widget.resize(320, 240);
    widget.show();
    widget.showPreview(navigationImage());

    EXPECT_EQ(displayedColor(widget, {5, 5}), QColor(Qt::red));
    widget.showPreviewPixelsAt100Percent();
    EXPECT_EQ(displayedColor(widget, {5, 5}), QColor(Qt::blue));
    EXPECT_EQ(widget.cursor().shape(), Qt::OpenHandCursor);

    widget.zoomOut();
    EXPECT_EQ(displayedColor(widget, {5, 5}), QColor(Qt::red));
    widget.zoomIn();
    EXPECT_EQ(displayedColor(widget, {5, 5}), QColor(Qt::blue));
    widget.fitToWindow();
    EXPECT_EQ(displayedColor(widget, {5, 5}), QColor(Qt::red));
    EXPECT_NE(widget.cursor().shape(), Qt::OpenHandCursor);
}

TEST(PreviewWidgetTest, PanClampsToFrameAndKeepsViewThroughUpdatedFrame)
{
    PreviewWidget widget;
    widget.resize(320, 240);
    widget.show();
    widget.showPreview(navigationImage());
    widget.showPreviewPixelsAt100Percent();

    sendMouseEvent(widget, QEvent::MouseButtonPress, {160, 120}, Qt::LeftButton, Qt::LeftButton);
    sendMouseEvent(widget, QEvent::MouseMove, {1000, 900}, Qt::NoButton, Qt::LeftButton);
    sendMouseEvent(widget, QEvent::MouseButtonRelease, {1000, 900}, Qt::LeftButton, Qt::NoButton);
    EXPECT_EQ(displayedColor(widget, {5, 5}), QColor(Qt::red));
    EXPECT_EQ(widget.cursor().shape(), Qt::OpenHandCursor);

    QImage updated = navigationImage();
    updated.setPixelColor(5, 5, Qt::green);
    widget.showPreview(updated);
    EXPECT_EQ(displayedColor(widget, {5, 5}), QColor(Qt::green));

    QImage larger(1280, 960, QImage::Format_RGB32);
    larger.fill(Qt::red);
    QPainter painter(&larger);
    painter.fillRect(0, 0, 320, 240, Qt::green);
    painter.end();
    widget.showPreview(larger);
    EXPECT_EQ(displayedColor(widget, {5, 5}), QColor(Qt::green));

    widget.showMessage(QStringLiteral("Loading preview..."));
    EXPECT_TRUE(widget.pixmap(Qt::ReturnByValue).isNull());
    widget.showPreview(navigationImage());
    EXPECT_EQ(displayedColor(widget, {5, 5}), QColor(Qt::red));
    EXPECT_NE(widget.cursor().shape(), Qt::OpenHandCursor);
}

TEST(PreviewWidgetTest, WheelZoomsAroundPointerAndRespectsFrameBounds)
{
    PreviewWidget widget;
    widget.resize(320, 240);
    widget.show();
    QImage image(640, 480, QImage::Format_RGB32);
    image.fill(Qt::red);
    QPainter painter(&image);
    painter.fillRect(85, 85, 30, 30, Qt::green);
    painter.end();
    widget.showPreview(image);
    const QPoint anchor(50, 50);
    ASSERT_EQ(displayedColor(widget, anchor), QColor(Qt::green));
    QWheelEvent wheel(QPointF(anchor),
                      QPointF(widget.mapToGlobal(anchor)),
                      {},
                      {0, 120},
                      Qt::NoButton,
                      Qt::NoModifier,
                      Qt::NoScrollPhase,
                      false);
    QApplication::sendEvent(&widget, &wheel);

    EXPECT_TRUE(wheel.isAccepted());
    EXPECT_FALSE(widget.pixmap(Qt::ReturnByValue).isNull());
    EXPECT_EQ(widget.cursor().shape(), Qt::OpenHandCursor);
    EXPECT_EQ(displayedColor(widget, anchor), QColor(Qt::green));

    QWheelEvent pixelWheel(QPointF(anchor),
                           QPointF(widget.mapToGlobal(anchor)),
                           {0, 120},
                           {},
                           Qt::NoButton,
                           Qt::NoModifier,
                           Qt::ScrollUpdate,
                           false);
    QApplication::sendEvent(&widget, &pixelWheel);
    EXPECT_TRUE(pixelWheel.isAccepted());
    EXPECT_EQ(displayedColor(widget, anchor), QColor(Qt::green));
}

TEST(PreviewWidgetTest, SplitBeforeAndAfterSharesZoomPanAndDraggableDivider)
{
    PreviewWidget widget;
    widget.resize(320, 240);
    widget.show();
    QImage after(640, 480, QImage::Format_RGB32);
    after.fill(Qt::blue);
    QImage before(320, 240, QImage::Format_RGB32);
    before.fill(Qt::red);
    widget.showPreview(after);
    widget.setComparisonPreview(before);
    widget.setComparisonDisplayMode(PreviewWidget::ComparisonDisplayMode::Split);

    EXPECT_EQ(displayedColor(widget, {40, 120}), QColor(Qt::red));
    EXPECT_EQ(displayedColor(widget, {280, 120}), QColor(Qt::blue));
    sendMouseEvent(widget, QEvent::MouseButtonPress, {160, 120}, Qt::LeftButton, Qt::LeftButton);
    sendMouseEvent(widget, QEvent::MouseMove, {240, 120}, Qt::NoButton, Qt::LeftButton);
    sendMouseEvent(widget, QEvent::MouseButtonRelease, {240, 120}, Qt::LeftButton, Qt::NoButton);
    EXPECT_EQ(displayedColor(widget, {200, 120}), QColor(Qt::red));
    EXPECT_EQ(displayedColor(widget, {280, 120}), QColor(Qt::blue));

    sendMouseEvent(widget, QEvent::MouseButtonPress, {280, 120}, Qt::RightButton, Qt::RightButton);
    EXPECT_EQ(displayedColor(widget, {280, 120}), QColor(Qt::red));
    sendMouseEvent(widget, QEvent::MouseButtonRelease, {280, 120}, Qt::RightButton, Qt::NoButton);
    EXPECT_EQ(displayedColor(widget, {280, 120}), QColor(Qt::blue));

    widget.showPreviewPixelsAt100Percent();
    EXPECT_EQ(displayedColor(widget, {200, 120}), QColor(Qt::red));
    widget.setComparisonDisplayMode(PreviewWidget::ComparisonDisplayMode::Before);
    EXPECT_EQ(displayedColor(widget, {280, 120}), QColor(Qt::red));
    widget.clearComparisonPreview();
    EXPECT_EQ(displayedColor(widget, {40, 120}), QColor(Qt::blue));
}

}  // namespace
}  // namespace flexraw::ui::editor
