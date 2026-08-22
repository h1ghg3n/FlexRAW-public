#pragma once

#include <vector>

#include <QWidget>

#include "develop_params.h"

class QDoubleSpinBox;
class QFormLayout;
class QComboBox;
class QSlider;
class QSpinBox;

namespace flexraw::core::develop
{
struct ImageHistogram;
}

namespace flexraw::ui::editor
{

class HistogramWidget;

class DevelopPanel : public QWidget
{
    Q_OBJECT

public:
    // 목적: 기본 현상 파라미터를 조작하는 editor panel 초기화
    // 입력: parent: Qt 부모 widget
    // 출력: 초기화된 DevelopPanel 객체
    explicit DevelopPanel(QWidget* parent = nullptr);

    // 목적: 현재 panel에 설정된 develop parameter 읽기 전용 참조 반환
    // 입력: 없음
    // 출력: 현재 DevelopParams 값 참조
    [[nodiscard]] const core::types::DevelopParams& params() const;

    // 목적: 외부 편집 상태를 panel control과 동기화
    // 입력: params: 표시하고 적용할 develop parameter 값
    // 출력: paramsChanged signal 발생
    void setParams(const core::types::DevelopParams& params);

    // 목적: 모든 현상 parameter를 기본값으로 복원
    // 입력: 없음
    // 출력: paramsChanged signal 발생
    void reset();

    // 목적: 현재 preview의 RGB와 휘도 histogram을 panel에 표시
    // 입력: histogram: worker가 계산한 preview histogram
    // 출력: histogram widget repaint 예약
    void setHistogram(const core::develop::ImageHistogram& histogram);

    // 목적: 새 preview를 기다리는 동안 이전 histogram 표시를 제거
    // 입력: 없음
    // 출력: histogram widget repaint 예약
    void clearHistogram();

signals:
    // 목적: 사용자가 수정한 develop parameter를 editor 조립 계층에 전달
    // 입력: params: 갱신된 develop parameter 값
    // 출력: 없음
    void paramsChanged(const core::types::DevelopParams& params);

    // 목적: 연속 slider 또는 spin box 조작의 시작을 history 계층에 전달
    // 입력: 없음
    // 출력: 없음
    void adjustmentStarted();

    // 목적: 연속 slider 또는 spin box 조작의 종료를 history 계층에 전달
    // 입력: 없음
    // 출력: 없음
    void adjustmentFinished();

private:
    struct NormalizedControl
    {
        QSlider* slider{nullptr};
        QSpinBox* spinBox{nullptr};
        float core::types::DevelopParams::* parameter{nullptr};
    };

    // 목적: 노출 control을 생성하고 DevelopParams::exposureEv에 연결
    // 입력: layout: control을 추가할 form layout
    // 출력: 없음
    void addExposureControl(QFormLayout* layout);

    // 목적: as-shot 또는 custom white balance control을 생성하고 DevelopParams에 연결
    // 입력: layout: control을 추가할 form layout
    // 출력: 없음
    void addWhiteBalanceControls(QFormLayout* layout);

    // 목적: 1~3 px 범위의 sharpening radius control을 생성하고 DevelopParams에 연결
    // 입력: layout: control을 추가할 form layout
    // 출력: 없음
    void addSharpeningRadiusControl(QFormLayout* layout);

    // 목적: 0~100% 범위의 sharpening masking control을 생성하고 DevelopParams에 연결
    // 입력: layout: control을 추가할 form layout
    // 출력: 없음
    void addSharpeningMaskingControl(QFormLayout* layout);

    // 목적: 지정된 정수 범위의 표준 develop control을 생성하고 parameter에 연결
    // 입력: layout: 추가 대상 layout, label: 표시명, objectName: test용 이름, parameter: 연결할 값, minimumValue:
    // 최솟값 출력: 없음
    void addNormalizedControl(QFormLayout* layout,
                              const QString& label,
                              const QString& objectName,
                              float core::types::DevelopParams::* parameter,
                              int minimumValue = -100);

    // 목적: 현재 DevelopParams 값을 모든 control에 signal 없이 반영
    // 입력: 없음
    // 출력: 없음
    void updateControls();

    // 목적: 현재 white balance mode에 맞춰 관련 control의 값과 활성 상태를 반영
    // 입력: 없음
    // 출력: 없음
    void updateWhiteBalanceControls();

    // 목적: history를 위한 조작 transaction을 아직 시작하지 않았으면 시작
    // 입력: 없음
    // 출력: adjustmentStarted signal 발생 가능
    void beginAdjustment();

    // 목적: history를 위한 현재 조작 transaction을 종료
    // 입력: 없음
    // 출력: adjustmentFinished signal 발생 가능
    void finishAdjustment();

    core::types::DevelopParams m_params;
    QSlider* m_exposureSlider{nullptr};
    QDoubleSpinBox* m_exposureSpinBox{nullptr};
    QComboBox* m_whiteBalanceModeCombo{nullptr};
    QSpinBox* m_whiteBalanceTemperatureSpinBox{nullptr};
    QSlider* m_whiteBalanceTintSlider{nullptr};
    QSpinBox* m_whiteBalanceTintSpinBox{nullptr};
    QSlider* m_sharpeningRadiusSlider{nullptr};
    QSpinBox* m_sharpeningRadiusSpinBox{nullptr};
    QSlider* m_sharpeningMaskingSlider{nullptr};
    QSpinBox* m_sharpeningMaskingSpinBox{nullptr};
    HistogramWidget* m_histogramWidget{nullptr};
    std::vector<NormalizedControl> m_normalizedControls;
    bool m_adjustmentInProgress{false};
};

}  // namespace flexraw::ui::editor
