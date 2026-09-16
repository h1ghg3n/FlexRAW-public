#include "develop_panel.h"

#include <cmath>

#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>

#include "adjustment_control.h"
#include "histogram_widget.h"

namespace flexraw::ui::editor
{
namespace
{

class CollapsibleSection final : public QWidget
{
public:
    // 목적: 제목 button과 접을 수 있는 content widget으로 editor section 초기화
    // 입력: title: section 제목, content: 접기/펼치기 대상 widget, expanded: 초기 펼침 상태, parent: Qt 부모 widget
    // 출력: 초기화된 CollapsibleSection 객체
    CollapsibleSection(const QString& title, QWidget* content, bool expanded, QWidget* parent = nullptr)
        : QWidget(parent), m_content(content)
    {
        m_toggleButton = new QToolButton(this);
        m_toggleButton->setText(title);
        m_toggleButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        m_toggleButton->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
        m_toggleButton->setCheckable(true);
        m_toggleButton->setChecked(expanded);
        m_toggleButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        m_content->setParent(this);
        m_content->setVisible(expanded);

        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins({});
        layout->setSpacing(2);
        layout->addWidget(m_toggleButton);
        layout->addWidget(m_content);
        connect(m_toggleButton, &QToolButton::toggled, this, [this](bool isExpanded) {
            m_content->setVisible(isExpanded);
            m_toggleButton->setArrowType(isExpanded ? Qt::DownArrow : Qt::RightArrow);
        });
    }

private:
    QToolButton* m_toggleButton{nullptr};
    QWidget* m_content{nullptr};
};

}  // namespace

// 목적: 기본 현상 파라미터를 조작하는 editor panel 초기화
// 입력: parent: Qt 부모 widget
// 출력: 초기화된 DevelopPanel 객체
DevelopPanel::DevelopPanel(QWidget* parent) : QWidget(parent)
{
    setMinimumWidth(200);
    setMaximumWidth(320);

    auto* layout = new QVBoxLayout(this);
    auto* headerLayout = new QHBoxLayout();
    auto* titleLabel = new QLabel(tr("Adjustments"), this);
    auto* resetButton = new QToolButton(this);
    resetButton->setIcon(style()->standardIcon(QStyle::SP_BrowserReload));
    resetButton->setToolTip(tr("Reset adjustments"));
    headerLayout->addWidget(titleLabel);
    headerLayout->addStretch();
    headerLayout->addWidget(resetButton);
    layout->addLayout(headerLayout);

    auto* scrollArea = new QScrollArea(this);
    scrollArea->setObjectName(QStringLiteral("adjustmentsScrollArea"));
    scrollArea->setWidgetResizable(true);
    scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scrollArea->setMinimumHeight(0);
    scrollArea->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Ignored);
    auto* contentWidget = new QWidget(scrollArea);
    auto* contentLayout = new QVBoxLayout(contentWidget);
    contentLayout->setContentsMargins({});
    scrollArea->setWidget(contentWidget);
    layout->addWidget(scrollArea);

    m_histogramWidget = new HistogramWidget(contentWidget);
    contentLayout->addWidget(m_histogramWidget);

    auto* lightGroup = new QGroupBox(contentWidget);
    lightGroup->setObjectName(QStringLiteral("lightGroup"));
    auto* lightLayout = new QFormLayout(lightGroup);
    lightLayout->setVerticalSpacing(2);
    addExposureControl(lightLayout);
    addNormalizedControl(
        lightLayout, tr("Contrast"), QStringLiteral("contrastSlider"), &core::types::DevelopParams::contrast);
    addNormalizedControl(
        lightLayout, tr("Highlights"), QStringLiteral("highlightsSlider"), &core::types::DevelopParams::highlights);
    addNormalizedControl(
        lightLayout, tr("Shadows"), QStringLiteral("shadowsSlider"), &core::types::DevelopParams::shadows);
    addNormalizedControl(
        lightLayout, tr("Whites"), QStringLiteral("whitesSlider"), &core::types::DevelopParams::whites);
    addNormalizedControl(
        lightLayout, tr("Blacks"), QStringLiteral("blacksSlider"), &core::types::DevelopParams::blacks);
    contentLayout->addWidget(new CollapsibleSection(tr("Light"), lightGroup, true, contentWidget));

