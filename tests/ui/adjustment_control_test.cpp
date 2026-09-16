#include <QAbstractSpinBox>
#include <QApplication>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QMouseEvent>
#include <QObject>
#include <QSlider>
#include <QThread>

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

// 목적: Qt timer event를 지정 시간 동안 처리해 rate control 누적값 관찰
// 입력: milliseconds: event loop를 진행할 최소 시간
// 출력: 대기 중 발생한 timer와 repaint event 처리
void processEventsFor(int milliseconds)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < milliseconds)
    {
        QApplication::processEvents();
        QThread::msleep(1);
    }
    QApplication::processEvents();
}

// 목적: Exposure 공통 parameter control test configuration 생성
// 입력: 없음
// 출력: -5~+5 EV, 최대 0.8 EV/s configuration
[[nodiscard]] AdjustmentParameterConfiguration exposureConfiguration()
{
    AdjustmentParameterConfiguration configuration;
    configuration.label = QStringLiteral("Exposure");
    configuration.sliderObjectName = QStringLiteral("exposureSlider");
    configuration.suffix = QStringLiteral(" EV");
    configuration.minimum = -5.0;
    configuration.maximum = 5.0;
    configuration.sliderScale = 100.0;
    configuration.displayScale = 1.0;
    configuration.singleStep = 0.1;
    configuration.maximumRate = 0.8;
    configuration.displayDecimals = 2;
    return configuration;
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

TEST(RelativeAdjustmentControlTest, KeepsWideGeometryCompactAtNarrowDrawerWidth)
{
    RelativeAdjustmentControl control;

    EXPECT_EQ(QSize(240, 30), control.sizeHint());
    EXPECT_EQ(QSize(140, 28), control.minimumSizeHint());
}

TEST(RelativeAdjustmentControlTest, MaximumDistanceContinuesNearConfiguredRateUntilRelease)
{
    RelativeAdjustmentControl control;
    control.resize(240, 36);
    control.setMaximumRate(0.8);
    control.setResolution(0.01);
    const QPoint pressPosition = control.rect().center();
    const QPoint dragPosition = pressPosition + QPoint(76, 0);

    sendMouseEvent(control, QEvent::MouseButtonPress, pressPosition, Qt::LeftButton, Qt::LeftButton);
    sendMouseEvent(control, QEvent::MouseMove, dragPosition, Qt::NoButton, Qt::LeftButton);
    QElapsedTimer rateMeasurement;
    rateMeasurement.start();
    processEventsFor(240);
    const double firstValue = control.value();
    processEventsFor(240);
    const double secondValue = control.value();
    const double elapsedSeconds = static_cast<double>(rateMeasurement.nsecsElapsed()) / 1'000'000'000.0;
    sendMouseEvent(control, QEvent::MouseButtonRelease, dragPosition, Qt::LeftButton, Qt::NoButton);
    const double releasedValue = control.value();
    processEventsFor(80);

    EXPECT_GT(firstValue, 0.1);
    EXPECT_LT(firstValue, 0.25);
    EXPECT_GT(secondValue, firstValue + 0.1);
    EXPECT_LT(secondValue, 0.45);
    EXPECT_NEAR(0.8, secondValue / elapsedSeconds, 0.12);
    EXPECT_DOUBLE_EQ(releasedValue, control.value());
}

TEST(AdjustmentParameterControlTest, ClassicTrackClickPublishesOneAdjustmentTransaction)
{
    AdjustmentParameterControl control(exposureConfiguration());
    control.resize(280, 72);
    control.show();
    QApplication::processEvents();
    QSlider* slider = control.findChild<QSlider*>(QStringLiteral("exposureSlider"));
    ASSERT_NE(nullptr, slider);
    int startedCount = 0;
    int finishedCount = 0;
    QObject::connect(&control, &AdjustmentParameterControl::adjustmentStarted, [&startedCount] { ++startedCount; });
    QObject::connect(&control, &AdjustmentParameterControl::adjustmentFinished, [&finishedCount] { ++finishedCount; });
    const QPoint trackPosition((slider->width() * 3) / 4, slider->height() / 2);

    sendMouseEvent(*slider, QEvent::MouseButtonPress, trackPosition, Qt::LeftButton, Qt::LeftButton);
    sendMouseEvent(*slider, QEvent::MouseButtonRelease, trackPosition, Qt::LeftButton, Qt::NoButton);

    EXPECT_GT(control.value(), 2.0);
    EXPECT_EQ(1, startedCount);
    EXPECT_EQ(1, finishedCount);
}

TEST(AdjustmentParameterControlTest, StyleSwitchPreservesValueWithoutPublishingEdit)
{
    AdjustmentParameterControl control(exposureConfiguration());
    control.setValue(0.35);
    control.show();
    QApplication::processEvents();
    const int classicHeight = control.sizeHint().height();
    int changedCount = 0;
    QObject::connect(&control, &AdjustmentParameterControl::valueChanged, [&changedCount](double) { ++changedCount; });

    control.setControlStyle(AdjustmentControlStyle::Relative);
    QApplication::processEvents();

    EXPECT_EQ(AdjustmentControlStyle::Relative, control.controlStyle());
    EXPECT_GT(control.sizeHint().height(), classicHeight);
    EXPECT_DOUBLE_EQ(0.35, control.value());
    EXPECT_EQ(0, changedCount);
    auto* relative = control.findChild<RelativeAdjustmentControl*>(QStringLiteral("exposureRelativeControl"));
    ASSERT_NE(nullptr, relative);
    EXPECT_DOUBLE_EQ(0.35, relative->value());
    QDoubleSpinBox* valueSpinBox = control.findChild<QDoubleSpinBox*>(QStringLiteral("exposureValueSpinBox"));
    ASSERT_NE(nullptr, valueSpinBox);
    EXPECT_EQ(64, valueSpinBox->minimumWidth());
    EXPECT_EQ(64, valueSpinBox->maximumWidth());
    EXPECT_EQ(QAbstractSpinBox::NoButtons, valueSpinBox->buttonSymbols());
    EXPECT_TRUE(valueSpinBox->text().startsWith(QStringLiteral("+0.35")));
}

}  // namespace
}  // namespace flexraw::ui::editor
