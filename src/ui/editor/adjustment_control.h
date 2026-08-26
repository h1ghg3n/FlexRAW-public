#pragma once

#include <QElapsedTimer>
#include <QPoint>
#include <QString>
#include <QWidget>

#include "adjustment_control_style.h"

class QDoubleSpinBox;
class QMouseEvent;
class QPaintEvent;
class QSlider;
class QTimer;

namespace flexraw::ui::editor
{

struct AdjustmentParameterConfiguration final
{
    QString label;
    QString sliderObjectName;
    QString suffix;
    double minimum{0.0};
    double maximum{1.0};
    double sliderScale{100.0};
    double displayScale{100.0};
    double singleStep{0.01};
    double maximumRate{0.1};
    int displayDecimals{0};
    bool showPlusSign{true};
};

class RelativeAdjustmentControl final : public QWidget
{
    Q_OBJECT

public:
    // 목적: neutral-position relative rate controller 초기화
    // 입력: parent: Qt 부모 widget
    // 출력: 초기화된 RelativeAdjustmentControl 객체
    explicit RelativeAdjustmentControl(QWidget* parent = nullptr);

    // 목적: relative gesture가 변경할 실제 parameter 범위 설정
    // 입력: minimum/maximum: 허용할 절대 parameter 범위
    // 출력: 현재 값이 새 범위로 제한되고 repaint 예약
    void setRange(double minimum, double maximum);

    // 목적: knob short click 한 번의 parameter 변화량 설정
    // 입력: step: 양수인 절대 변화량
    // 출력: 이후 click에 적용할 step 갱신
    void setSingleStep(double step);

    // 목적: rate 적분 결과를 parameter가 표현할 수 있는 최소 단위로 양자화
    // 입력: resolution: 양수인 절대 parameter 해상도
    // 출력: 이후 rate tick의 publish 해상도 갱신
    void setResolution(double resolution);

    // 목적: knob가 최대 거리에 있을 때의 절대 parameter 변화 속도 설정
    // 입력: maximumRate: 초당 양수 변화량
    // 출력: 이후 drag의 최대 rate 갱신
    void setMaximumRate(double maximumRate);

    // 목적: 외부 parameter state를 gesture 없이 controller에 동기화
    // 입력: value: 표시 대상 절대 parameter 값
    // 출력: valueChanged signal 없이 내부 값 갱신
    void setValue(double value);

    // 목적: 현재 실제 parameter 값 반환
    // 입력: 없음
    // 출력: gesture release 후에도 유지되는 절대 값
    [[nodiscard]] double value() const noexcept;

    // 목적: Relative Wide controller 권장 크기 반환
    // 입력: 없음
    // 출력: full track과 knob를 표시할 size hint
    [[nodiscard]] QSize sizeHint() const override;

    // 목적: Relative Wide controller 최소 크기 반환
    // 입력: 없음
    // 출력: Compact 전환 없이 유지할 최소 size hint
    [[nodiscard]] QSize minimumSizeHint() const override;

signals:
    // 목적: relative click 또는 drag transaction 시작 전달
    // 입력: 없음
    // 출력: 없음
    void adjustmentStarted();

    // 목적: relative gesture로 변경된 절대 parameter 값 전달
    // 입력: value: range로 제한된 새 절대 값
    // 출력: 없음
    void valueChanged(double value);

    // 목적: relative click 또는 drag transaction 종료 전달
    // 입력: 없음
    // 출력: 없음
    void adjustmentFinished();

protected:
    // 목적: track, neutral notch, transient rate displacement와 knob painting
    // 입력: event: Qt paint event
    // 출력: 현재 gesture state가 widget surface에 표시됨
    void paintEvent(QPaintEvent* event) override;

    // 목적: neutral knob 안의 left/right click 또는 drag 후보 gesture 시작
    // 입력: event: local pointer 위치와 button 정보
    // 출력: 유효한 left-button press면 adjustmentStarted signal 발생
    void mousePressEvent(QMouseEvent* event) override;

    // 목적: drag threshold를 넘은 pointer 변위를 지속 rate로 변환
    // 입력: event: 현재 local pointer 위치와 button 상태
    // 출력: knob displacement와 이후 timer tick rate 갱신
    void mouseMoveEvent(QMouseEvent* event) override;

    // 목적: short click 적용 또는 rate 적분을 정지하고 knob를 neutral로 복귀
    // 입력: event: release 위치와 button 정보
    // 출력: click이면 한 step 변경, 이후 adjustmentFinished signal 발생
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    // 목적: neutral position의 knob rectangle 계산
    // 입력: 없음
    // 출력: gesture displacement가 0인 knob geometry
    [[nodiscard]] QRect neutralKnobRect() const;