    auto* colorGroup = new QGroupBox(contentWidget);
    colorGroup->setObjectName(QStringLiteral("colorGroup"));
    auto* colorLayout = new QFormLayout(colorGroup);
    colorLayout->setVerticalSpacing(2);
    addWhiteBalanceControls(colorLayout);
    addNormalizedControl(
        colorLayout, tr("Vibrance"), QStringLiteral("vibranceSlider"), &core::types::DevelopParams::vibrance);
    addNormalizedControl(
        colorLayout, tr("Saturation"), QStringLiteral("saturationSlider"), &core::types::DevelopParams::saturation);
    contentLayout->addWidget(new CollapsibleSection(tr("Color"), colorGroup, false, contentWidget));

    auto* presenceGroup = new QGroupBox(contentWidget);
    presenceGroup->setObjectName(QStringLiteral("presenceGroup"));
    auto* presenceLayout = new QFormLayout(presenceGroup);
    presenceLayout->setVerticalSpacing(2);
    addNormalizedControl(
        presenceLayout, tr("Clarity"), QStringLiteral("claritySlider"), &core::types::DevelopParams::clarity);
    addNormalizedControl(
        presenceLayout, tr("Dehaze"), QStringLiteral("dehazeSlider"), &core::types::DevelopParams::dehaze);
    contentLayout->addWidget(new CollapsibleSection(tr("Presence"), presenceGroup, false, contentWidget));

    auto* toneCurveGroup = new QGroupBox(contentWidget);
    toneCurveGroup->setObjectName(QStringLiteral("toneCurveGroup"));
    auto* toneCurveLayout = new QFormLayout(toneCurveGroup);
    toneCurveLayout->setVerticalSpacing(2);
    addNormalizedControl(toneCurveLayout,
                         tr("Shadows"),
                         QStringLiteral("toneCurveShadowsSlider"),
                         &core::types::DevelopParams::toneCurveShadows);
    addNormalizedControl(toneCurveLayout,
                         tr("Darks"),
                         QStringLiteral("toneCurveDarksSlider"),
                         &core::types::DevelopParams::toneCurveDarks);
    addNormalizedControl(toneCurveLayout,
                         tr("Lights"),
                         QStringLiteral("toneCurveLightsSlider"),
                         &core::types::DevelopParams::toneCurveLights);
    addNormalizedControl(toneCurveLayout,
                         tr("Highlights"),
                         QStringLiteral("toneCurveHighlightsSlider"),
                         &core::types::DevelopParams::toneCurveHighlights);
    contentLayout->addWidget(new CollapsibleSection(tr("Tone Curve"), toneCurveGroup, true, contentWidget));

    auto* pointCurveGroup = new QGroupBox(contentWidget);
    pointCurveGroup->setObjectName(QStringLiteral("pointCurveGroup"));
    auto* pointCurveLayout = new QFormLayout(pointCurveGroup);
    pointCurveLayout->setVerticalSpacing(2);
    addNormalizedControl(pointCurveLayout,
                         tr("Black"),
                         QStringLiteral("pointCurveBlackSlider"),
                         &core::types::DevelopParams::pointCurveBlack);
    addNormalizedControl(pointCurveLayout,
                         tr("Shadows"),
                         QStringLiteral("pointCurveShadowsSlider"),
                         &core::types::DevelopParams::pointCurveShadows);
    addNormalizedControl(pointCurveLayout,
                         tr("Midtones"),
                         QStringLiteral("pointCurveMidtonesSlider"),
                         &core::types::DevelopParams::pointCurveMidtones);
    addNormalizedControl(pointCurveLayout,
                         tr("Highlights"),
                         QStringLiteral("pointCurveHighlightsSlider"),
                         &core::types::DevelopParams::pointCurveHighlights);
    addNormalizedControl(pointCurveLayout,
                         tr("White"),
                         QStringLiteral("pointCurveWhiteSlider"),
                         &core::types::DevelopParams::pointCurveWhite);
    contentLayout->addWidget(new CollapsibleSection(tr("Point Curve"), pointCurveGroup, false, contentWidget));

