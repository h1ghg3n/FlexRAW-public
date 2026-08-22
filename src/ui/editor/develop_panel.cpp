#include "develop_panel.h"

#include <cmath>

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>

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
    setMinimumWidth(260);
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
    addWhiteBalanceControls(colorLayout);
    addNormalizedControl(
        colorLayout, tr("Vibrance"), QStringLiteral("vibranceSlider"), &core::types::DevelopParams::vibrance);
    addNormalizedControl(
        colorLayout, tr("Saturation"), QStringLiteral("saturationSlider"), &core::types::DevelopParams::saturation);
    contentLayout->addWidget(new CollapsibleSection(tr("Color"), colorGroup, false, contentWidget));

    auto* presenceGroup = new QGroupBox(contentWidget);
    presenceGroup->setObjectName(QStringLiteral("presenceGroup"));
    auto* presenceLayout = new QFormLayout(presenceGroup);
    addNormalizedControl(
        presenceLayout, tr("Clarity"), QStringLiteral("claritySlider"), &core::types::DevelopParams::clarity);
    addNormalizedControl(
        presenceLayout, tr("Dehaze"), QStringLiteral("dehazeSlider"), &core::types::DevelopParams::dehaze);
    contentLayout->addWidget(new CollapsibleSection(tr("Presence"), presenceGroup, false, contentWidget));

    auto* toneCurveGroup = new QGroupBox(contentWidget);
    toneCurveGroup->setObjectName(QStringLiteral("toneCurveGroup"));
    auto* toneCurveLayout = new QFormLayout(toneCurveGroup);
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

// 목적: 노출 control을 생성하고 DevelopParams::exposureEv에 연결
// 입력: layout: control을 추가할 form layout
// 출력: 없음
void DevelopPanel::addExposureControl(QFormLayout* layout)
{
    m_exposureSlider = new QSlider(Qt::Horizontal, this);
    m_exposureSlider->setObjectName(QStringLiteral("exposureSlider"));
    m_exposureSlider->setRange(-50, 50);
    m_exposureSpinBox = new QDoubleSpinBox(this);
    m_exposureSpinBox->setRange(-5.0, 5.0);
    m_exposureSpinBox->setDecimals(1);
    m_exposureSpinBox->setSingleStep(0.1);
    m_exposureSpinBox->setSuffix(tr(" EV"));

    auto* controlLayout = new QHBoxLayout();
    controlLayout->setContentsMargins({});
    controlLayout->addWidget(m_exposureSlider);
    controlLayout->addWidget(m_exposureSpinBox);
    layout->addRow(tr("Exposure"), controlLayout);

    connect(m_exposureSlider, &QSlider::valueChanged, this, [this](int value) {
        m_params.exposureEv = static_cast<float>(value) / 10.0F;
        QSignalBlocker blocker(m_exposureSpinBox);
        m_exposureSpinBox->setValue(m_params.exposureEv);
        emit paramsChanged(m_params);
    });
    connect(m_exposureSpinBox, &QDoubleSpinBox::valueChanged, this, [this](double value) {
        beginAdjustment();
        m_exposureSlider->setValue(static_cast<int>(std::lround(value * 10.0)));
    });
    connect(m_exposureSlider, &QSlider::sliderPressed, this, &DevelopPanel::beginAdjustment);
    connect(m_exposureSlider, &QSlider::sliderReleased, this, &DevelopPanel::finishAdjustment);
    connect(m_exposureSpinBox, &QDoubleSpinBox::editingFinished, this, &DevelopPanel::finishAdjustment);
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

    m_whiteBalanceTintSlider = new QSlider(Qt::Horizontal, this);
    m_whiteBalanceTintSlider->setObjectName(QStringLiteral("whiteBalanceTintSlider"));
    m_whiteBalanceTintSlider->setRange(-100, 100);
    m_whiteBalanceTintSpinBox = new QSpinBox(this);
    m_whiteBalanceTintSpinBox->setRange(-100, 100);
    auto* tintLayout = new QHBoxLayout();
    tintLayout->setContentsMargins({});
    tintLayout->addWidget(m_whiteBalanceTintSlider);
    tintLayout->addWidget(m_whiteBalanceTintSpinBox);
    layout->addRow(tr("Tint"), tintLayout);

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
    connect(m_whiteBalanceTintSlider, &QSlider::valueChanged, this, [this](int value) {
        m_params.whiteBalanceMode = core::types::WhiteBalanceMode::Custom;
        m_params.whiteBalanceTint = static_cast<float>(value) / 100.0F;
        QSignalBlocker blocker(m_whiteBalanceTintSpinBox);
        m_whiteBalanceTintSpinBox->setValue(value);
        updateWhiteBalanceControls();
        emit paramsChanged(m_params);
    });
    connect(m_whiteBalanceTintSpinBox, &QSpinBox::valueChanged, this, [this](int value) {
        beginAdjustment();
        m_whiteBalanceTintSlider->setValue(value);
    });
    connect(m_whiteBalanceTintSlider, &QSlider::sliderPressed, this, &DevelopPanel::beginAdjustment);
    connect(m_whiteBalanceTintSlider, &QSlider::sliderReleased, this, &DevelopPanel::finishAdjustment);
    connect(m_whiteBalanceTintSpinBox, &QSpinBox::editingFinished, this, &DevelopPanel::finishAdjustment);

    updateWhiteBalanceControls();
}

