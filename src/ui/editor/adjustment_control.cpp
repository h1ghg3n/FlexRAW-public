#include "adjustment_control.h"

#include <algorithm>
#include <cmath>

#include <QAbstractSpinBox>
#include <QApplication>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QSignalBlocker>
#include <QSlider>
#include <QStyle>
#include <QStyleOptionSlider>
#include <QTimer>
#include <QVBoxLayout>

namespace flexraw::ui::editor
{
namespace
{

constexpr int KnobWidth = 72;
constexpr int KnobHeight = 22;
constexpr int HorizontalMargin = 8;
constexpr int TrackCenterOverhang = 19;
constexpr int RateTimerIntervalMs = 16;
constexpr double NeutralDeadZoneRatio = 0.04;
constexpr double LinearRateWeight = 0.25;

class DirectTrackSlider final : public QSlider
{
public:
    using QSlider::QSlider;

protected:
    // 목적: handle 또는 track press를 absolute slider position과 drag 시작으로 변환
    // 입력: event: local pointer 위치와 button 정보
    // 출력: left press면 sliderPressed와 valueChanged signal 발생 가능
    void mousePressEvent(QMouseEvent* event) override
    {
        if (event->button() != Qt::LeftButton)
        {
            QSlider::mousePressEvent(event);
            return;
        }

        QStyleOptionSlider option;
        initStyleOption(&option);
        const QStyle::SubControl hitControl =
            style()->hitTestComplexControl(QStyle::CC_Slider, &option, event->position().toPoint(), this);
        if (hitControl == QStyle::SC_SliderHandle)
        {
            m_directTrackDrag = false;
            QSlider::mousePressEvent(event);
            return;
        }

        m_directTrackDrag = true;
        setSliderDown(true);
        updatePosition(event->position().toPoint());
        event->accept();
    }

    // 목적: slider-down 상태의 pointer 위치를 absolute slider position으로 갱신
    // 입력: event: 현재 local pointer 위치와 button 상태
    // 출력: drag 중 valueChanged signal 발생 가능
    void mouseMoveEvent(QMouseEvent* event) override
    {
        if (!m_directTrackDrag || !isSliderDown() || !event->buttons().testFlag(Qt::LeftButton))
        {
            QSlider::mouseMoveEvent(event);
            return;
        }
        updatePosition(event->position().toPoint());
        event->accept();
    }

    // 목적: 마지막 absolute position을 적용하고 Classic adjustment 종료
    // 입력: event: release 위치와 button 정보
    // 출력: sliderReleased signal 발생
    void mouseReleaseEvent(QMouseEvent* event) override
    {
        if (!m_directTrackDrag || event->button() != Qt::LeftButton || !isSliderDown())
        {
            QSlider::mouseReleaseEvent(event);
            return;
        }
        updatePosition(event->position().toPoint());
        setSliderDown(false);
        m_directTrackDrag = false;
        event->accept();
    }

private:
    // 목적: widget x 좌표를 handle 크기를 고려한 slider value로 변환
    // 입력: position: local pointer 위치
    // 출력: sliderPosition과 value 갱신
    void updatePosition(const QPoint& position)
    {
        QStyleOptionSlider option;
        initStyleOption(&option);
        const int handleLength = style()->pixelMetric(QStyle::PM_SliderLength, &option, this);
        const int available = std::max(1, width() - handleLength);
        const int pixelPosition = std::clamp(position.x() - (handleLength / 2), 0, available);
        const int sliderPosition =
            QStyle::sliderValueFromPosition(minimum(), maximum(), pixelPosition, available, option.upsideDown);
        setSliderPosition(sliderPosition);
    }