    auto* detailGroup = new QGroupBox(contentWidget);
    detailGroup->setObjectName(QStringLiteral("detailGroup"));
    auto* detailLayout = new QFormLayout(detailGroup);
    detailLayout->setVerticalSpacing(2);
    addNormalizedControl(detailLayout,
                         tr("Sharpening"),
                         QStringLiteral("sharpeningSlider"),
                         &core::types::DevelopParams::sharpeningAmount);
    addSharpeningRadiusControl(detailLayout);
    addNormalizedControl(detailLayout,
                         tr("Detail"),
                         QStringLiteral("sharpeningDetailSlider"),
                         &core::types::DevelopParams::sharpeningDetail);
    addSharpeningMaskingControl(detailLayout);
    addNormalizedControl(detailLayout,
                         tr("Luminance NR"),
                         QStringLiteral("luminanceNoiseReductionSlider"),
                         &core::types::DevelopParams::luminanceNoiseReduction,
                         0);
    addNormalizedControl(detailLayout,
                         tr("Color NR"),
                         QStringLiteral("colorNoiseReductionSlider"),
                         &core::types::DevelopParams::colorNoiseReduction,
                         0);
    contentLayout->addWidget(new CollapsibleSection(tr("Detail"), detailGroup, false, contentWidget));
    contentLayout->addStretch();

    connect(resetButton, &QToolButton::clicked, this, &DevelopPanel::reset);
    updateControls();
}

// 목적: 현재 panel에 설정된 develop parameter 읽기 전용 참조 반환
// 입력: 없음
// 출력: 현재 DevelopParams 값 참조
const core::types::DevelopParams& DevelopPanel::params() const
{
    return m_params;
}

// 목적: 외부 편집 상태를 panel control과 동기화
// 입력: params: 표시하고 적용할 develop parameter 값
// 출력: paramsChanged signal 발생
void DevelopPanel::setParams(const core::types::DevelopParams& params)
{
    m_params = params;
    updateControls();
    emit paramsChanged(m_params);
}

// 목적: 모든 현상 parameter를 기본값으로 복원
// 입력: 없음
// 출력: paramsChanged signal 발생
void DevelopPanel::reset()
{
    beginAdjustment();
    setParams({});
    finishAdjustment();
}

// 목적: 현재 preview의 RGB와 휘도 histogram을 panel에 표시
// 입력: histogram: worker가 계산한 preview histogram
// 출력: histogram widget repaint 예약
void DevelopPanel::setHistogram(const core::develop::ImageHistogram& histogram)
{
    m_histogramWidget->setHistogram(histogram);
}

// 목적: 새 preview를 기다리는 동안 이전 histogram 표시를 제거
// 입력: 없음
// 출력: histogram widget repaint 예약
void DevelopPanel::clearHistogram()
{
    m_histogramWidget->clearHistogram();
}

// 목적: navigation 또는 외부 state 전환 전에 진행 중인 adjustment transaction 종료
// 입력: 없음
// 출력: 진행 중인 조작이 있으면 adjustmentFinished signal 발생
void DevelopPanel::finishActiveAdjustment()
{
    finishAdjustment();
}

// 목적: 모든 Develop parameter 의미를 유지하며 adjustment presentation 교체
// 입력: style: Classic 또는 Relative
// 출력: style 변경은 paramsChanged나 history signal을 발생시키지 않음
void DevelopPanel::setAdjustmentControlStyle(AdjustmentControlStyle style)
{
    finishAdjustment();
    m_adjustmentControlStyle = style;
    for (const ParameterControlBinding& binding : m_parameterControls)
    {
        binding.control->setControlStyle(style);
    }
}

// 목적: 현재 adjustment presentation preference 반환
// 입력: 없음
// 출력: Classic 또는 Relative style
AdjustmentControlStyle DevelopPanel::adjustmentControlStyle() const
{
    return m_adjustmentControlStyle;
}

// 목적: 공통 label/value와 Classic/Relative presentation을 가진 parameter control 생성
// 입력: layout: 추가 대상, configuration: 표시·범위·rate, parameter: 연결할 DevelopParams member,
//       activatesCustomWhiteBalance: 변경 시 Custom White Balance로 전환할지 여부
// 출력: 생성되어 layout과 parameter에 연결된 control
AdjustmentParameterControl* DevelopPanel::addParameterControl(QFormLayout* layout,
                                                              const AdjustmentParameterConfiguration& configuration,
                                                              float core::types::DevelopParams::* parameter,
                                                              bool activatesCustomWhiteBalance)
{
    auto* control = new AdjustmentParameterControl(configuration, this);
    control->setControlStyle(m_adjustmentControlStyle);
    layout->addRow(control);
    m_parameterControls.push_back({control, parameter});
    connect(control,
            &AdjustmentParameterControl::valueChanged,
            this,
            [this, parameter, activatesCustomWhiteBalance](double value) {
                m_params.*parameter = static_cast<float>(value);
                if (activatesCustomWhiteBalance)
                {
                    m_params.whiteBalanceMode = core::types::WhiteBalanceMode::Custom;
                    updateWhiteBalanceControls();
                }
                emit paramsChanged(m_params);
            });
    connect(control, &AdjustmentParameterControl::adjustmentStarted, this, &DevelopPanel::beginAdjustment);
    connect(control, &AdjustmentParameterControl::adjustmentFinished, this, &DevelopPanel::finishAdjustment);
    return control;
}

