#pragma once

#include <QImage>
#include <QLabel>
#include <QPointF>
#include <QSize>

class QMouseEvent;
class QWheelEvent;

namespace flexraw::ui::editor
{

class PreviewWidget : public QLabel
{
    Q_OBJECT

public:
    enum class ComparisonDisplayMode
    {
        After,
        Split,
        Before,
    };

    // 목적: 중앙 preview 영역 표시를 위한 widget 초기화
    // 입력: parent: Qt 부모 widget
    // 출력: 초기화된 PreviewWidget 객체
    explicit PreviewWidget(QWidget* parent = nullptr);

    // 목적: 표시할 preview 이미지를 저장하고 widget 크기에 맞춰 렌더링
    // 입력: image: 표시할 QImage 값
    // 출력: 없음
    void showPreview(const QImage& image);

    // 목적: preview 대신 사용자에게 상태 또는 오류 메시지 표시
    // 입력: message: 표시할 user-facing 메시지
    // 출력: 없음
    void showMessage(const QString& message);

    // 목적: 현재 Preview frame을 widget 전체에 맞게 표시하고 Pan을 초기화
    // 입력: 없음
    // 출력: 없음
    void fitToWindow();

    // 목적: Preview frame 픽셀을 화면의 물리 픽셀과 1:1로 표시
    // 입력: 없음
    // 출력: 없음
    void showPreviewPixelsAt100Percent();

    // 목적: 현재 표시 배율을 중심점 기준으로 한 단계 확대
    // 입력: 없음
    // 출력: 없음
    void zoomIn();

    // 목적: 현재 표시 배율을 중심점 기준으로 한 단계 축소
    // 입력: 없음
    // 출력: 없음
    void zoomOut();

    // 목적: 같은 사진의 기본 Develop Preview를 비교용 frame으로 저장
    // 입력: image: owner가 전달한 Before frame
    // 출력: 같은 Fit/Zoom/Pan 변환으로 화면 갱신
    void setComparisonPreview(const QImage& image);

    // 목적: 비교 frame과 pointer 상태 폐기
    // 입력: 없음
    // 출력: After frame 표시 유지
    void clearComparisonPreview();

    // 목적: After·Before·분할 표시 mode 선택
    // 입력: mode: Qt view에만 속한 표시 상태
    // 출력: 화면 갱신
    void setComparisonDisplayMode(ComparisonDisplayMode mode);

signals:
    // 목적: 실제 preview viewport 크기 변경을 frontend adapter에 전달
    // 입력: size: layout 적용 후 새 widget 크기
    // 출력: 없음
    void viewportSizeChanged(const QSize& size);

protected:
    // 목적: widget 크기 변경 시 현재 preview 이미지를 다시 scale
    // 입력: event: Qt resize event
    // 출력: 없음
    void resizeEvent(QResizeEvent* event) override;

    // 목적: 확대된 Preview frame을 왼쪽 버튼으로 Pan 시작
    // 입력: event: pointer 위치와 버튼
    // 출력: 없음
    void mousePressEvent(QMouseEvent* event) override;

    // 목적: 드래그 이동량만큼 Preview frame의 표시 위치 갱신
    // 입력: event: pointer 위치와 버튼 상태
    // 출력: 없음
    void mouseMoveEvent(QMouseEvent* event) override;

    // 목적: 왼쪽 버튼 해제 시 Preview Pan 종료
    // 입력: event: pointer 버튼 상태
    // 출력: 없음
    void mouseReleaseEvent(QMouseEvent* event) override;

    // 목적: pointer 위치를 고정한 채 휠로 Preview frame 확대 또는 축소
    // 입력: event: 휠 위치와 회전량
    // 출력: 없음
    void wheelEvent(QWheelEvent* event) override;

private:
    // 목적: widget 안에 전체 Preview frame이 들어가는 표시 배율 계산
    // 입력: 없음
    // 출력: 논리 pixel 단위의 Fit 배율
    [[nodiscard]] qreal fitScale() const;

    // 목적: 현재 Fit 또는 사용자 지정 Preview frame 표시 배율 반환
    // 입력: 없음
    // 출력: 논리 pixel 단위의 표시 배율
    [[nodiscard]] qreal displayScale() const;

    // 목적: 확대된 frame의 중심 오프셋을 빈 영역이 나타나지 않는 범위로 제한
    // 입력: 없음
    // 출력: 제한된 m_panOffset
    void constrainPan();

    // 목적: anchor 아래의 Preview frame 위치를 유지하면서 표시 배율 갱신
    // 입력: factor: 현재 배율에 곱할 값, anchor: widget 좌표
    // 출력: 없음
    void zoomBy(qreal factor, const QPointF& anchor);

    // 목적: Pan 가능 여부에 따라 pointer 모양 갱신
    // 입력: 없음
    // 출력: 없음
    void updatePanCursor();

    // 목적: 저장된 Preview frame을 Fit/Zoom/Pan 상태로 viewport에 그려 표시
    // 입력: 없음
    // 출력: 없음
    void updatePreviewPixmap();

    QImage m_image;
    QImage m_comparisonImage;
    qreal m_zoomScale{1.0};
    QPointF m_panOffset;
    QPointF m_lastPanPosition;
    qreal m_splitFraction{0.5};
    ComparisonDisplayMode m_comparisonMode{ComparisonDisplayMode::After};
    bool m_isDraggingSplit{false};
    bool m_isHoveringSplit{false};
    bool m_isHoldingBefore{false};
    bool m_fitToWindow{true};
    bool m_isPanning{false};
    bool m_isUpdatingPixmap{false};
};

}  // namespace flexraw::ui::editor
