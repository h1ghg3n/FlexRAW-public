#pragma once

#include <QWidget>

#include "histogram.h"

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

    // 목적: 현재 histogram 표시 내용을 제거
    // 입력: 없음
    // 출력: widget repaint 예약
    void clearHistogram();

protected:
    // 목적: 현재 histogram channel을 widget 영역에 선 그래프로 렌더링
    // 입력: event: Qt paint event
    // 출력: 없음
    void paintEvent(QPaintEvent* event) override;

private:
    core::develop::ImageHistogram m_histogram;
    bool m_hasHistogram{false};
};

}  // namespace flexraw::ui::editor
