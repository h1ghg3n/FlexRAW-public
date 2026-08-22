#include "histogram_widget.h"

#include <algorithm>

#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPen>

namespace flexraw::ui::editor
{
namespace
{

// 목적: channel bin 중 가장 큰 빈도를 찾아 정규화 기준 반환
// 입력: histogram: 최대값을 찾을 histogram 값
// 출력: 모든 channel 중 최대 bin 빈도
[[nodiscard]] quint32 findMaximumBin(const core::develop::ImageHistogram& histogram)
{
    quint32 maximum = 0;
    for (const core::develop::HistogramBins* bins :
         {&histogram.red, &histogram.green, &histogram.blue, &histogram.luminance})
    {
        maximum = std::max(maximum, *std::max_element(bins->cbegin(), bins->cend()));
    }

    return maximum;
}

// 목적: 하나의 256-bin histogram channel을 widget 내부에 polyline으로 그림
// 입력: painter: 그리기 대상, bins: channel bin, bounds: 그래프 영역, maximum: 정규화 기준, color: 선 색상
// 출력: 없음
void drawHistogramChannel(QPainter& painter,
                          const core::develop::HistogramBins& bins,
                          const QRectF& bounds,
                          quint32 maximum,
                          const QColor& color)
{
    if (maximum == 0)
    {
        return;
    }

    QPainterPath path;
    for (int index = 0; index < static_cast<int>(bins.size()); ++index)
    {
        const qreal x = bounds.left() + bounds.width() * static_cast<qreal>(index) / (bins.size() - 1);
        const qreal normalizedHeight = static_cast<qreal>(bins[index]) / static_cast<qreal>(maximum);
        const qreal y = bounds.bottom() - bounds.height() * normalizedHeight;
        index == 0 ? path.moveTo(x, y) : path.lineTo(x, y);
    }

    painter.setPen(QPen(color, 1.0));
    painter.drawPath(path);
}

}  // namespace

// 목적: preview histogram을 표시하는 compact widget 초기화
// 입력: parent: Qt 부모 widget
// 출력: 초기화된 HistogramWidget 객체
HistogramWidget::HistogramWidget(QWidget* parent) : QWidget(parent)
{
    setMinimumHeight(110);
    setMaximumHeight(150);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

// 목적: 표시할 preview histogram을 갱신
// 입력: histogram: RGB와 휘도 bin을 포함하는 계산 결과
// 출력: widget repaint 예약
void HistogramWidget::setHistogram(const core::develop::ImageHistogram& histogram)
{
    m_histogram = histogram;
    m_hasHistogram = true;
    update();
}

// 목적: 현재 histogram 표시 내용을 제거
// 입력: 없음
// 출력: widget repaint 예약
void HistogramWidget::clearHistogram()
{
    m_histogram = {};
    m_hasHistogram = false;
    update();
}

// 목적: 현재 histogram channel을 widget 영역에 선 그래프로 렌더링
// 입력: event: Qt paint event
// 출력: 없음
void HistogramWidget::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);

    QPainter painter(this);
    painter.fillRect(rect(), palette().color(QPalette::Base));

    if (!m_hasHistogram)
    {
        return;
    }

    constexpr qreal Margin = 4.0;
    const QRectF bounds = rect().adjusted(Margin, Margin, -Margin, -Margin);
    const quint32 maximum = findMaximumBin(m_histogram);
    drawHistogramChannel(painter, m_histogram.luminance, bounds, maximum, palette().color(QPalette::Text));
    drawHistogramChannel(painter, m_histogram.red, bounds, maximum, QColor(220, 72, 72));
    drawHistogramChannel(painter, m_histogram.green, bounds, maximum, QColor(67, 150, 89));
    drawHistogramChannel(painter, m_histogram.blue, bounds, maximum, QColor(76, 122, 210));
}

}  // namespace flexraw::ui::editor
