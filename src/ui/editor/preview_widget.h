#pragma once

#include <QImage>
#include <QLabel>
#include <QSize>

#include "clipping.h"

namespace flexraw::ui::editor
{

class PreviewWidget : public QLabel
{
    Q_OBJECT

public:
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

    // 목적: 현재 preview의 clipping 결과를 모서리 overlay로 갱신
    // 입력: clipping: shadow와 highlight clipping pixel 수
    // 출력: widget repaint 예약
    void setClippingSummary(const core::develop::ClippingSummary& clipping);

    // 목적: 현재 preview의 clipping overlay를 제거
    // 입력: 없음
    // 출력: widget repaint 예약
    void clearClippingSummary();

    // 목적: clipping overlay의 투명도를 갱신
    // 입력: opacity: 0~1 범위의 overlay alpha 값
    // 출력: widget repaint 예약
    void setClippingOpacity(qreal opacity);

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

    // 목적: QLabel preview 위에 clipping overlay를 표시
    // 입력: event: Qt paint event
    // 출력: 없음
    void paintEvent(QPaintEvent* event) override;

private:
    // 목적: 저장된 preview 이미지를 현재 widget 크기에 맞게 표시
    // 입력: 없음
    // 출력: 없음
    void updatePreviewPixmap();

    QImage m_image;
    core::develop::ClippingSummary m_clipping;
    qreal m_clippingOpacity{0.55};
    bool m_isUpdatingPixmap{false};
};

}  // namespace flexraw::ui::editor
