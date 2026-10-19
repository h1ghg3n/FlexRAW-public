#include <algorithm>
#include <cmath>

#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QResizeEvent>
#include <QScopedValueRollback>
#include <QWheelEvent>

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
    setMouseTracking(true);
    setToolTip(tr("When comparing, hold the right mouse button to view Before."));
}

// 목적: 표시할 preview 이미지를 저장하고 widget 크기에 맞춰 렌더링
// 입력: image: 표시할 QImage 값
// 출력: 없음
void PreviewWidget::showPreview(const QImage& image)
{
    if (m_image.isNull())
    {
        m_fitToWindow = true;
        m_panOffset = {};
    }
    else if (!m_fitToWindow && !image.isNull())
    {
        m_panOffset.setX(m_panOffset.x() * image.width() / m_image.width());
        m_panOffset.setY(m_panOffset.y() * image.height() / m_image.height());
    }
    m_image = image;
    constrainPan();
    setText({});
    updatePreviewPixmap();
}

// 목적: preview 대신 사용자에게 상태 또는 오류 메시지 표시
// 입력: message: 표시할 user-facing 메시지
// 출력: 없음
void PreviewWidget::showMessage(const QString& message)
{
    m_image = {};
    m_comparisonImage = {};
    m_comparisonMode = ComparisonDisplayMode::After;
    m_isDraggingSplit = false;
    m_isHoveringSplit = false;
    m_isHoldingBefore = false;
    m_fitToWindow = true;
    m_panOffset = {};
    m_isPanning = false;
    unsetCursor();
    setPixmap({});
    setText(message);
}

// 목적: 현재 Preview frame을 widget 전체에 맞게 표시하고 Pan을 초기화
// 입력: 없음
// 출력: 없음
void PreviewWidget::fitToWindow()
{
    if (m_image.isNull())
    {
        return;
    }
    m_fitToWindow = true;
    m_panOffset = {};
    updatePreviewPixmap();
}

// 목적: Preview frame 픽셀을 화면의 물리 픽셀과 1:1로 표시
// 입력: 없음
// 출력: 없음
void PreviewWidget::showPreviewPixelsAt100Percent()
{
    if (m_image.isNull())
    {
        return;
    }
    m_fitToWindow = false;
    m_zoomScale = 1.0 / devicePixelRatioF();
    m_panOffset = {};
    updatePreviewPixmap();
}

// 목적: 현재 표시 배율을 중심점 기준으로 한 단계 확대
// 입력: 없음
// 출력: 없음
void PreviewWidget::zoomIn()
{
    zoomBy(1.25, QRectF(rect()).center());
}

// 목적: 현재 표시 배율을 중심점 기준으로 한 단계 축소
// 입력: 없음
// 출력: 없음
void PreviewWidget::zoomOut()
{
    zoomBy(1.0 / 1.25, QRectF(rect()).center());
}

// 목적: 같은 사진의 기본 Develop Preview를 비교용 frame으로 저장
// 입력: image: owner가 전달한 Before frame
// 출력: 같은 Fit/Zoom/Pan 변환으로 화면 갱신
void PreviewWidget::setComparisonPreview(const QImage& image)
{
    m_comparisonImage = image;
    updatePreviewPixmap();
}

// 목적: 비교 frame과 pointer 상태 폐기
// 입력: 없음
// 출력: After frame 표시 유지
void PreviewWidget::clearComparisonPreview()
{
    m_comparisonImage = {};
    m_isDraggingSplit = false;
    m_isHoveringSplit = false;
    m_isHoldingBefore = false;
    updatePreviewPixmap();
}

// 목적: After·Before·분할 표시 mode 선택
// 입력: mode: Qt view에만 속한 표시 상태
// 출력: 화면 갱신
void PreviewWidget::setComparisonDisplayMode(ComparisonDisplayMode mode)
{
    m_comparisonMode = mode;
    if (mode != ComparisonDisplayMode::Split)
    {
        m_isDraggingSplit = false;
        m_isHoveringSplit = false;
    }
    if (mode == ComparisonDisplayMode::After)
    {
        m_isHoldingBefore = false;
    }
    updatePreviewPixmap();
}

