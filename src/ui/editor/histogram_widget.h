#pragma once

#include <QWidget>

#include "clipping.h"
#include "histogram.h"

class QLabel;
class QPaintEvent;

namespace flexraw::ui::editor
{

class HistogramWidget final : public QWidget
{
    Q_OBJECT

public:
    // 목적: preview histogram을 표시하는 compact widget 초기화
    // 입력: parent: Qt 부모 widget
    // 출력: 초기화된 HistogramWidget 객체
    explicit HistogramWidget(QWidget* parent = nullptr);

    // 목적: 표시할 preview histogram을 갱신
    // 입력: histogram: RGB와 휘도 bin을 포함하는 계산 결과
    // 출력: widget repaint 예약
    void setHistogram(const core::develop::ImageHistogram& histogram);

    // 목적: histogram 양쪽 indicator에 shadow/highlight clipping 비율 표시
    // 입력: clipping: shadow/highlight/전체 preview pixel 수
    // 출력: indicator text, tooltip과 활성 색상 갱신
    void setClippingSummary(const core::develop::ClippingSummary& clipping);

    // 목적: 조정된 preview의 Final clipping 분석을 기다리는 상태 표시
    // 입력: 없음
    // 출력: 기존 수치를 현재 결과로 오해하지 않도록 양쪽 indicator를 대기 표시로 갱신
    void setClippingAnalysisPending();

    // 목적: 현재 histogram 표시 내용을 제거
    // 입력: 없음
    // 출력: widget repaint 예약
    void clearHistogram();

    // 목적: clipping 분석이 없는 초기 indicator 상태로 복원
    // 입력: 없음
    // 출력: 양쪽 indicator를 비활성 `—` 표시로 갱신
    void clearClippingSummary();

protected:
    // 목적: 현재 histogram channel을 widget 영역에 선 그래프로 렌더링
    // 입력: event: Qt paint event
    // 출력: 없음
    void paintEvent(QPaintEvent* event) override;

private:
    // 목적: 현재 clipping summary를 고정형 Shadow/Highlight indicator에 투영
    // 입력: 없음
    // 출력: text, tooltip, palette와 clippingActive property 갱신
    void updateClippingIndicators();

    core::develop::ImageHistogram m_histogram;
    core::develop::ClippingSummary m_clipping;
    QLabel* m_shadowClippingIndicator{nullptr};
    QLabel* m_highlightClippingIndicator{nullptr};
    bool m_hasHistogram{false};
    bool m_hasClipping{false};
    bool m_clippingAnalysisPending{false};
};

}  // namespace flexraw::ui::editor
