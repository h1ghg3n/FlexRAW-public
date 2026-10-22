#include "hsl_mixer_widget.h"

#include <algorithm>

#include <QButtonGroup>
#include <QColor>
#include <QEvent>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QPixmap>
#include <QResizeEvent>
#include <QSize>
#include <QToolButton>
#include <QVBoxLayout>

#include "adjustment_control.h"

namespace flexraw::ui::editor
{
namespace
{

constexpr int WideChannelGridMinimum = 260;

const std::array<QColor, 8> ChannelColors = {
    QColor(QStringLiteral("#d94a4a")),
    QColor(QStringLiteral("#e88632")),
    QColor(QStringLiteral("#d5b92b")),
    QColor(QStringLiteral("#4cae5a")),
    QColor(QStringLiteral("#39aeb5")),
    QColor(QStringLiteral("#4779d1")),
    QColor(QStringLiteral("#8758c7")),
    QColor(QStringLiteral("#c94f9f")),
};

}  // namespace

// 목적: 8개 HSL channel과 선택 channel의 절대 보정 control 초기화
// 입력: parent: Qt 부모 widget
// 출력: Red channel이 선택된 HslMixerWidget 객체
HslMixerWidget::HslMixerWidget(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("hslMixer"));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins({});
    layout->setSpacing(4);

    auto* channelWidget = new QWidget(this);
    channelWidget->setObjectName(QStringLiteral("hslChannelSelector"));
    m_channelLayout = new QGridLayout(channelWidget);
    m_channelLayout->setContentsMargins({});
    m_channelLayout->setHorizontalSpacing(4);
    m_channelLayout->setVerticalSpacing(3);
    m_channelGroup = new QButtonGroup(this);
    m_channelGroup->setExclusive(true);

    const std::array<QString, 8> channelLabels = {
        tr("Red"), tr("Orange"), tr("Yellow"), tr("Green"), tr("Aqua"), tr("Blue"), tr("Purple"), tr("Magenta")};
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

    for (int index = 0; index < static_cast<int>(m_channelButtons.size()); ++index)
    {
        auto* button = new QToolButton(channelWidget);
        button->setObjectName(objectNames[static_cast<std::size_t>(index)]);
        button->setText(channelLabels[static_cast<std::size_t>(index)]);
        button->setAccessibleName(tr("%1 HSL channel").arg(channelLabels[static_cast<std::size_t>(index)]));
        button->setToolTip(tr("Edit the %1 HSL channel").arg(channelLabels[static_cast<std::size_t>(index)]));
        button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        button->setCheckable(true);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        QPixmap swatch(12, 12);
        swatch.fill(ChannelColors[static_cast<std::size_t>(index)]);
        button->setIcon(QIcon(swatch));
        button->setIconSize(QSize(12, 12));
        button->installEventFilter(this);
        m_channelGroup->addButton(button, index);
        m_channelButtons[static_cast<std::size_t>(index)] = button;
    }
    m_channelButtons.front()->setChecked(true);
    reflowChannelButtons(2);
    layout->addWidget(channelWidget);

    m_hueControl = createParameterControl(tr("Hue"),
                                          QStringLiteral("hslHueSlider"),
                                          tr("Selected channel hue shift; -100 to +100 maps to -30° to +30°."));
    m_saturationControl = createParameterControl(
        tr("Saturation"), QStringLiteral("hslSaturationSlider"), tr("Selected channel saturation adjustment."));
    m_luminanceControl = createParameterControl(
        tr("Luminance"), QStringLiteral("hslLuminanceSlider"), tr("Selected channel HSL lightness adjustment."));
    layout->addWidget(m_hueControl);
    layout->addWidget(m_saturationControl);
    layout->addWidget(m_luminanceControl);

    auto* resetLayout = new QHBoxLayout();
    resetLayout->setContentsMargins({});
    resetLayout->addStretch();
    m_resetChannelButton = new QToolButton(this);
    m_resetChannelButton->setObjectName(QStringLiteral("hslResetChannelButton"));
    m_resetChannelButton->setText(tr("Reset Channel"));
    m_resetChannelButton->setToolTip(tr("Reset Hue, Saturation, and Luminance for the selected channel"));
    m_resetAllButton = new QToolButton(this);
    m_resetAllButton->setObjectName(QStringLiteral("hslResetAllButton"));
    m_resetAllButton->setText(tr("Reset HSL"));
    m_resetAllButton->setToolTip(tr("Reset all HSL channels"));
    resetLayout->addWidget(m_resetChannelButton);
    resetLayout->addWidget(m_resetAllButton);
    layout->addLayout(resetLayout);