    bool m_directTrackDrag{false};
};

class SignedDoubleSpinBox final : public QDoubleSpinBox
{
public:
    using QDoubleSpinBox::QDoubleSpinBox;

protected:
    // 목적: 양수 adjustment 값에 explicit plus sign을 붙인 numeric text 생성
    // 입력: value: 표시할 spin box 값
    // 출력: locale formatting을 유지한 signed 숫자 text
    [[nodiscard]] QString textFromValue(double value) const override
    {
        const QString text = QDoubleSpinBox::textFromValue(value);
        return value > 0.0 ? QStringLiteral("+") + text : text;
    }
};

// 목적: 기존 slider object name에서 공통 parameter control base name 추출
// 입력: sliderObjectName: Slider suffix를 가진 test/automation 이름
// 출력: suffix가 제거된 object name base
[[nodiscard]] QString parameterObjectBase(QString sliderObjectName)
{
    const QString suffix = QStringLiteral("Slider");
    if (sliderObjectName.endsWith(suffix))
    {
        sliderObjectName.chop(suffix.size());
    }
    return sliderObjectName;
}

}  // namespace

// 목적: neutral-position relative rate controller 초기화
// 입력: parent: Qt 부모 widget
// 출력: 초기화된 RelativeAdjustmentControl 객체
RelativeAdjustmentControl::RelativeAdjustmentControl(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("relativeAdjustmentControl"));
    setCursor(Qt::OpenHandCursor);
    setFocusPolicy(Qt::StrongFocus);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setAccessibleName(tr("Relative adjustment controller"));
    m_rateTimer = new QTimer(this);
    m_rateTimer->setInterval(RateTimerIntervalMs);
    m_rateTimer->setTimerType(Qt::PreciseTimer);
    connect(m_rateTimer, &QTimer::timeout, this, &RelativeAdjustmentControl::applyRateTick);
}

// 목적: relative gesture가 변경할 실제 parameter 범위 설정
// 입력: minimum/maximum: 허용할 절대 parameter 범위
// 출력: 현재 값이 새 범위로 제한되고 repaint 예약
void RelativeAdjustmentControl::setRange(double minimum, double maximum)
{
    if (minimum > maximum)
    {
        std::swap(minimum, maximum);
    }
    m_minimum = minimum;
    m_maximum = maximum;
    m_value = std::clamp(m_value, m_minimum, m_maximum);
    m_continuousValue = std::clamp(m_continuousValue, m_minimum, m_maximum);
    update();
}

// 목적: knob short click 한 번의 parameter 변화량 설정
// 입력: step: 양수인 절대 변화량
// 출력: 이후 click에 적용할 step 갱신
void RelativeAdjustmentControl::setSingleStep(double step)
{
    if (step > 0.0)
    {
        m_singleStep = step;
    }
}

// 목적: rate 적분 결과를 parameter가 표현할 수 있는 최소 단위로 양자화
// 입력: resolution: 양수인 절대 parameter 해상도
// 출력: 이후 rate tick의 publish 해상도 갱신
void RelativeAdjustmentControl::setResolution(double resolution)
{
    if (resolution > 0.0)
    {
        m_resolution = resolution;
    }
}

// 목적: knob가 최대 거리에 있을 때의 절대 parameter 변화 속도 설정
// 입력: maximumRate: 초당 양수 변화량
// 출력: 이후 drag의 최대 rate 갱신
void RelativeAdjustmentControl::setMaximumRate(double maximumRate)
{
    if (maximumRate > 0.0)
    {
        m_maximumRate = maximumRate;
    }
}

// 목적: 외부 parameter state를 gesture 없이 controller에 동기화
// 입력: value: 표시 대상 절대 parameter 값
// 출력: valueChanged signal 없이 내부 값 갱신
void RelativeAdjustmentControl::setValue(double value)
{
    m_value = std::clamp(value, m_minimum, m_maximum);
    m_continuousValue = m_value;
    update();
}

// 목적: 현재 실제 parameter 값 반환
// 입력: 없음
// 출력: gesture release 후에도 유지되는 절대 값
double RelativeAdjustmentControl::value() const noexcept
{
    return m_value;
}

// 목적: Relative Wide controller 권장 크기 반환
// 입력: 없음
// 출력: full track과 knob를 표시할 size hint
QSize RelativeAdjustmentControl::sizeHint() const
{
    return {240, 30};
}

// 목적: Relative Wide controller 최소 크기 반환
// 입력: 없음
// 출력: Compact 전환 없이 유지할 최소 size hint
QSize RelativeAdjustmentControl::minimumSizeHint() const
{
    return {140, 28};
}

