#pragma once

#include <array>

#include <QWidget>

#include "hsl_params.h"

class QButtonGroup;
class QEvent;
class QGridLayout;
class QResizeEvent;
class QToolButton;

namespace flexraw::ui::editor
{

class AdjustmentParameterControl;

class HslMixerWidget final : public QWidget
{
    Q_OBJECT

public:
    // 목적: 8개 HSL channel과 선택 channel의 절대 보정 control 초기화
    // 입력: parent: Qt 부모 widget
    // 출력: Red channel이 선택된 HslMixerWidget 객체
    explicit HslMixerWidget(QWidget* parent = nullptr);

    // 목적: 외부 HSL snapshot을 새 edit 없이 모든 channel control에 반영
    // 입력: params: 표시할 8개 channel HSL 값
    // 출력: hslChanged나 adjustment signal 없이 표시 상태 갱신
    void setHsl(const core::types::HslParams& params);

    // 목적: 현재 widget이 보유한 HSL projection 반환
    // 입력: 없음
    // 출력: 8개 channel HSL 값의 읽기 전용 참조
    [[nodiscard]] const core::types::HslParams& hsl() const noexcept;

    // 목적: 현재 선택된 HSL channel index 반환
    // 입력: 없음
    // 출력: red부터 magenta까지 0~7 index
    [[nodiscard]] int selectedChannelIndex() const noexcept;

    // 목적: 200 px drawer에서도 horizontal overflow 없이 사용할 최소 크기 제공
    // 입력: 없음
    // 출력: compact 2열 selector를 허용하는 size hint
    [[nodiscard]] QSize minimumSizeHint() const override;

signals:
    // 목적: 사용자가 수정한 전체 HSL snapshot 전달
    // 입력: params: 대상 channel만 바뀌고 나머지는 보존된 HSL 값
    // 출력: 없음
    void hslChanged(const core::types::HslParams& params);

    // 목적: HSL slider 또는 reset transaction 시작 전달
    // 입력: 없음
    // 출력: 없음
    void adjustmentStarted();

    // 목적: HSL slider 또는 reset transaction 종료 전달
    // 입력: 없음
    // 출력: 없음
    void adjustmentFinished();

protected:
    // 목적: drawer 폭에 따라 channel selector를 2열 또는 4열로 재배치
    // 입력: event: 새 widget 크기
    // 출력: channel button grid 갱신
    void resizeEvent(QResizeEvent* event) override;

    // 목적: channel button의 방향키와 Home/End keyboard navigation 처리
    // 입력: watched: event 대상, event: keyboard 또는 일반 event
    // 출력: channel navigation을 처리했으면 true
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    // 목적: 공통 범위와 표시 형식의 HSL parameter control 생성
    // 입력: label: 표시명, objectName: slider 식별자, toolTip: parameter 의미 안내
    // 출력: Classic 절대 slider로 고정된 parameter control
    AdjustmentParameterControl* createParameterControl(const QString& label,
                                                       const QString& objectName,
                                                       const QString& toolTip);

    // 목적: 선택 channel을 바꾸고 해당 HSL 값을 control에 표시
    // 입력: channelIndex: red부터 magenta까지 0~7 index
    // 출력: edit signal 없이 선택과 control 값 갱신
    void selectChannel(int channelIndex);

    // 목적: 현재 선택 channel 값을 세 parameter control에 signal 없이 반영
    // 입력: 없음
    // 출력: Hue/Saturation/Luminance와 reset 활성 상태 갱신
    void updateControls();

    // 목적: 현재 HSL identity 여부에 따라 reset button 활성 상태 갱신
    // 입력: 없음
    // 출력: selected channel과 전체 HSL reset 활성 상태 갱신
    void updateResetButtons();

    // 목적: 선택 channel의 한 parameter만 바꾸고 전체 HSL snapshot publish
    // 입력: parameter: HSL channel member, value: -1~1 절대 값
    // 출력: hslChanged signal 발생 가능
    void applyCurrentChannelValue(float core::types::HslChannelAdjustment::* parameter, double value);

    // 목적: 선택 channel의 HSL 값을 identity로 복원
    // 입력: 없음
    // 출력: 값이 바뀌면 한 adjustment transaction과 hslChanged 발생
    void resetSelectedChannel();

    // 목적: 8개 channel HSL 값을 identity로 복원
    // 입력: 없음
    // 출력: 값이 바뀌면 한 adjustment transaction과 hslChanged 발생
    void resetAllChannels();

    // 목적: channel button을 지정 열 수의 grid로 재배치
    // 입력: columns: 2 또는 4
    // 출력: button 순서를 유지한 grid geometry 갱신
    void reflowChannelButtons(int columns);

    core::types::HslParams m_hsl;
    QButtonGroup* m_channelGroup{nullptr};
    QGridLayout* m_channelLayout{nullptr};
    std::array<QToolButton*, 8> m_channelButtons{};
    AdjustmentParameterControl* m_hueControl{nullptr};
    AdjustmentParameterControl* m_saturationControl{nullptr};
    AdjustmentParameterControl* m_luminanceControl{nullptr};
    QToolButton* m_resetChannelButton{nullptr};
    QToolButton* m_resetAllButton{nullptr};
    int m_selectedChannel{0};
    int m_channelColumns{0};
};

}  // namespace flexraw::ui::editor