    connect(m_channelGroup, &QButtonGroup::idClicked, this, &HslMixerWidget::selectChannel);
    connect(m_hueControl, &AdjustmentParameterControl::valueChanged, this, [this](double value) {
        applyCurrentChannelValue(&core::types::HslChannelAdjustment::hue, value);
    });
    connect(m_saturationControl, &AdjustmentParameterControl::valueChanged, this, [this](double value) {
        applyCurrentChannelValue(&core::types::HslChannelAdjustment::saturation, value);
    });
    connect(m_luminanceControl, &AdjustmentParameterControl::valueChanged, this, [this](double value) {
        applyCurrentChannelValue(&core::types::HslChannelAdjustment::luminance, value);
    });
    for (AdjustmentParameterControl* control : {m_hueControl, m_saturationControl, m_luminanceControl})
    {
        connect(control, &AdjustmentParameterControl::adjustmentStarted, this, &HslMixerWidget::adjustmentStarted);
        connect(control, &AdjustmentParameterControl::adjustmentFinished, this, &HslMixerWidget::adjustmentFinished);
    }
    connect(m_resetChannelButton, &QToolButton::clicked, this, &HslMixerWidget::resetSelectedChannel);
    connect(m_resetAllButton, &QToolButton::clicked, this, &HslMixerWidget::resetAllChannels);
    updateControls();
}

// 목적: 외부 HSL snapshot을 새 edit 없이 모든 channel control에 반영
// 입력: params: 표시할 8개 channel HSL 값
// 출력: hslChanged나 adjustment signal 없이 표시 상태 갱신
void HslMixerWidget::setHsl(const core::types::HslParams& params)
{
    m_hsl = params;
    updateControls();
}

// 목적: 현재 widget이 보유한 HSL projection 반환
// 입력: 없음
// 출력: 8개 channel HSL 값의 읽기 전용 참조
const core::types::HslParams& HslMixerWidget::hsl() const noexcept
{
    return m_hsl;
}

// 목적: 현재 선택된 HSL channel index 반환
// 입력: 없음
// 출력: red부터 magenta까지 0~7 index
int HslMixerWidget::selectedChannelIndex() const noexcept
{
    return m_selectedChannel;
}

// 목적: 200 px drawer에서도 horizontal overflow 없이 사용할 최소 크기 제공
// 입력: 없음
// 출력: compact 2열 selector를 허용하는 size hint
QSize HslMixerWidget::minimumSizeHint() const
{
    const QSize base = QWidget::minimumSizeHint();
    return {160, base.height()};
}

// 목적: drawer 폭에 따라 channel selector를 2열 또는 4열로 재배치
// 입력: event: 새 widget 크기
// 출력: channel button grid 갱신
void HslMixerWidget::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    reflowChannelButtons(event->size().width() >= WideChannelGridMinimum ? 4 : 2);
}

// 목적: channel button의 방향키와 Home/End keyboard navigation 처리
// 입력: watched: event 대상, event: keyboard 또는 일반 event
// 출력: channel navigation을 처리했으면 true
bool HslMixerWidget::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() != QEvent::KeyPress)
    {
        return QWidget::eventFilter(watched, event);
    }

    const auto* keyEvent = static_cast<QKeyEvent*>(event);
    int target = -1;
    switch (keyEvent->key())
    {
    case Qt::Key_Left:
    case Qt::Key_Up:
        target = (m_selectedChannel + 7) % 8;
        break;
    case Qt::Key_Right:
    case Qt::Key_Down:
        target = (m_selectedChannel + 1) % 8;
        break;
    case Qt::Key_Home:
        target = 0;
        break;
    case Qt::Key_End:
        target = 7;
        break;
    default:
        return QWidget::eventFilter(watched, event);
    }

    selectChannel(target);
    m_channelButtons[static_cast<std::size_t>(target)]->setFocus();
    return true;
}

// 목적: 공통 범위와 표시 형식의 HSL parameter control 생성
// 입력: label: 표시명, objectName: slider 식별자, toolTip: parameter 의미 안내
// 출력: Classic 절대 slider로 고정된 parameter control
AdjustmentParameterControl* HslMixerWidget::createParameterControl(const QString& label,
                                                                   const QString& objectName,
                                                                   const QString& toolTip)
{
    AdjustmentParameterConfiguration configuration;
    configuration.label = label;
    configuration.sliderObjectName = objectName;
    configuration.minimum = -1.0;
    configuration.maximum = 1.0;
    configuration.sliderScale = 100.0;
    configuration.displayScale = 100.0;
    configuration.singleStep = 0.01;
    configuration.maximumRate = 0.1;
    configuration.displayDecimals = 0;
    configuration.showPlusSign = true;
    auto* control = new AdjustmentParameterControl(configuration, this);
    control->setControlStyle(AdjustmentControlStyle::Classic);
    control->setToolTip(toolTip);
    return control;
}