// 목적: sharpening radius control을 생성하고 DevelopParams::sharpeningRadius에 연결
// 입력: layout: control을 추가할 form layout
// 출력: 없음
void DevelopPanel::addSharpeningRadiusControl(QFormLayout* layout)
{
    m_sharpeningRadiusSlider = new QSlider(Qt::Horizontal, this);
    m_sharpeningRadiusSlider->setObjectName(QStringLiteral("sharpeningRadiusSlider"));
    m_sharpeningRadiusSlider->setRange(1, 3);
    m_sharpeningRadiusSpinBox = new QSpinBox(this);
    m_sharpeningRadiusSpinBox->setRange(1, 3);
    m_sharpeningRadiusSpinBox->setSuffix(tr(" px"));

    auto* controlLayout = new QHBoxLayout();
    controlLayout->setContentsMargins({});
    controlLayout->addWidget(m_sharpeningRadiusSlider);
    controlLayout->addWidget(m_sharpeningRadiusSpinBox);
    layout->addRow(tr("Radius"), controlLayout);

    connect(m_sharpeningRadiusSlider, &QSlider::valueChanged, this, [this](int value) {
        m_params.sharpeningRadius = static_cast<float>(value);
        QSignalBlocker blocker(m_sharpeningRadiusSpinBox);
        m_sharpeningRadiusSpinBox->setValue(value);
        emit paramsChanged(m_params);
    });
    connect(m_sharpeningRadiusSpinBox, &QSpinBox::valueChanged, this, [this](int value) {
        beginAdjustment();
        m_sharpeningRadiusSlider->setValue(value);
    });
    connect(m_sharpeningRadiusSlider, &QSlider::sliderPressed, this, &DevelopPanel::beginAdjustment);
    connect(m_sharpeningRadiusSlider, &QSlider::sliderReleased, this, &DevelopPanel::finishAdjustment);
    connect(m_sharpeningRadiusSpinBox, &QSpinBox::editingFinished, this, &DevelopPanel::finishAdjustment);
}