// 목적: track, neutral notch, transient rate displacement와 knob painting
// 입력: event: Qt paint event
// 출력: 현재 gesture state가 widget surface에 표시됨
void RelativeAdjustmentControl::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QPalette colors = palette();
    const int centerX = rect().center().x();
    const int centerY = rect().center().y();
    const int trackLeft = HorizontalMargin + (KnobWidth / 2) - TrackCenterOverhang;
    const int trackRight = width() - HorizontalMargin - (KnobWidth / 2) + TrackCenterOverhang;
    const QRect knob = neutralKnobRect().translated(m_gestureDisplacement, 0);

    QPen trackPen(colors.color(QPalette::Mid), 2.0);
    trackPen.setCapStyle(Qt::RoundCap);
    painter.setPen(trackPen);
    const int leftTrackEnd = knob.left() - 2;
    const int rightTrackStart = knob.right() + 2;
    if (leftTrackEnd > trackLeft)
    {
        painter.drawLine(trackLeft, centerY, leftTrackEnd, centerY);
    }
    if (rightTrackStart < trackRight)
    {
        painter.drawLine(rightTrackStart, centerY, trackRight, centerY);
    }

    painter.setPen(QPen(colors.color(QPalette::Text), 1.0));
    painter.drawLine(centerX, centerY - 8, centerX, centerY + 8);

    if (m_dragging && m_gestureDisplacement != 0)
    {
        QPen displacementPen(colors.color(QPalette::Highlight), 4.0);
        displacementPen.setCapStyle(Qt::RoundCap);
        painter.setPen(displacementPen);
        painter.drawLine(centerX, centerY, centerX + m_gestureDisplacement, centerY);
    }

    painter.setPen(QPen(colors.color(QPalette::Mid), 1.0));
    QColor knobColor = colors.color(QPalette::Button);
    knobColor.setAlpha(255);
    painter.setBrush(knobColor);
    painter.drawRoundedRect(knob, 5.0, 5.0);
    painter.setPen(colors.color(QPalette::ButtonText));
    painter.drawLine(knob.center().x(), knob.top() + 4, knob.center().x(), knob.bottom() - 4);

    QPainterPath leftArrow;
    const int leftCenter = knob.left() + (knob.width() / 4);
    leftArrow.moveTo(leftCenter + 3, centerY - 5);
    leftArrow.lineTo(leftCenter - 3, centerY);
    leftArrow.lineTo(leftCenter + 3, centerY + 5);
    QPainterPath rightArrow;
    const int rightCenter = knob.right() - (knob.width() / 4);
    rightArrow.moveTo(rightCenter - 3, centerY - 5);
    rightArrow.lineTo(rightCenter + 3, centerY);
    rightArrow.lineTo(rightCenter - 3, centerY + 5);
    painter.drawPath(leftArrow);
    painter.drawPath(rightArrow);
}

// 목적: neutral knob 안의 left/right click 또는 drag 후보 gesture 시작
// 입력: event: local pointer 위치와 button 정보
// 출력: 유효한 left-button press면 adjustmentStarted signal 발생
void RelativeAdjustmentControl::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton || !neutralKnobRect().contains(event->position().toPoint()))
    {
        QWidget::mousePressEvent(event);
        return;
    }

    m_pressed = true;
    m_dragging = false;
    m_pressPosition = event->position().toPoint();
    m_pressValue = m_value;
    m_continuousValue = m_value;
    m_gestureDisplacement = 0;
    setCursor(Qt::ClosedHandCursor);
    emit adjustmentStarted();
    event->accept();
}

// 목적: drag threshold를 넘은 pointer 변위를 지속 rate로 변환
// 입력: event: 현재 local pointer 위치와 button 상태
// 출력: knob displacement와 이후 timer tick rate 갱신
void RelativeAdjustmentControl::mouseMoveEvent(QMouseEvent* event)
{
    if (!m_pressed || !event->buttons().testFlag(Qt::LeftButton))
    {
        QWidget::mouseMoveEvent(event);
        return;
    }

    const QPoint displacement = event->position().toPoint() - m_pressPosition;
    if (!m_dragging && displacement.manhattanLength() >= QApplication::startDragDistance())
    {
        m_dragging = true;
        m_continuousValue = m_value;
        m_rateClock.start();
        m_rateTimer->start();
    }
    if (m_dragging)
    {
        applyRateTick();
        m_gestureDisplacement = std::clamp(displacement.x(), -maximumDisplacement(), maximumDisplacement());
        update();
    }
    event->accept();
}