// 목적: 선택 channel을 바꾸고 해당 HSL 값을 control에 표시
// 입력: channelIndex: red부터 magenta까지 0~7 index
// 출력: edit signal 없이 선택과 control 값 갱신
void HslMixerWidget::selectChannel(int channelIndex)
{
    if (channelIndex < 0 || channelIndex >= static_cast<int>(m_channelButtons.size()))
    {
        return;
    }
    m_selectedChannel = channelIndex;
    m_channelButtons[static_cast<std::size_t>(channelIndex)]->setChecked(true);
    updateControls();
}

// 목적: 현재 선택 channel 값을 세 parameter control에 signal 없이 반영
// 입력: 없음
// 출력: Hue/Saturation/Luminance와 reset 활성 상태 갱신
void HslMixerWidget::updateControls()
{
    const core::types::HslChannelAdjustment& channel = m_hsl.channels[static_cast<std::size_t>(m_selectedChannel)];
    m_hueControl->setValue(channel.hue);
    m_saturationControl->setValue(channel.saturation);
    m_luminanceControl->setValue(channel.luminance);
    updateResetButtons();
}

// 목적: 현재 HSL identity 여부에 따라 reset button 활성 상태 갱신
// 입력: 없음
// 출력: selected channel과 전체 HSL reset 활성 상태 갱신
void HslMixerWidget::updateResetButtons()
{
    const core::types::HslChannelAdjustment& selected = m_hsl.channels[static_cast<std::size_t>(m_selectedChannel)];
    m_resetChannelButton->setEnabled(selected != core::types::HslChannelAdjustment{});
    m_resetAllButton->setEnabled(m_hsl != core::types::HslParams{});
}

// 목적: 선택 channel의 한 parameter만 바꾸고 전체 HSL snapshot publish
// 입력: parameter: HSL channel member, value: -1~1 절대 값
// 출력: hslChanged signal 발생 가능
void HslMixerWidget::applyCurrentChannelValue(float core::types::HslChannelAdjustment::* parameter, double value)
{
    core::types::HslChannelAdjustment& channel = m_hsl.channels[static_cast<std::size_t>(m_selectedChannel)];
    channel.*parameter = static_cast<float>(std::clamp(value, -1.0, 1.0));
    updateResetButtons();
    emit hslChanged(m_hsl);
}

// 목적: 선택 channel의 HSL 값을 identity로 복원
// 입력: 없음
// 출력: 값이 바뀌면 한 adjustment transaction과 hslChanged 발생
void HslMixerWidget::resetSelectedChannel()
{
    core::types::HslChannelAdjustment& channel = m_hsl.channels[static_cast<std::size_t>(m_selectedChannel)];
    if (channel == core::types::HslChannelAdjustment{})
    {
        return;
    }
    emit adjustmentStarted();
    channel = {};
    updateControls();
    emit hslChanged(m_hsl);
    emit adjustmentFinished();
}

// 목적: 8개 channel HSL 값을 identity로 복원
// 입력: 없음
// 출력: 값이 바뀌면 한 adjustment transaction과 hslChanged 발생
void HslMixerWidget::resetAllChannels()
{
    if (m_hsl == core::types::HslParams{})
    {
        return;
    }
    emit adjustmentStarted();
    m_hsl = {};
    updateControls();
    emit hslChanged(m_hsl);
    emit adjustmentFinished();
}

// 목적: channel button을 지정 열 수의 grid로 재배치
// 입력: columns: 2 또는 4
// 출력: button 순서를 유지한 grid geometry 갱신
void HslMixerWidget::reflowChannelButtons(int columns)
{
    if (columns == m_channelColumns)
    {
        return;
    }
    m_channelColumns = columns;
    for (int index = 0; index < static_cast<int>(m_channelButtons.size()); ++index)
    {
        QToolButton* button = m_channelButtons[static_cast<std::size_t>(index)];
        m_channelLayout->removeWidget(button);
        m_channelLayout->addWidget(button, index / columns, index % columns);
    }
}

}  // namespace flexraw::ui::editor
