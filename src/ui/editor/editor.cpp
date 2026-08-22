#include <algorithm>

#include <QPaintEvent>
#include <QPainter>
#include <QPixmap>
#include <QResizeEvent>
#include <QScopedValueRollback>

#include "preview_widget.h"

namespace flexraw::ui::editor
{

// 목적: 중앙 preview 영역 표시를 위한 widget 초기화
// 입력: parent: Qt 부모 widget
// 출력: 초기화된 PreviewWidget 객체
PreviewWidget::PreviewWidget(QWidget* parent) : QLabel(parent)
{
    setAlignment(Qt::AlignCenter);
    setMinimumSize(320, 240);
    setWordWrap(true);
}

// 목적: 표시할 preview 이미지를 저장하고 widget 크기에 맞춰 렌더링
// 입력: image: 표시할 QImage 값
// 출력: 없음
void PreviewWidget::showPreview(const QImage& image)
{
    m_image = image;
    setText({});
    updatePreviewPixmap();
}

// 목적: preview 대신 사용자에게 상태 또는 오류 메시지 표시
// 입력: message: 표시할 user-facing 메시지
// 출력: 없음
void PreviewWidget::showMessage(const QString& message)
{
    m_image = {};
    clearClippingSummary();
    setPixmap({});
    setText(message);
}

// 목적: 현재 preview의 clipping 결과를 모서리 overlay로 갱신
// 입력: clipping: shadow와 highlight clipping pixel 수
// 출력: widget repaint 예약
void PreviewWidget::setClippingSummary(const core::develop::ClippingSummary& clipping)
{
    m_clipping = clipping;
    update();
}

// 목적: 현재 preview의 clipping overlay를 제거
// 입력: 없음
// 출력: widget repaint 예약
void PreviewWidget::clearClippingSummary()
{
    m_clipping = {};
    update();
}

// 목적: clipping overlay의 투명도를 갱신
// 입력: opacity: 0~1 범위의 overlay alpha 값
// 출력: widget repaint 예약
void PreviewWidget::setClippingOpacity(qreal opacity)
{
    m_clippingOpacity = std::clamp(opacity, 0.0, 1.0);
    update();
}

// 목적: widget 크기 변경 시 현재 preview 이미지를 다시 scale
// 입력: event: Qt resize event
// 출력: 없음
void PreviewWidget::resizeEvent(QResizeEvent* event)
{
    QLabel::resizeEvent(event);
    updatePreviewPixmap();
    emit viewportSizeChanged(event->size());
}

// 목적: QLabel preview 위에 clipping overlay를 표시
// 입력: event: Qt paint event
// 출력: 없음
void PreviewWidget::paintEvent(QPaintEvent* event)
{
    QLabel::paintEvent(event);

    constexpr int OverlaySize = 12;
    constexpr int OverlayMargin = 10;
    QPainter painter(this);

    if (m_clipping.shadowPixelCount > 0)
    {
        QColor shadowColor(56, 116, 214);
        shadowColor.setAlphaF(m_clippingOpacity);
        painter.fillRect(OverlayMargin, OverlayMargin, OverlaySize, OverlaySize, shadowColor);
    }

    if (m_clipping.highlightPixelCount > 0)
    {
        QColor highlightColor(225, 73, 67);
        highlightColor.setAlphaF(m_clippingOpacity);
        painter.fillRect(
            width() - OverlayMargin - OverlaySize, OverlayMargin, OverlaySize, OverlaySize, highlightColor);
    }
}

// 목적: 저장된 preview 이미지를 현재 widget 크기에 맞게 표시
// 입력: 없음
// 출력: 없음
void PreviewWidget::updatePreviewPixmap()
{
    if (m_isUpdatingPixmap || m_image.isNull() || size().isEmpty())
    {
        return;
    }

    QScopedValueRollback<bool> updatingPixmap(m_isUpdatingPixmap, true);
    setPixmap(QPixmap::fromImage(m_image.scaled(size(), Qt::KeepAspectRatio, Qt::SmoothTransformation)));
}

}  // namespace flexraw::ui::editor