    // 목적: knob가 track 안에 머무는 최대 horizontal displacement 계산
    // 입력: 없음
    // 출력: pixel 단위 양수 변위 상한
    [[nodiscard]] int maximumDisplacement() const;

    // 목적: drag 거리를 neutral dead zone과 혼합 2차 곡선으로 초당 rate에 매핑
    // 입력: displacement: neutral notch 기준 signed pixel 변위
    // 출력: 최대 rate 범위의 signed parameter units/second
    [[nodiscard]] double rateForDisplacement(int displacement) const;

    // 목적: 직전 tick 이후 경과 시간만큼 현재 displacement rate를 값에 적분
    // 입력: 없음
    // 출력: 값이 바뀌면 valueChanged signal 발생
    void applyRateTick();

    // 목적: parameter resolution에 맞춘 range 내부 값 생성
    // 입력: value: 양자화할 절대 parameter 후보
    // 출력: resolution과 range로 제한된 값
    [[nodiscard]] double quantizedValue(double value) const;

    // 목적: user gesture 값을 저장하고 실제 변경 시 signal publish
    // 입력: value: 새 절대 parameter 후보
    // 출력: 값이 변경되면 valueChanged signal 발생
    void publishValue(double value);

    double m_minimum{-5.0};
    double m_maximum{5.0};
    double m_singleStep{0.1};
    double m_resolution{0.01};
    double m_maximumRate{1.0};
    double m_value{0.0};
    double m_pressValue{0.0};
    double m_continuousValue{0.0};
    QPoint m_pressPosition;
    int m_gestureDisplacement{0};
    QTimer* m_rateTimer{nullptr};
    QElapsedTimer m_rateClock;
    bool m_pressed{false};
    bool m_dragging{false};
};

class AdjustmentParameterControl final : public QWidget
{
    Q_OBJECT

public:
    // 목적: label/value header와 교체 가능한 Classic/Relative parameter control 초기화
    // 입력: configuration: 이름, 범위, 표시 단위와 rate, parent: Qt 부모 widget
    // 출력: Classic style이 선택된 AdjustmentParameterControl 객체
    explicit AdjustmentParameterControl(const AdjustmentParameterConfiguration& configuration,
                                        QWidget* parent = nullptr);

    // 목적: 실제 parameter 값을 모든 presentation에 signal 없이 동기화
    // 입력: value: configuration 범위의 절대 값
    // 출력: Classic, Relative와 numeric value가 같은 값으로 갱신
    void setValue(double value);

    // 목적: 현재 parameter 절대 값 반환
    // 입력: 없음
    // 출력: configuration 범위의 값
    [[nodiscard]] double value() const noexcept;

    // 목적: parameter 의미를 유지하며 control presentation 교체
    // 입력: style: Classic 또는 Relative
    // 출력: 선택 presentation만 표시되고 값과 history signal은 변경되지 않음
    void setControlStyle(AdjustmentControlStyle style);

    // 목적: 현재 선택된 control presentation 반환
    // 입력: 없음
    // 출력: Classic 또는 Relative style
    [[nodiscard]] AdjustmentControlStyle controlStyle() const noexcept;

signals:
    // 목적: parameter adjustment transaction 시작 전달
    // 입력: 없음
    // 출력: 없음
    void adjustmentStarted();

    // 목적: presentation과 무관한 새 parameter 절대 값 전달
    // 입력: value: configuration 범위의 값
    // 출력: 없음
    void valueChanged(double value);

    // 목적: parameter adjustment transaction 종료 전달
    // 입력: 없음
    // 출력: 없음
    void adjustmentFinished();

private:
    // 목적: user control에서 받은 값을 모든 peer presentation에 반영하고 publish
    // 입력: value: 새 parameter 후보, source: 변경을 시작한 QObject
    // 출력: 실제 값이 변경되면 valueChanged signal 발생
    void applyUserValue(double value, const QObject* source);

    QSlider* m_classicSlider{nullptr};
    RelativeAdjustmentControl* m_relativeControl{nullptr};
    QDoubleSpinBox* m_valueSpinBox{nullptr};
    QWidget* m_classicPage{nullptr};
    QWidget* m_relativePage{nullptr};
    AdjustmentControlStyle m_controlStyle{AdjustmentControlStyle::Classic};
    double m_minimum{0.0};
    double m_maximum{1.0};
    double m_sliderScale{100.0};
    double m_displayScale{100.0};
    double m_value{0.0};
    bool m_spinAdjustmentInProgress{false};
};

}  // namespace flexraw::ui::editor