// 목적: short click 적용 또는 rate 적분을 정지하고 knob를 neutral로 복귀
// 입력: event: release 위치와 button 정보
// 출력: click이면 한 step 변경, 이후 adjustmentFinished signal 발생
void RelativeAdjustmentControl::mouseReleaseEvent(QMouseEvent* event)
{
    if (!m_pressed || event->button() != Qt::LeftButton)
    {
        QWidget::mouseReleaseEvent(event);
        return;
    }

    if (m_dragging)
    {
        applyRateTick();
    }
    else
    {
        const int neutralCenter = neutralKnobRect().center().x();
        if (m_pressPosition.x() < neutralCenter)
        {
            publishValue(m_pressValue - m_singleStep);
        }
        else if (m_pressPosition.x() > neutralCenter)
        {
            publishValue(m_pressValue + m_singleStep);
        }
    }

    m_rateTimer->stop();
    m_pressed = false;
    m_dragging = false;
    m_gestureDisplacement = 0;
    setCursor(Qt::OpenHandCursor);
    update();
    emit adjustmentFinished();
    event->accept();
}

// 목적: neutral position의 knob rectangle 계산
// 입력: 없음
// 출력: gesture displacement가 0인 knob geometry
QRect RelativeAdjustmentControl::neutralKnobRect() const
{
    return {rect().center().x() - (KnobWidth / 2), rect().center().y() - (KnobHeight / 2), KnobWidth, KnobHeight};
}

// 목적: knob가 track 안에 머무는 최대 horizontal displacement 계산
// 입력: 없음
// 출력: pixel 단위 양수 변위 상한
int RelativeAdjustmentControl::maximumDisplacement() const
{
    return std::max(1, ((width() - KnobWidth) / 2) - HorizontalMargin);
}

// 목적: drag 거리를 neutral dead zone과 혼합 2차 곡선으로 초당 rate에 매핑
// 입력: displacement: neutral notch 기준 signed pixel 변위
// 출력: 최대 rate 범위의 signed parameter units/second
double RelativeAdjustmentControl::rateForDisplacement(int displacement) const
{
    const double normalized =
        std::clamp(static_cast<double>(std::abs(displacement)) / static_cast<double>(maximumDisplacement()), 0.0, 1.0);
    if (normalized <= NeutralDeadZoneRatio)
    {
        return 0.0;
    }
    const double activeDistance = (normalized - NeutralDeadZoneRatio) / (1.0 - NeutralDeadZoneRatio);
    const double curved =
        (LinearRateWeight * activeDistance) + ((1.0 - LinearRateWeight) * activeDistance * activeDistance);
    return std::copysign(m_maximumRate * curved, static_cast<double>(displacement));
}

// 목적: 직전 tick 이후 경과 시간만큼 현재 displacement rate를 값에 적분
// 입력: 없음
// 출력: 값이 바뀌면 valueChanged signal 발생
void RelativeAdjustmentControl::applyRateTick()
{
    if (!m_dragging || !m_rateClock.isValid())
    {
        return;
    }
    constexpr double NanosecondsPerSecond = 1'000'000'000.0;
    const double elapsedSeconds = static_cast<double>(m_rateClock.nsecsElapsed()) / NanosecondsPerSecond;
    m_rateClock.restart();
    m_continuousValue = std::clamp(
        m_continuousValue + (rateForDisplacement(m_gestureDisplacement) * elapsedSeconds), m_minimum, m_maximum);
    publishValue(quantizedValue(m_continuousValue));
}

// 목적: parameter resolution에 맞춘 range 내부 값 생성
// 입력: value: 양자화할 절대 parameter 후보
// 출력: resolution과 range로 제한된 값
double RelativeAdjustmentControl::quantizedValue(double value) const
{
    const double quantized = std::round(value / m_resolution) * m_resolution;
    return std::clamp(quantized, m_minimum, m_maximum);
}

// 목적: user gesture 값을 저장하고 실제 변경 시 signal publish
// 입력: value: 새 절대 parameter 후보
// 출력: 값이 변경되면 valueChanged signal 발생
void RelativeAdjustmentControl::publishValue(double value)
{
    const double bounded = std::clamp(value, m_minimum, m_maximum);
    if (qFuzzyCompare(m_value + 1.0, bounded + 1.0))
    {
        return;
    }
    m_value = bounded;
    emit valueChanged(m_value);
}

