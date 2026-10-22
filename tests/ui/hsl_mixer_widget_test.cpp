#include <array>

#include <QApplication>
#include <QDoubleSpinBox>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QObject>
#include <QSlider>
#include <QToolButton>

#include <gtest/gtest.h>

#include "adjustment_control.h"
#include "hsl_mixer_widget.h"

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

TEST(HslMixerWidgetTest, ShowsFixedChannelOrderWithTextAndColorIcons)
{
    HslMixerWidget widget;
    const std::array<QString, 8> objectNames = {
        QStringLiteral("hslRedButton"),
        QStringLiteral("hslOrangeButton"),
        QStringLiteral("hslYellowButton"),
        QStringLiteral("hslGreenButton"),
        QStringLiteral("hslAquaButton"),
        QStringLiteral("hslBlueButton"),
        QStringLiteral("hslPurpleButton"),
        QStringLiteral("hslMagentaButton"),
    };
    const std::array<QString, 8> labels = {
        QObject::tr("Red"),
        QObject::tr("Orange"),
        QObject::tr("Yellow"),
        QObject::tr("Green"),
        QObject::tr("Aqua"),
        QObject::tr("Blue"),
        QObject::tr("Purple"),
        QObject::tr("Magenta"),
    };

    for (std::size_t index = 0; index < objectNames.size(); ++index)
    {
        QToolButton* button = widget.findChild<QToolButton*>(objectNames[index]);
        ASSERT_NE(nullptr, button);
        EXPECT_EQ(labels[index], button->text());
        EXPECT_FALSE(button->icon().isNull());
        EXPECT_FALSE(button->accessibleName().isEmpty());
    }
    EXPECT_TRUE(widget.findChild<QToolButton*>(QStringLiteral("hslRedButton"))->isChecked());
}

TEST(HslMixerWidgetTest, ReflowsChannelGridForNarrowAndWideDrawerWidths)
{
    HslMixerWidget widget;
    widget.setFixedWidth(220);
    widget.resize(220, 500);
    widget.show();
    QApplication::processEvents();
    QToolButton* red = widget.findChild<QToolButton*>(QStringLiteral("hslRedButton"));
    QToolButton* yellow = widget.findChild<QToolButton*>(QStringLiteral("hslYellowButton"));
    ASSERT_NE(nullptr, red);
    ASSERT_NE(nullptr, yellow);
    EXPECT_NE(red->geometry().top(), yellow->geometry().top());

    widget.setFixedWidth(300);
    QApplication::processEvents();

    EXPECT_EQ(red->geometry().top(), yellow->geometry().top());
}

TEST(HslMixerWidgetTest, ExternalSnapshotUpdatesControlsWithoutPublishingEdit)
{
    HslMixerWidget widget;
    int changedCount = 0;
    int startedCount = 0;
    int finishedCount = 0;
    QObject::connect(
        &widget, &HslMixerWidget::hslChanged, [&changedCount](const core::types::HslParams&) { ++changedCount; });
    QObject::connect(&widget, &HslMixerWidget::adjustmentStarted, [&startedCount] { ++startedCount; });
    QObject::connect(&widget, &HslMixerWidget::adjustmentFinished, [&finishedCount] { ++finishedCount; });
    core::types::HslParams params;
    params.channels[5].hue = 0.45F;

    widget.setHsl(params);
    widget.findChild<QToolButton*>(QStringLiteral("hslBlueButton"))->click();

    QSlider* hue = widget.findChild<QSlider*>(QStringLiteral("hslHueSlider"));
    ASSERT_NE(nullptr, hue);
    EXPECT_EQ(45, hue->value());
    EXPECT_EQ(5, widget.selectedChannelIndex());
    EXPECT_EQ(0, changedCount);
    EXPECT_EQ(0, startedCount);
    EXPECT_EQ(0, finishedCount);
}

TEST(HslMixerWidgetTest, SelectedChannelEditPreservesEveryOtherChannel)
{
    HslMixerWidget widget;
    core::types::HslParams initial;
    initial.channels[0].hue = -0.4F;
    initial.channels[5].luminance = 0.3F;
    widget.setHsl(initial);
    widget.findChild<QToolButton*>(QStringLiteral("hslBlueButton"))->click();
    core::types::HslParams published;
    int changedCount = 0;
    QObject::connect(
        &widget, &HslMixerWidget::hslChanged, [&published, &changedCount](const core::types::HslParams& hsl) {
            published = hsl;
            ++changedCount;
        });

    widget.findChild<QSlider*>(QStringLiteral("hslSaturationSlider"))->setValue(25);

    ASSERT_EQ(1, changedCount);
    EXPECT_FLOAT_EQ(-0.4F, published.channels[0].hue);
    EXPECT_FLOAT_EQ(0.25F, published.channels[5].saturation);
    EXPECT_FLOAT_EQ(0.3F, published.channels[5].luminance);
    for (std::size_t index = 1; index < published.channels.size(); ++index)
    {
        if (index != 5)
        {
            EXPECT_EQ(initial.channels[index], published.channels[index]);
        }
    }
}