// 목적: widget 크기 변경 시 현재 preview 이미지를 다시 scale
// 입력: event: Qt resize event
// 출력: 없음
void PreviewWidget::resizeEvent(QResizeEvent* event)
{
    QLabel::resizeEvent(event);
    constrainPan();
    updatePreviewPixmap();
    emit viewportSizeChanged(event->size());
}

// 목적: 확대된 Preview frame을 왼쪽 버튼으로 Pan 시작
// 입력: event: pointer 위치와 버튼
// 출력: 없음
void PreviewWidget::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::RightButton && !m_comparisonImage.isNull() &&
        m_comparisonMode != ComparisonDisplayMode::After)
    {
        m_isHoldingBefore = true;
        updatePreviewPixmap();
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton && m_comparisonMode == ComparisonDisplayMode::Split &&
        !m_comparisonImage.isNull() && std::abs(event->position().x() - width() * m_splitFraction) <= 8)
    {
        m_isDraggingSplit = true;
        updatePanCursor();
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton && cursor().shape() == Qt::OpenHandCursor)
    {
        m_isPanning = true;
        m_lastPanPosition = event->position();
        updatePanCursor();
        event->accept();
        return;
    }
    QLabel::mousePressEvent(event);
}

// 목적: 드래그 이동량만큼 Preview frame의 표시 위치 갱신
// 입력: event: pointer 위치와 버튼 상태
// 출력: 없음
void PreviewWidget::mouseMoveEvent(QMouseEvent* event)
{
    if (m_isDraggingSplit && (event->buttons() & Qt::LeftButton))
    {
        m_splitFraction = std::clamp(event->position().x() / width(), 0.0, 1.0);
        updatePreviewPixmap();
        event->accept();
        return;
    }
    m_isHoveringSplit = m_comparisonMode == ComparisonDisplayMode::Split && !m_comparisonImage.isNull() &&
                        std::abs(event->position().x() - width() * m_splitFraction) <= 8;
    updatePanCursor();
    if (m_isPanning && (event->buttons() & Qt::LeftButton))
    {
        m_panOffset += event->position() - m_lastPanPosition;
        m_lastPanPosition = event->position();
        constrainPan();
        updatePreviewPixmap();
        event->accept();
        return;
    }
    QLabel::mouseMoveEvent(event);
}

// 목적: 왼쪽 버튼 해제 시 Preview Pan 종료
// 입력: event: pointer 버튼 상태
// 출력: 없음
void PreviewWidget::mouseReleaseEvent(QMouseEvent* event)
{
    if (m_isHoldingBefore && event->button() == Qt::RightButton)
    {
        m_isHoldingBefore = false;
        updatePreviewPixmap();
        event->accept();
        return;
    }
    if (m_isDraggingSplit && event->button() == Qt::LeftButton)
    {
        m_isDraggingSplit = false;
        updatePanCursor();
        event->accept();
        return;
    }
    if (m_isPanning && event->button() == Qt::LeftButton)
    {
        m_isPanning = false;
        updatePanCursor();
        event->accept();
        return;
    }
    QLabel::mouseReleaseEvent(event);
}

// 목적: pointer 위치를 고정한 채 휠로 Preview frame 확대 또는 축소
// 입력: event: 휠 위치와 회전량
// 출력: 없음
void PreviewWidget::wheelEvent(QWheelEvent* event)
{
    const qreal delta = event->angleDelta().y() != 0 ? event->angleDelta().y() : event->pixelDelta().y();
    if (m_image.isNull() || delta == 0)
    {
        event->ignore();
        return;
    }
    zoomBy(std::pow(1.25, delta / 120.0), event->position());
    event->accept();
}

// 목적: widget 안에 전체 Preview frame이 들어가는 표시 배율 계산
// 입력: 없음
// 출력: 논리 pixel 단위의 Fit 배율
qreal PreviewWidget::fitScale() const
{
    if (m_image.isNull() || size().isEmpty())
    {
        return 1.0;
    }
    return std::min(qreal(width()) / m_image.width(), qreal(height()) / m_image.height());
}

// 목적: 현재 Fit 또는 사용자 지정 Preview frame 표시 배율 반환
// 입력: 없음
// 출력: 논리 pixel 단위의 표시 배율
qreal PreviewWidget::displayScale() const
{
    return m_fitToWindow ? fitScale() : m_zoomScale;
}