// 목적: label/value header와 교체 가능한 Classic/Relative parameter control 초기화
// 입력: configuration: 이름, 범위, 표시 단위와 rate, parent: Qt 부모 widget
// 출력: Classic style이 선택된 AdjustmentParameterControl 객체
AdjustmentParameterControl::AdjustmentParameterControl(const AdjustmentParameterConfiguration& configuration,
                                                       QWidget* parent)
    : QWidget(parent),
      m_minimum(std::min(configuration.minimum, configuration.maximum)),
      m_maximum(std::max(configuration.minimum, configuration.maximum)),
      m_sliderScale(configuration.sliderScale > 0.0 ? configuration.sliderScale : 1.0),
      m_displayScale(configuration.displayScale > 0.0 ? configuration.displayScale : 1.0)
{
    const QString objectBase = parameterObjectBase(configuration.sliderObjectName);
    setObjectName(objectBase + QStringLiteral("AdjustmentControl"));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins({});
    layout->setSpacing(1);

    auto* headerLayout = new QHBoxLayout();
    headerLayout->setContentsMargins({});
    auto* label = new QLabel(configuration.label, this);
    m_valueSpinBox = configuration.showPlusSign ? static_cast<QDoubleSpinBox*>(new SignedDoubleSpinBox(this))
                                                : new QDoubleSpinBox(this);
    label->setBuddy(m_valueSpinBox);
    m_valueSpinBox->setObjectName(objectBase + QStringLiteral("ValueSpinBox"));
    m_valueSpinBox->setRange(m_minimum * m_displayScale, m_maximum * m_displayScale);
    m_valueSpinBox->setDecimals(configuration.displayDecimals);
    m_valueSpinBox->setSingleStep(configuration.singleStep * m_displayScale);
    m_valueSpinBox->setSuffix(configuration.suffix);
    m_valueSpinBox->setAlignment(Qt::AlignRight);
    m_valueSpinBox->setButtonSymbols(QAbstractSpinBox::NoButtons);
    m_valueSpinBox->setFixedWidth(64);
    headerLayout->addWidget(label);
    headerLayout->addStretch();
    headerLayout->addWidget(m_valueSpinBox);
    layout->addLayout(headerLayout);

    m_classicPage = new QWidget(this);
    auto* classicLayout = new QHBoxLayout(m_classicPage);
    classicLayout->setContentsMargins({});
    m_classicSlider = new DirectTrackSlider(Qt::Horizontal, m_classicPage);
    m_classicSlider->setObjectName(configuration.sliderObjectName);
    m_classicSlider->setRange(static_cast<int>(std::lround(m_minimum * m_sliderScale)),
                              static_cast<int>(std::lround(m_maximum * m_sliderScale)));
    m_classicSlider->setSingleStep(1);
    m_classicSlider->setPageStep(std::max(1, static_cast<int>(std::lround(configuration.singleStep * m_sliderScale))));
    classicLayout->addWidget(m_classicSlider);

    m_relativePage = new QWidget(this);
    auto* relativeLayout = new QHBoxLayout(m_relativePage);
    relativeLayout->setContentsMargins({});
    m_relativeControl = new RelativeAdjustmentControl(m_relativePage);
    m_relativeControl->setObjectName(objectBase + QStringLiteral("RelativeControl"));
    m_relativeControl->setRange(m_minimum, m_maximum);
    m_relativeControl->setSingleStep(configuration.singleStep);
    m_relativeControl->setResolution(1.0 / m_sliderScale);
    m_relativeControl->setMaximumRate(configuration.maximumRate);
    relativeLayout->addWidget(m_relativeControl);

    auto* controlLayout = new QVBoxLayout();
    controlLayout->setContentsMargins({});
    controlLayout->setSpacing(0);
    controlLayout->addWidget(m_classicPage);
    controlLayout->addWidget(m_relativePage);
    m_relativePage->hide();
    layout->addLayout(controlLayout);

    m_value = std::clamp(0.0, m_minimum, m_maximum);
    setValue(m_value);
    connect(m_classicSlider, &QSlider::sliderPressed, this, &AdjustmentParameterControl::adjustmentStarted);
    connect(m_classicSlider, &QSlider::valueChanged, this, [this](int value) {
        applyUserValue(static_cast<double>(value) / m_sliderScale, m_classicSlider);
    });
    connect(m_classicSlider, &QSlider::sliderReleased, this, &AdjustmentParameterControl::adjustmentFinished);
    connect(m_relativeControl,
            &RelativeAdjustmentControl::adjustmentStarted,
            this,
            &AdjustmentParameterControl::adjustmentStarted);
    connect(m_relativeControl, &RelativeAdjustmentControl::valueChanged, this, [this](double value) {
        applyUserValue(value, m_relativeControl);
    });
    connect(m_relativeControl,
            &RelativeAdjustmentControl::adjustmentFinished,
            this,
            &AdjustmentParameterControl::adjustmentFinished);
    connect(m_valueSpinBox, &QDoubleSpinBox::valueChanged, this, [this](double value) {
        if (!m_spinAdjustmentInProgress)
        {
            m_spinAdjustmentInProgress = true;
            emit adjustmentStarted();
        }
        applyUserValue(value / m_displayScale, m_valueSpinBox);
    });
    connect(m_valueSpinBox, &QDoubleSpinBox::editingFinished, this, [this] {
        if (m_spinAdjustmentInProgress)
        {
            m_spinAdjustmentInProgress = false;
            emit adjustmentFinished();
        }
    });
}