TEST(HslMixerWidgetTest, ClassicSliderGesturePublishesOneAdjustmentTransaction)
{
    HslMixerWidget widget;
    widget.resize(300, 500);
    widget.show();
    QApplication::processEvents();
    QSlider* hue = widget.findChild<QSlider*>(QStringLiteral("hslHueSlider"));
    ASSERT_NE(nullptr, hue);
    int startedCount = 0;
    int finishedCount = 0;
    QObject::connect(&widget, &HslMixerWidget::adjustmentStarted, [&startedCount] { ++startedCount; });
    QObject::connect(&widget, &HslMixerWidget::adjustmentFinished, [&finishedCount] { ++finishedCount; });
    const QPoint trackPosition((hue->width() * 3) / 4, hue->height() / 2);

    sendMouseEvent(*hue, QEvent::MouseButtonPress, trackPosition, Qt::LeftButton, Qt::LeftButton);
    sendMouseEvent(*hue, QEvent::MouseButtonRelease, trackPosition, Qt::LeftButton, Qt::NoButton);

    EXPECT_EQ(1, startedCount);
    EXPECT_EQ(1, finishedCount);
    EXPECT_GT(widget.hsl().channels[0].hue, 0.0F);
}

TEST(HslMixerWidgetTest, ChannelResetIsOneTransactionAndPreservesOtherChannels)
{
    HslMixerWidget widget;
    core::types::HslParams initial;
    initial.channels[0] = {0.2F, -0.3F, 0.4F};
    initial.channels[5] = {-0.4F, 0.5F, -0.6F};
    widget.setHsl(initial);
    int changedCount = 0;
    int startedCount = 0;
    int finishedCount = 0;
    QObject::connect(
        &widget, &HslMixerWidget::hslChanged, [&changedCount](const core::types::HslParams&) { ++changedCount; });
    QObject::connect(&widget, &HslMixerWidget::adjustmentStarted, [&startedCount] { ++startedCount; });
    QObject::connect(&widget, &HslMixerWidget::adjustmentFinished, [&finishedCount] { ++finishedCount; });

    widget.findChild<QToolButton*>(QStringLiteral("hslResetChannelButton"))->click();

    EXPECT_EQ(core::types::HslChannelAdjustment{}, widget.hsl().channels[0]);
    EXPECT_EQ(initial.channels[5], widget.hsl().channels[5]);
    EXPECT_EQ(1, changedCount);
    EXPECT_EQ(1, startedCount);
    EXPECT_EQ(1, finishedCount);
}

TEST(HslMixerWidgetTest, HslResetIsOneTransactionAndClearsEveryChannel)
{
    HslMixerWidget widget;
    core::types::HslParams initial;
    initial.channels[0].hue = 0.2F;
    initial.channels[7].saturation = -0.7F;
    widget.setHsl(initial);
    int startedCount = 0;
    int finishedCount = 0;
    QObject::connect(&widget, &HslMixerWidget::adjustmentStarted, [&startedCount] { ++startedCount; });
    QObject::connect(&widget, &HslMixerWidget::adjustmentFinished, [&finishedCount] { ++finishedCount; });

    widget.findChild<QToolButton*>(QStringLiteral("hslResetAllButton"))->click();

    EXPECT_EQ(core::types::HslParams{}, widget.hsl());
    EXPECT_EQ(1, startedCount);
    EXPECT_EQ(1, finishedCount);
}

TEST(HslMixerWidgetTest, KeyboardNavigationSelectsBoundaryChannelsWithoutEditing)
{
    HslMixerWidget widget;
    QToolButton* red = widget.findChild<QToolButton*>(QStringLiteral("hslRedButton"));
    ASSERT_NE(nullptr, red);
    int changedCount = 0;
    QObject::connect(
        &widget, &HslMixerWidget::hslChanged, [&changedCount](const core::types::HslParams&) { ++changedCount; });
    QKeyEvent endEvent(QEvent::KeyPress, Qt::Key_End, Qt::NoModifier);

    QApplication::sendEvent(red, &endEvent);

    EXPECT_EQ(7, widget.selectedChannelIndex());
    EXPECT_TRUE(widget.findChild<QToolButton*>(QStringLiteral("hslMagentaButton"))->isChecked());
    EXPECT_EQ(0, changedCount);
}

TEST(HslMixerWidgetTest, KeepsHslParametersOnClassicAbsoluteSliders)
{
    HslMixerWidget widget;
    const QList<AdjustmentParameterControl*> controls = widget.findChildren<AdjustmentParameterControl*>();

    ASSERT_EQ(3, controls.size());
    for (const AdjustmentParameterControl* control : controls)
    {
        EXPECT_EQ(AdjustmentControlStyle::Classic, control->controlStyle());
    }
    for (const QString& objectName : {QStringLiteral("hslHueValueSpinBox"),
                                      QStringLiteral("hslSaturationValueSpinBox"),
                                      QStringLiteral("hslLuminanceValueSpinBox")})
    {
        QDoubleSpinBox* value = widget.findChild<QDoubleSpinBox*>(objectName);
        ASSERT_NE(nullptr, value);
        EXPECT_DOUBLE_EQ(-100.0, value->minimum());
        EXPECT_DOUBLE_EQ(100.0, value->maximum());
        EXPECT_TRUE(value->suffix().isEmpty());
    }
    EXPECT_TRUE(widget.findChild<AdjustmentParameterControl*>(QStringLiteral("hslHueAdjustmentControl"))
                    ->toolTip()
                    .contains(QStringLiteral("30")));
}

}  // namespace
}  // namespace flexraw::ui::editor