// 목적: 노출 control을 생성하고 DevelopParams::exposureEv에 연결
// 입력: layout: control을 추가할 form layout
// 출력: 없음
void DevelopPanel::addExposureControl(QFormLayout* layout)
{
    AdjustmentParameterConfiguration configuration;
    configuration.label = tr("Exposure");
    configuration.sliderObjectName = QStringLiteral("exposureSlider");
    configuration.suffix = tr(" EV");
    configuration.minimum = -5.0;
    configuration.maximum = 5.0;
    configuration.sliderScale = 100.0;
    configuration.displayScale = 1.0;
    configuration.singleStep = 0.1;
    configuration.maximumRate = 0.8;
    configuration.displayDecimals = 2;
    addParameterControl(layout, configuration, &core::types::DevelopParams::exposureEv);
}

// 목적: as-shot 또는 custom white balance control을 생성하고 DevelopParams에 연결
// 입력: layout: control을 추가할 form layout
// 출력: 없음
void DevelopPanel::addWhiteBalanceControls(QFormLayout* layout)
{
    m_whiteBalanceModeCombo = new QComboBox(this);
    m_whiteBalanceModeCombo->setObjectName(QStringLiteral("whiteBalanceModeCombo"));
    m_whiteBalanceModeCombo->addItem(tr("As Shot"));
    m_whiteBalanceModeCombo->addItem(tr("Custom"));
    layout->addRow(tr("White Balance"), m_whiteBalanceModeCombo);

    m_whiteBalanceTemperatureSpinBox = new QSpinBox(this);
    m_whiteBalanceTemperatureSpinBox->setObjectName(QStringLiteral("whiteBalanceTemperatureSpinBox"));
    m_whiteBalanceTemperatureSpinBox->setRange(2000, 50000);
    m_whiteBalanceTemperatureSpinBox->setSingleStep(100);
    m_whiteBalanceTemperatureSpinBox->setSuffix(tr(" K"));
    layout->addRow(tr("Temperature"), m_whiteBalanceTemperatureSpinBox);

    AdjustmentParameterConfiguration tintConfiguration;
    tintConfiguration.label = tr("Tint");
    tintConfiguration.sliderObjectName = QStringLiteral("whiteBalanceTintSlider");
    tintConfiguration.minimum = -1.0;
    tintConfiguration.maximum = 1.0;
    tintConfiguration.sliderScale = 100.0;
    tintConfiguration.displayScale = 100.0;
    tintConfiguration.singleStep = 0.01;
    tintConfiguration.maximumRate = 0.1;
    tintConfiguration.displayDecimals = 0;
    m_whiteBalanceTintControl =
        addParameterControl(layout, tintConfiguration, &core::types::DevelopParams::whiteBalanceTint, true);

    connect(m_whiteBalanceModeCombo, &QComboBox::currentIndexChanged, this, [this](int index) {
        beginAdjustment();
        m_params.whiteBalanceMode =
            index == 0 ? core::types::WhiteBalanceMode::AsShot : core::types::WhiteBalanceMode::Custom;
        updateWhiteBalanceControls();
        emit paramsChanged(m_params);
        finishAdjustment();
    });
    connect(m_whiteBalanceTemperatureSpinBox, &QSpinBox::valueChanged, this, [this](int value) {
        beginAdjustment();
        m_params.whiteBalanceMode = core::types::WhiteBalanceMode::Custom;
        m_params.whiteBalanceTemperatureKelvin = static_cast<float>(value);
        updateWhiteBalanceControls();
        emit paramsChanged(m_params);
    });
    connect(m_whiteBalanceTemperatureSpinBox, &QSpinBox::editingFinished, this, &DevelopPanel::finishAdjustment);
    updateWhiteBalanceControls();
}