// 목적: 실제 parameter 값을 모든 presentation에 signal 없이 동기화
// 입력: value: configuration 범위의 절대 값
// 출력: Classic, Relative와 numeric value가 같은 값으로 갱신
void AdjustmentParameterControl::setValue(double value)
{
    m_value = std::clamp(value, m_minimum, m_maximum);
    const QSignalBlocker sliderBlocker(m_classicSlider);
    const QSignalBlocker spinBoxBlocker(m_valueSpinBox);
    const QSignalBlocker relativeBlocker(m_relativeControl);
    m_classicSlider->setValue(static_cast<int>(std::lround(m_value * m_sliderScale)));
    m_valueSpinBox->setValue(m_value * m_displayScale);
    m_relativeControl->setValue(m_value);
}

// 목적: 현재 parameter 절대 값 반환
// 입력: 없음
// 출력: configuration 범위의 값
double AdjustmentParameterControl::value() const noexcept
{
    return m_value;
}

// 목적: parameter 의미를 유지하며 control presentation 교체
// 입력: style: Classic 또는 Relative
// 출력: 선택 presentation만 표시되고 값과 history signal은 변경되지 않음
void AdjustmentParameterControl::setControlStyle(AdjustmentControlStyle style)
{
    m_controlStyle = style;
    const bool classicVisible = style == AdjustmentControlStyle::Classic;
    m_classicPage->setVisible(classicVisible);
    m_relativePage->setVisible(!classicVisible);
    layout()->invalidate();
    updateGeometry();
}

// 목적: 현재 선택된 control presentation 반환
// 입력: 없음
// 출력: Classic 또는 Relative style
AdjustmentControlStyle AdjustmentParameterControl::controlStyle() const noexcept
{
    return m_controlStyle;
}

// 목적: user control에서 받은 값을 모든 peer presentation에 반영하고 publish
// 입력: value: 새 parameter 후보, source: 변경을 시작한 QObject
// 출력: 실제 값이 변경되면 valueChanged signal 발생
void AdjustmentParameterControl::applyUserValue(double value, const QObject* source)
{
    const double bounded = std::clamp(value, m_minimum, m_maximum);
    if (source != m_classicSlider)
    {
        const QSignalBlocker blocker(m_classicSlider);
        m_classicSlider->setValue(static_cast<int>(std::lround(bounded * m_sliderScale)));
    }
    if (source != m_valueSpinBox)
    {
        const QSignalBlocker blocker(m_valueSpinBox);
        m_valueSpinBox->setValue(bounded * m_displayScale);
    }
    if (source != m_relativeControl)
    {
        const QSignalBlocker blocker(m_relativeControl);
        m_relativeControl->setValue(bounded);
    }
    if (qFuzzyCompare(m_value + 1.0, bounded + 1.0))
    {
        return;
    }
    m_value = bounded;
    emit valueChanged(m_value);
}

}  // namespace flexraw::ui::editor