// 목적: sharpening masking control을 생성하고 DevelopParams::sharpeningMasking에 연결
// 입력: layout: control을 추가할 form layout
// 출력: 없음
void DevelopPanel::addSharpeningMaskingControl(QFormLayout* layout)
{
    m_sharpeningMaskingSlider = new QSlider(Qt::Horizontal, this);
    m_sharpeningMaskingSlider->setObjectName(QStringLiteral("sharpeningMaskingSlider"));
    m_sharpeningMaskingSlider->setRange(0, 100);
    m_sharpeningMaskingSpinBox = new QSpinBox(this);
    m_sharpeningMaskingSpinBox->setRange(0, 100);
    m_sharpeningMaskingSpinBox->setSuffix(tr("%"));

    auto* controlLayout = new QHBoxLayout();
    controlLayout->setContentsMargins({});
    controlLayout->addWidget(m_sharpeningMaskingSlider);
    controlLayout->addWidget(m_sharpeningMaskingSpinBox);
    layout->addRow(tr("Masking"), controlLayout);

    connect(m_sharpeningMaskingSlider, &QSlider::valueChanged, this, [this](int value) {
        m_params.sharpeningMasking = static_cast<float>(value) / 100.0F;
        QSignalBlocker blocker(m_sharpeningMaskingSpinBox);
        m_sharpeningMaskingSpinBox->setValue(value);
        emit paramsChanged(m_params);
    });
    connect(m_sharpeningMaskingSpinBox, &QSpinBox::valueChanged, this, [this](int value) {
        beginAdjustment();
        m_sharpeningMaskingSlider->setValue(value);
    });
    connect(m_sharpeningMaskingSlider, &QSlider::sliderPressed, this, &DevelopPanel::beginAdjustment);
    connect(m_sharpeningMaskingSlider, &QSlider::sliderReleased, this, &DevelopPanel::finishAdjustment);
    connect(m_sharpeningMaskingSpinBox, &QSpinBox::editingFinished, this, &DevelopPanel::finishAdjustment);
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
    auto* slider = new QSlider(Qt::Horizontal, this);
    slider->setObjectName(objectName);
    slider->setRange(minimumValue, 100);
    auto* spinBox = new QSpinBox(this);
    spinBox->setRange(minimumValue, 100);
    spinBox->setSuffix(tr("%"));

    auto* controlLayout = new QHBoxLayout();
    controlLayout->setContentsMargins({});
    controlLayout->addWidget(slider);
    controlLayout->addWidget(spinBox);
    layout->addRow(label, controlLayout);
    m_normalizedControls.push_back({slider, spinBox, parameter});

    connect(slider, &QSlider::valueChanged, this, [this, spinBox, parameter](int value) {
        m_params.*parameter = static_cast<float>(value) / 100.0F;
        QSignalBlocker blocker(spinBox);
        spinBox->setValue(value);
        emit paramsChanged(m_params);
    });
    connect(spinBox, &QSpinBox::valueChanged, this, [this, slider](int value) {
        beginAdjustment();
        slider->setValue(value);
    });
    connect(slider, &QSlider::sliderPressed, this, &DevelopPanel::beginAdjustment);
    connect(slider, &QSlider::sliderReleased, this, &DevelopPanel::finishAdjustment);
    connect(spinBox, &QSpinBox::editingFinished, this, &DevelopPanel::finishAdjustment);
}

// 목적: 현재 DevelopParams 값을 모든 control에 signal 없이 반영
// 입력: 없음
// 출력: 없음
void DevelopPanel::updateControls()
{
    const QSignalBlocker exposureSliderBlocker(m_exposureSlider);
    const QSignalBlocker exposureSpinBoxBlocker(m_exposureSpinBox);
    m_exposureSlider->setValue(static_cast<int>(std::lround(m_params.exposureEv * 10.0F)));
    m_exposureSpinBox->setValue(m_params.exposureEv);

    const QSignalBlocker sharpeningRadiusSliderBlocker(m_sharpeningRadiusSlider);
    const QSignalBlocker sharpeningRadiusSpinBoxBlocker(m_sharpeningRadiusSpinBox);
    const int sharpeningRadius = static_cast<int>(std::lround(m_params.sharpeningRadius));
    m_sharpeningRadiusSlider->setValue(sharpeningRadius);
    m_sharpeningRadiusSpinBox->setValue(sharpeningRadius);

    const QSignalBlocker sharpeningMaskingSliderBlocker(m_sharpeningMaskingSlider);
    const QSignalBlocker sharpeningMaskingSpinBoxBlocker(m_sharpeningMaskingSpinBox);
    const int sharpeningMasking = static_cast<int>(std::lround(m_params.sharpeningMasking * 100.0F));
    m_sharpeningMaskingSlider->setValue(sharpeningMasking);
    m_sharpeningMaskingSpinBox->setValue(sharpeningMasking);

    for (const NormalizedControl& control : m_normalizedControls)
    {
        const QSignalBlocker sliderBlocker(control.slider);
        const QSignalBlocker spinBoxBlocker(control.spinBox);
        const int value = static_cast<int>(std::lround(m_params.*(control.parameter) * 100.0F));
        control.slider->setValue(value);
        control.spinBox->setValue(value);
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
    const QSignalBlocker tintSliderBlocker(m_whiteBalanceTintSlider);
    const QSignalBlocker tintSpinBoxBlocker(m_whiteBalanceTintSpinBox);
    m_whiteBalanceModeCombo->setCurrentIndex(isCustom ? 1 : 0);
    m_whiteBalanceTemperatureSpinBox->setValue(static_cast<int>(std::lround(m_params.whiteBalanceTemperatureKelvin)));
    const int tint = static_cast<int>(std::lround(m_params.whiteBalanceTint * 100.0F));
    m_whiteBalanceTintSlider->setValue(tint);
    m_whiteBalanceTintSpinBox->setValue(tint);
    m_whiteBalanceTemperatureSpinBox->setEnabled(isCustom);
    m_whiteBalanceTintSlider->setEnabled(isCustom);
    m_whiteBalanceTintSpinBox->setEnabled(isCustom);
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