// 목적: 확대된 frame의 중심 오프셋을 빈 영역이 나타나지 않는 범위로 제한
// 입력: 없음
// 출력: 제한된 m_panOffset
void PreviewWidget::constrainPan()
{
    if (m_image.isNull())
    {
        m_panOffset = {};
        return;
    }
    const qreal horizontalTravel = std::max<qreal>(0.0, (m_image.width() * displayScale() - width()) / 2.0);
    const qreal verticalTravel = std::max<qreal>(0.0, (m_image.height() * displayScale() - height()) / 2.0);
    m_panOffset.setX(std::clamp(m_panOffset.x(), -horizontalTravel, horizontalTravel));
    m_panOffset.setY(std::clamp(m_panOffset.y(), -verticalTravel, verticalTravel));
}

// 목적: anchor 아래의 Preview frame 위치를 유지하면서 표시 배율 갱신
// 입력: factor: 현재 배율에 곱할 값, anchor: widget 좌표
// 출력: 없음
void PreviewWidget::zoomBy(qreal factor, const QPointF& anchor)
{
    if (m_image.isNull())
    {
        return;
    }
    const qreal oldScale = displayScale();
    const qreal pixelScale = 1.0 / devicePixelRatioF();
    const qreal scale =
        std::clamp(oldScale * factor, std::min(fitScale(), pixelScale) / 16.0, std::max(fitScale(), pixelScale) * 16.0);
    const QPointF center = QRectF(rect()).center();
    m_panOffset = (anchor - center) - (anchor - center - m_panOffset) * (scale / oldScale);
    m_zoomScale = scale;
    m_fitToWindow = false;
    constrainPan();
    updatePreviewPixmap();
}

// 목적: Pan 가능 여부에 따라 pointer 모양 갱신
// 입력: 없음
// 출력: 없음
void PreviewWidget::updatePanCursor()
{
    if (m_isDraggingSplit || m_isHoveringSplit)
    {
        setCursor(Qt::SplitHCursor);
        return;
    }
    if (m_image.isNull() ||
        (m_image.width() * displayScale() <= width() && m_image.height() * displayScale() <= height()))
    {
        unsetCursor();
        return;
    }
    setCursor(m_isPanning ? Qt::ClosedHandCursor : Qt::OpenHandCursor);
}

// 목적: 저장된 Preview frame을 Fit/Zoom/Pan 상태로 viewport에 그려 표시
// 입력: 없음
// 출력: 없음
void PreviewWidget::updatePreviewPixmap()
{
    if (m_isUpdatingPixmap || m_image.isNull() || size().isEmpty())
    {
        return;
    }

    QScopedValueRollback<bool> updatingPixmap(m_isUpdatingPixmap, true);
    const qreal ratio = devicePixelRatioF();
    QPixmap canvas((QSizeF(size()) * ratio).toSize());
    canvas.setDevicePixelRatio(ratio);
    canvas.fill(palette().color(QPalette::Window));
    QPainter painter(&canvas);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    const qreal scale = displayScale();
    const QSizeF imageSize(m_image.width() * scale, m_image.height() * scale);
    const QRectF target(QRectF(rect()).center() - QPointF(imageSize.width() / 2, imageSize.height() / 2) + m_panOffset,
                        imageSize);
    painter.drawImage(target, m_image);
    if (!m_comparisonImage.isNull() && m_comparisonMode != ComparisonDisplayMode::After)
    {
        painter.save();
        if (m_comparisonMode == ComparisonDisplayMode::Split && !m_isHoldingBefore)
        {
            painter.setClipRect(QRectF(0, 0, width() * m_splitFraction, height()));
        }
        painter.drawImage(target, m_comparisonImage);
        painter.restore();
        if (m_comparisonMode == ComparisonDisplayMode::Split && !m_isHoldingBefore)
        {
            painter.fillRect(QRectF(width() * m_splitFraction - 1, 0, 2, height()), Qt::white);
        }
    }
    painter.end();
    setPixmap(canvas);
    updatePanCursor();
}

}  // namespace flexraw::ui::editor