// 목적: sharpening radius control을 생성하고 DevelopParams::sharpeningRadius에 연결
// 입력: layout: control을 추가할 form layout
// 출력: 없음
void DevelopPanel::addSharpeningRadiusControl(QFormLayout* layout)
{
    AdjustmentParameterConfiguration configuration;
    configuration.label = tr("Radius");
    configuration.sliderObjectName = QStringLiteral("sharpeningRadiusSlider");
    configuration.suffix = tr(" px");
    configuration.minimum = 1.0;
    configuration.maximum = 3.0;
    configuration.sliderScale = 1.0;
    configuration.displayScale = 1.0;
    configuration.singleStep = 1.0;
    configuration.maximumRate = 1.0;
    configuration.displayDecimals = 0;
    configuration.showPlusSign = false;
    addParameterControl(layout, configuration, &core::types::DevelopParams::sharpeningRadius);
}

// 목적: sharpening masking control을 생성하고 DevelopParams::sharpeningMasking에 연결
// 입력: layout: control을 추가할 form layout
// 출력: 없음
void DevelopPanel::addSharpeningMaskingControl(QFormLayout* layout)
{
    AdjustmentParameterConfiguration configuration;
    configuration.label = tr("Masking");
    configuration.sliderObjectName = QStringLiteral("sharpeningMaskingSlider");
    configuration.suffix = tr("%");
    configuration.minimum = 0.0;
    configuration.maximum = 1.0;
    configuration.sliderScale = 100.0;
    configuration.displayScale = 100.0;
    configuration.singleStep = 0.01;
    configuration.maximumRate = 0.1;
    configuration.displayDecimals = 0;
    configuration.showPlusSign = false;
    addParameterControl(layout, configuration, &core::types::DevelopParams::sharpeningMasking);
}

// 목적: 지정된 정수 범위의 표준 develop control을 생성하고 parameter에 연결
// 입력: layout: 추가 대상 layout, label: 표시명, objectName: test용 이름, parameter: 연결할 값, minimumValue: 최솟값
// 출력: 없음
void DevelopPanel::addNormalizedControl(QFormLayout* layout,
                                        const QString& label,
                                        const QString& objectName,
                                        float core::types::DevelopParams::* parameter,
                                        int minimumValue)
{
    AdjustmentParameterConfiguration configuration;
    configuration.label = label;
    configuration.sliderObjectName = objectName;
    configuration.suffix = tr("%");
    configuration.minimum = static_cast<double>(minimumValue) / 100.0;
    configuration.maximum = 1.0;
    configuration.sliderScale = 100.0;
    configuration.displayScale = 100.0;
    configuration.singleStep = 0.01;
    configuration.maximumRate = 0.1;
    configuration.displayDecimals = 0;
    configuration.showPlusSign = minimumValue < 0;
    addParameterControl(layout, configuration, parameter);
}

// 목적: 현재 DevelopParams 값을 모든 control에 signal 없이 반영
// 입력: 없음
// 출력: 없음
void DevelopPanel::updateControls()
{
    for (const ParameterControlBinding& binding : m_parameterControls)
    {
        binding.control->setValue(m_params.*(binding.parameter));
    }

    updateWhiteBalanceControls();
}

// 목적: 현재 white balance mode에 맞춰 관련 control의 값과 활성 상태를 반영
// 입력: 없음
// 출력: 없음
void DevelopPanel::updateWhiteBalanceControls()
{
    const bool isCustom = m_params.whiteBalanceMode == core::types::WhiteBalanceMode::Custom;
    const QSignalBlocker modeBlocker(m_whiteBalanceModeCombo);
    const QSignalBlocker temperatureBlocker(m_whiteBalanceTemperatureSpinBox);
    m_whiteBalanceModeCombo->setCurrentIndex(isCustom ? 1 : 0);
    m_whiteBalanceTemperatureSpinBox->setValue(static_cast<int>(std::lround(m_params.whiteBalanceTemperatureKelvin)));
    m_whiteBalanceTemperatureSpinBox->setEnabled(isCustom);
    m_whiteBalanceTintControl->setEnabled(isCustom);
}

// 목적: history를 위한 조작 transaction을 아직 시작하지 않았으면 시작
// 입력: 없음
// 출력: adjustmentStarted signal 발생 가능
void DevelopPanel::beginAdjustment()
{
    if (m_adjustmentInProgress)
    {
        return;
    }

    m_adjustmentInProgress = true;
    emit adjustmentStarted();
}

// 목적: history를 위한 현재 조작 transaction을 종료
// 입력: 없음
// 출력: adjustmentFinished signal 발생 가능
void DevelopPanel::finishAdjustment()
{
    if (!m_adjustmentInProgress)
    {
        return;
    }

    m_adjustmentInProgress = false;
    emit adjustmentFinished();
}

}  // namespace flexraw::ui::editor
