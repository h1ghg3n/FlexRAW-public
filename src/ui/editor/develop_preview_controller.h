#pragma once

#include <QFutureWatcher>
#include <QImage>
#include <QObject>
#include <QTimer>

#include "clipping.h"
#include "develop.h"
#include "histogram.h"

class QString;

namespace flexraw::ui::editor
{

struct DevelopPreviewRender
{
    QImage image;
    core::develop::ImageHistogram histogram;
    core::develop::ClippingSummary clipping;
};

using DevelopPreviewRenderResult = core::types::Result<DevelopPreviewRender, core::types::CoreError>;

class DevelopPreviewController : public QObject
{
    Q_OBJECT

public:
    // 목적: preview 보정 상태를 관리하는 controller 초기화
    // 입력: parent: Qt 부모 object
    // 출력: 초기화된 DevelopPreviewController 객체
    explicit DevelopPreviewController(QObject* parent = nullptr);

    // 목적: 현상 적용 전 원본 preview를 갱신
    // 입력: sourceImage: RAW 또는 raster decoder가 생성한 preview
    // 출력: previewReady 또는 previewFailed signal 발생
    void setSourceImage(QImage sourceImage);

    // 목적: 현재 source preview를 제거해 새 파일 loading 상태로 전환
    // 입력: 없음
    // 출력: 없음
    void clearSourceImage();

    // 목적: preview에 적용할 develop parameter를 갱신
    // 입력: params: 적용할 develop parameter 값
    // 출력: source가 있으면 previewReady 또는 previewFailed signal 발생
    void setParams(const core::types::DevelopParams& params);

signals:
    // 목적: 현상된 display preview를 widget에 전달
    // 입력: image: 적용 완료된 preview 이미지, histogram: channel histogram, clipping: clipping 분석 결과
    // 출력: 없음
    void previewReady(const QImage& image,
                      const core::develop::ImageHistogram& histogram,
                      const core::develop::ClippingSummary& clipping);

    // 목적: preview 현상 실패를 조립 계층에 전달
    // 입력: message: 실패 원인 설명
    // 출력: 없음
    void previewFailed(const QString& message);

private:
    // 목적: 최신 source image와 parameter에 대한 background preview 생성을 예약
    // 입력: 없음
    // 출력: debounce 후 background preview 작업 시작
    void schedulePreview();

    // 목적: 예약된 최신 source image와 parameter로 background preview 생성 시작
    // 입력: 없음
    // 출력: 작업 완료 시 handlePreviewFinished 호출
    void startPreview();

    // 목적: worker thread에서 develop image와 그 histogram을 함께 계산
    // 입력: sourceImage: 현상 전 preview, params: 적용할 develop parameter 값
    // 출력: 현상 image와 histogram 또는 구조화된 오류
    [[nodiscard]] static DevelopPreviewRenderResult renderPreview(const QImage& sourceImage,
                                                                  const core::types::DevelopParams& params);

    // 목적: 완료된 background preview가 최신 요청이면 UI signal로 전달
    // 입력: 없음
    // 출력: previewReady 또는 previewFailed signal 발생 가능
    void handlePreviewFinished();

    QImage m_sourceImage;
    core::types::DevelopParams m_params;
    QFutureWatcher<DevelopPreviewRenderResult> m_previewWatcher;
    QTimer m_debounceTimer;
    quint64 m_currentRequestId{0};
    quint64 m_activeRequestId{0};
};

}  // namespace flexraw::ui::editor
