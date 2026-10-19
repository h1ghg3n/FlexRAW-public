#include "histogram_widget.h"

#include <algorithm>

#include <QHBoxLayout>
#include <QLabel>
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

    m_shadowClippingIndicator = new QLabel(this);
    m_shadowClippingIndicator->setObjectName(QStringLiteral("shadowClippingIndicator"));
    m_shadowClippingIndicator->setAccessibleName(tr("Shadow clipping"));
    m_highlightClippingIndicator = new QLabel(this);
    m_highlightClippingIndicator->setObjectName(QStringLiteral("highlightClippingIndicator"));
    m_highlightClippingIndicator->setAccessibleName(tr("Highlight clipping"));
    for (QLabel* indicator : {m_shadowClippingIndicator, m_highlightClippingIndicator})
    {
        indicator->setAlignment(Qt::AlignCenter);
        indicator->setAutoFillBackground(true);
        indicator->setFrameShape(QFrame::StyledPanel);
        indicator->setFixedHeight(20);
        indicator->setMinimumWidth(58);
    }

    auto* indicatorLayout = new QHBoxLayout(this);
    indicatorLayout->setContentsMargins(4, 4, 4, 0);
    indicatorLayout->setAlignment(Qt::AlignTop);
    indicatorLayout->addWidget(m_shadowClippingIndicator);
    indicatorLayout->addStretch();
    indicatorLayout->addWidget(m_highlightClippingIndicator);
    updateClippingIndicators();
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

// 목적: histogram 양쪽 indicator에 shadow/highlight clipping 비율 표시
// 입력: clipping: shadow/highlight/전체 preview pixel 수
// 출력: indicator text, tooltip과 활성 색상 갱신
void HistogramWidget::setClippingSummary(const core::develop::ClippingSummary& clipping)
{
    m_clipping = clipping;
    m_hasClipping = clipping.pixelCount > 0;
    m_clippingAnalysisPending = false;
    updateClippingIndicators();
}

// 목적: 조정된 preview의 Final clipping 분석을 기다리는 상태 표시
// 입력: 없음
// 출력: 기존 수치를 현재 결과로 오해하지 않도록 양쪽 indicator를 대기 표시로 갱신
void HistogramWidget::setClippingAnalysisPending()
{
    m_clippingAnalysisPending = true;
    updateClippingIndicators();
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

// 목적: clipping 분석이 없는 초기 indicator 상태로 복원
// 입력: 없음
// 출력: 양쪽 indicator를 비활성 `—` 표시로 갱신
void HistogramWidget::clearClippingSummary()
{
    m_clipping = {};
    m_hasClipping = false;
    m_clippingAnalysisPending = false;
    updateClippingIndicators();
}

// 목적: 현재 clipping summary를 고정형 Shadow/Highlight indicator에 투영
// 입력: 없음
// 출력: text, tooltip, palette와 clippingActive property 갱신
void HistogramWidget::updateClippingIndicators()
{
    const auto percentageText = [this](quint64 count) {
        const double percentage = 100.0 * static_cast<double>(count) / static_cast<double>(m_clipping.pixelCount);
        if (count > 0 && percentage < 0.1)
        {
            return tr("<0.1%");
        }
        return tr("%1%").arg(QString::number(percentage, 'f', 1));
    };
    const auto updateIndicator = [this](QLabel* indicator, bool active, const QColor& activeColor) {
        QPalette indicatorPalette = palette();
        indicatorPalette.setColor(QPalette::Window, active ? activeColor : palette().color(QPalette::AlternateBase));
        indicatorPalette.setColor(QPalette::WindowText,
                                  active ? QColor(Qt::white) : palette().color(QPalette::Disabled, QPalette::Text));
        indicator->setPalette(indicatorPalette);
        indicator->setStyleSheet(active ? QStringLiteral("QLabel { background-color: %1; color: white; border: 1px "
                                                         "solid %1; }")
                                              .arg(activeColor.name(QColor::HexRgb))
                                        : QString{});
        indicator->setProperty("clippingActive", active);
    };

    if (m_clippingAnalysisPending)
    {
        m_shadowClippingIndicator->setText(tr("S …"));
        m_highlightClippingIndicator->setText(tr("H …"));
        m_shadowClippingIndicator->setToolTip(tr("Shadow clipping analysis is updating."));
        m_highlightClippingIndicator->setToolTip(tr("Highlight clipping analysis is updating."));
        updateIndicator(m_shadowClippingIndicator, false, {});
        updateIndicator(m_highlightClippingIndicator, false, {});
        return;
    }

    if (!m_hasClipping)
    {
        m_shadowClippingIndicator->setText(tr("S —"));
        m_highlightClippingIndicator->setText(tr("H —"));
        m_shadowClippingIndicator->setToolTip(tr("Shadow clipping analysis is unavailable."));
        m_highlightClippingIndicator->setToolTip(tr("Highlight clipping analysis is unavailable."));
        updateIndicator(m_shadowClippingIndicator, false, {});
        updateIndicator(m_highlightClippingIndicator, false, {});
        return;
    }

    const QString shadowPercentage = percentageText(m_clipping.shadowPixelCount);
    const QString highlightPercentage = percentageText(m_clipping.highlightPixelCount);
    m_shadowClippingIndicator->setText(tr("S %1").arg(shadowPercentage));
    m_highlightClippingIndicator->setText(tr("H %1").arg(highlightPercentage));
    m_shadowClippingIndicator->setToolTip(
        tr("Shadow clipping: %1 pixels (%2).").arg(m_clipping.shadowPixelCount).arg(shadowPercentage));
    m_highlightClippingIndicator->setToolTip(
        tr("Highlight clipping: %1 pixels (%2).").arg(m_clipping.highlightPixelCount).arg(highlightPercentage));
    updateIndicator(m_shadowClippingIndicator, m_clipping.shadowPixelCount > 0, QColor(56, 116, 214));
    updateIndicator(m_highlightClippingIndicator, m_clipping.highlightPixelCount > 0, QColor(225, 73, 67));
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
    constexpr qreal IndicatorBandHeight = 24.0;
    const QRectF bounds = rect().adjusted(Margin, IndicatorBandHeight + Margin, -Margin, -Margin);
    const quint32 maximum = findMaximumBin(m_histogram);
    drawHistogramChannel(painter, m_histogram.luminance, bounds, maximum, palette().color(QPalette::Text));
    drawHistogramChannel(painter, m_histogram.red, bounds, maximum, QColor(220, 72, 72));
    drawHistogramChannel(painter, m_histogram.green, bounds, maximum, QColor(67, 150, 89));
    drawHistogramChannel(painter, m_histogram.blue, bounds, maximum, QColor(76, 122, 210));
}

}  // namespace flexraw::ui::editor
