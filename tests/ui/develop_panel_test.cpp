#include <QApplication>
#include <QComboBox>
#include <QGroupBox>
#include <QObject>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
#include <QToolButton>

#include <gtest/gtest.h>

#include "adjustment_control.h"
#include "develop_panel.h"

namespace flexraw::ui::editor
{
namespace
{

TEST(DevelopPanelTest, EmitsParamsChangedForSliderInput)
{
    DevelopPanel panel;
    core::types::DevelopParams emittedParams;
    int signalCount = 0;
    QObject::connect(
        &panel, &DevelopPanel::paramsChanged, [&emittedParams, &signalCount](const core::types::DevelopParams& params) {
            emittedParams = params;
            ++signalCount;
        });

    QSlider* exposureSlider = panel.findChild<QSlider*>(QStringLiteral("exposureSlider"));
    ASSERT_NE(exposureSlider, nullptr);
    exposureSlider->setValue(150);

    EXPECT_EQ(signalCount, 1);
    EXPECT_FLOAT_EQ(emittedParams.exposureEv, 1.5F);
    EXPECT_FLOAT_EQ(panel.params().exposureEv, 1.5F);
}

TEST(DevelopPanelTest, StyleSwitchPreservesParamsWithoutCreatingHistoryEvent)
{
    DevelopPanel panel;
    core::types::DevelopParams params;
    params.exposureEv = 0.35F;
    panel.setParams(params);
    int paramsChangedCount = 0;
    int adjustmentStartedCount = 0;
    int adjustmentFinishedCount = 0;
    QObject::connect(&panel, &DevelopPanel::paramsChanged, [&paramsChangedCount](const core::types::DevelopParams&) {
        ++paramsChangedCount;
    });
    QObject::connect(&panel, &DevelopPanel::adjustmentStarted, [&adjustmentStartedCount] { ++adjustmentStartedCount; });
    QObject::connect(
        &panel, &DevelopPanel::adjustmentFinished, [&adjustmentFinishedCount] { ++adjustmentFinishedCount; });

    panel.setAdjustmentControlStyle(AdjustmentControlStyle::Relative);

    EXPECT_EQ(AdjustmentControlStyle::Relative, panel.adjustmentControlStyle());
    EXPECT_FLOAT_EQ(0.35F, panel.params().exposureEv);
    EXPECT_EQ(0, paramsChangedCount);
    EXPECT_EQ(0, adjustmentStartedCount);
    EXPECT_EQ(0, adjustmentFinishedCount);
    const QList<AdjustmentParameterControl*> controls = panel.findChildren<AdjustmentParameterControl*>();
    ASSERT_EQ(26, controls.size());
    for (const AdjustmentParameterControl* control : controls)
    {
        EXPECT_EQ(AdjustmentControlStyle::Relative, control->controlStyle());
    }
}

TEST(DevelopPanelTest, UsesFullWidthCommonRowsForLightSliders)
{
    DevelopPanel panel;
    panel.resize(320, 900);
    panel.show();
    QApplication::processEvents();
    auto* exposureControl = panel.findChild<AdjustmentParameterControl*>(QStringLiteral("exposureAdjustmentControl"));
    auto* contrastControl = panel.findChild<AdjustmentParameterControl*>(QStringLiteral("contrastAdjustmentControl"));
    ASSERT_NE(nullptr, exposureControl);
    ASSERT_NE(nullptr, contrastControl);

    EXPECT_EQ(200, panel.minimumWidth());
    EXPECT_EQ(320, panel.maximumWidth());
    EXPECT_EQ(QSizePolicy::Expanding, exposureControl->sizePolicy().horizontalPolicy());
    EXPECT_EQ(QSizePolicy::Expanding, contrastControl->sizePolicy().horizontalPolicy());
    EXPECT_EQ(exposureControl->width(), contrastControl->width());
}

TEST(DevelopPanelTest, AppliesDehazeFromSlider)
{
    DevelopPanel panel;
    QSlider* dehazeSlider = panel.findChild<QSlider*>(QStringLiteral("dehazeSlider"));
    ASSERT_NE(dehazeSlider, nullptr);

    dehazeSlider->setValue(40);

    EXPECT_FLOAT_EQ(0.4F, panel.params().dehaze);
}

TEST(DevelopPanelTest, LimitsNoiseReductionSlidersToPositiveValues)
{
    DevelopPanel panel;
    QSlider* luminanceSlider = panel.findChild<QSlider*>(QStringLiteral("luminanceNoiseReductionSlider"));
    QSlider* colorSlider = panel.findChild<QSlider*>(QStringLiteral("colorNoiseReductionSlider"));
    ASSERT_NE(luminanceSlider, nullptr);
    ASSERT_NE(colorSlider, nullptr);

    luminanceSlider->setValue(35);
    colorSlider->setValue(60);

    EXPECT_EQ(0, luminanceSlider->minimum());
    EXPECT_EQ(0, colorSlider->minimum());
    EXPECT_FLOAT_EQ(0.35F, panel.params().luminanceNoiseReduction);
    EXPECT_FLOAT_EQ(0.6F, panel.params().colorNoiseReduction);
}

TEST(DevelopPanelTest, AppliesToneCurveFromSlider)
{
    DevelopPanel panel;
    QSlider* lightsSlider = panel.findChild<QSlider*>(QStringLiteral("toneCurveLightsSlider"));
    ASSERT_NE(lightsSlider, nullptr);

    lightsSlider->setValue(45);

    EXPECT_FLOAT_EQ(0.45F, panel.params().toneCurveLights);
}

TEST(DevelopPanelTest, AppliesPointCurveFromSlider)
{
    DevelopPanel panel;
    QSlider* midpointSlider = panel.findChild<QSlider*>(QStringLiteral("pointCurveMidtonesSlider"));
    ASSERT_NE(midpointSlider, nullptr);

    midpointSlider->setValue(-35);

    EXPECT_FLOAT_EQ(-0.35F, panel.params().pointCurveMidtones);
}

TEST(DevelopPanelTest, ScrollsAndCollapsesAdjustmentSections)
{
    DevelopPanel panel;
    QScrollArea* scrollArea = panel.findChild<QScrollArea*>(QStringLiteral("adjustmentsScrollArea"));
    QGroupBox* colorGroup = panel.findChild<QGroupBox*>(QStringLiteral("colorGroup"));
    QToolButton* colorToggle = nullptr;
    for (QToolButton* button : panel.findChildren<QToolButton*>())
    {
        if (button->text() == QStringLiteral("Color"))
        {
            colorToggle = button;
            break;
        }
    }

    ASSERT_NE(scrollArea, nullptr);
    ASSERT_NE(colorGroup, nullptr);
    ASSERT_NE(colorToggle, nullptr);
    EXPECT_EQ(Qt::ScrollBarAlwaysOff, scrollArea->horizontalScrollBarPolicy());
    EXPECT_EQ(Qt::ScrollBarAsNeeded, scrollArea->verticalScrollBarPolicy());
    EXPECT_EQ(QSizePolicy::Ignored, scrollArea->sizePolicy().verticalPolicy());
    EXPECT_TRUE(colorGroup->isHidden());

    colorToggle->click();

    EXPECT_FALSE(colorGroup->isHidden());
}

TEST(DevelopPanelTest, ResetRestoresAllDevelopParams)
{
    DevelopPanel panel;
    core::types::DevelopParams params;
    params.contrast = 0.6F;
    params.saturation = -0.5F;
    panel.setParams(params);

    panel.reset();

    EXPECT_FLOAT_EQ(panel.params().contrast, 0.0F);
    EXPECT_FLOAT_EQ(panel.params().saturation, 0.0F);
}

TEST(DevelopPanelTest, AppliesSharpeningMaskingFromSlider)
{
    DevelopPanel panel;
    QSlider* maskingSlider = panel.findChild<QSlider*>(QStringLiteral("sharpeningMaskingSlider"));
    ASSERT_NE(maskingSlider, nullptr);

    maskingSlider->setValue(65);

    EXPECT_EQ(0, maskingSlider->minimum());
    EXPECT_EQ(100, maskingSlider->maximum());
    EXPECT_FLOAT_EQ(0.65F, panel.params().sharpeningMasking);
}

TEST(DevelopPanelTest, EnablesCustomWhiteBalanceControlsForManualValues)
{
    DevelopPanel panel;
    QComboBox* modeCombo = panel.findChild<QComboBox*>(QStringLiteral("whiteBalanceModeCombo"));
    QSpinBox* temperatureSpinBox = panel.findChild<QSpinBox*>(QStringLiteral("whiteBalanceTemperatureSpinBox"));
    QSlider* tintSlider = panel.findChild<QSlider*>(QStringLiteral("whiteBalanceTintSlider"));
    ASSERT_NE(modeCombo, nullptr);
    ASSERT_NE(temperatureSpinBox, nullptr);
    ASSERT_NE(tintSlider, nullptr);

    EXPECT_FALSE(temperatureSpinBox->isEnabled());
    modeCombo->setCurrentIndex(1);
    temperatureSpinBox->setValue(5200);
    tintSlider->setValue(25);

    EXPECT_EQ(core::types::WhiteBalanceMode::Custom, panel.params().whiteBalanceMode);
    EXPECT_FLOAT_EQ(5200.0F, panel.params().whiteBalanceTemperatureKelvin);
    EXPECT_FLOAT_EQ(0.25F, panel.params().whiteBalanceTint);
    EXPECT_TRUE(temperatureSpinBox->isEnabled());
}

}  // namespace
}  // namespace flexraw::ui::editor
