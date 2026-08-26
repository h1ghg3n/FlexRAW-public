#include <QApplication>
#include <QDoubleSpinBox>
#include <QMouseEvent>
#include <QObject>
#include <QSlider>

#include <gtest/gtest.h>

#include "adjustment_control.h"

namespace flexraw::ui::editor
{
namespace
{

// 목적: Qt test dependency 없이 widget에 pointer event 전달
// 입력: widget: event 대상, type/position/button/buttons: QMouseEvent field
// 출력: synchronous event handler 실행
void sendMouseEvent(
    QWidget& widget, QEvent::Type type, const QPoint& position, Qt::MouseButton button, Qt::MouseButtons buttons)
{
    const QPoint globalPosition = widget.mapToGlobal(position);
    QMouseEvent event(type, QPointF(position), QPointF(globalPosition), button, buttons, Qt::NoModifier);
    QApplication::sendEvent(&widget, &event);
}

TEST(RelativeAdjustmentControlTest, AppliesOneStepOnlyWhenShortClickIsReleased)
{
    RelativeAdjustmentControl control;
    control.resize(240, 36);
    int startedCount = 0;
    int finishedCount = 0;
    int changedCount = 0;
    QObject::connect(&control, &RelativeAdjustmentControl::adjustmentStarted, [&startedCount] { ++startedCount; });
    QObject::connect(&control, &RelativeAdjustmentControl::adjustmentFinished, [&finishedCount] { ++finishedCount; });
    QObject::connect(&control, &RelativeAdjustmentControl::valueChanged, [&changedCount](double) { ++changedCount; });
    const QPoint leftHalf = control.rect().center() - QPoint(18, 0);

    sendMouseEvent(control, QEvent::MouseButtonPress, leftHalf, Qt::LeftButton, Qt::LeftButton);

    EXPECT_DOUBLE_EQ(0.0, control.value());
    EXPECT_EQ(1, startedCount);
    EXPECT_EQ(0, changedCount);

    sendMouseEvent(control, QEvent::MouseButtonRelease, leftHalf, Qt::LeftButton, Qt::NoButton);

    EXPECT_DOUBLE_EQ(-0.1, control.value());
    EXPECT_EQ(1, changedCount);
    EXPECT_EQ(1, finishedCount);
}

TEST(RelativeAdjustmentControlTest, DragNeverFallsBackToClickAndKeepsAbsoluteValue)
{
    RelativeAdjustmentControl control;
    control.resize(240, 36);
    const QPoint pressPosition = control.rect().center() - QPoint(18, 0);
    const QPoint dragPosition = pressPosition + QPoint(70, 0);

    sendMouseEvent(control, QEvent::MouseButtonPress, pressPosition, Qt::LeftButton, Qt::LeftButton);
    sendMouseEvent(control, QEvent::MouseMove, dragPosition, Qt::NoButton, Qt::LeftButton);
    const double draggedValue = control.value();
    sendMouseEvent(control, QEvent::MouseButtonRelease, dragPosition, Qt::LeftButton, Qt::NoButton);

    EXPECT_GT(draggedValue, 0.1);
    EXPECT_LE(draggedValue, 0.5);
    EXPECT_DOUBLE_EQ(draggedValue, control.value());
}

TEST(ExposureAdjustmentControlTest, ClassicTrackClickPublishesOneAdjustmentTransaction)
{
    ExposureAdjustmentControl control;
    control.resize(280, 72);
    control.show();
    QApplication::processEvents();
    QSlider* slider = control.findChild<QSlider*>(QStringLiteral("exposureSlider"));
    ASSERT_NE(nullptr, slider);
    int startedCount = 0;
    int finishedCount = 0;
    QObject::connect(&control, &ExposureAdjustmentControl::adjustmentStarted, [&startedCount] { ++startedCount; });
    QObject::connect(&control, &ExposureAdjustmentControl::adjustmentFinished, [&finishedCount] { ++finishedCount; });
    const QPoint trackPosition((slider->width() * 3) / 4, slider->height() / 2);

    sendMouseEvent(*slider, QEvent::MouseButtonPress, trackPosition, Qt::LeftButton, Qt::LeftButton);
    sendMouseEvent(*slider, QEvent::MouseButtonRelease, trackPosition, Qt::LeftButton, Qt::NoButton);

    EXPECT_GT(control.value(), 2.0);
    EXPECT_EQ(1, startedCount);
    EXPECT_EQ(1, finishedCount);
}

TEST(ExposureAdjustmentControlTest, StyleSwitchPreservesValueWithoutPublishingEdit)
{
    ExposureAdjustmentControl control;
    control.setValue(0.35);
    int changedCount = 0;
    QObject::connect(&control, &ExposureAdjustmentControl::valueChanged, [&changedCount](double) { ++changedCount; });

    control.setControlStyle(AdjustmentControlStyle::Relative);

    EXPECT_EQ(AdjustmentControlStyle::Relative, control.controlStyle());
    EXPECT_DOUBLE_EQ(0.35, control.value());
    EXPECT_EQ(0, changedCount);
    auto* relative = control.findChild<RelativeAdjustmentControl*>(QStringLiteral("exposureRelativeControl"));
    ASSERT_NE(nullptr, relative);
    EXPECT_DOUBLE_EQ(0.35, relative->value());
    QDoubleSpinBox* valueSpinBox = control.findChild<QDoubleSpinBox*>(QStringLiteral("exposureValueSpinBox"));
    ASSERT_NE(nullptr, valueSpinBox);
    EXPECT_TRUE(valueSpinBox->text().startsWith(QStringLiteral("+0.35")));
}

}  // namespace
}  // namespace flexraw::ui::editor
