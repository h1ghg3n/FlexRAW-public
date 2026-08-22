#include "develop_preview_controller.h"

#include <utility>

#include <QImage>
#include <QtConcurrentRun>

namespace flexraw::ui::editor
{

// 목적: preview 보정 상태를 관리하는 controller 초기화
// 입력: parent: Qt 부모 object
// 출력: 초기화된 DevelopPreviewController 객체
DevelopPreviewController::DevelopPreviewController(QObject* parent) : QObject(parent)
{
    m_debounceTimer.setSingleShot(true);
    m_debounceTimer.setInterval(40);
    connect(&m_debounceTimer, &QTimer::timeout, this, &DevelopPreviewController::startPreview);
    connect(&m_previewWatcher, &QFutureWatcher<DevelopPreviewRenderResult>::finished, this, [this] {
        handlePreviewFinished();
    });
}

// 목적: 현상 적용 전 원본 preview를 갱신
// 입력: sourceImage: RAW 또는 raster decoder가 생성한 preview
// 출력: previewReady 또는 previewFailed signal 발생
void DevelopPreviewController::setSourceImage(QImage sourceImage)
{
    m_sourceImage = std::move(sourceImage);
    schedulePreview();
}

// 목적: 현재 source preview를 제거해 새 파일 loading 상태로 전환
// 입력: 없음
// 출력: 없음
void DevelopPreviewController::clearSourceImage()
{
    ++m_currentRequestId;
    m_debounceTimer.stop();
    m_sourceImage = {};
}

// 목적: preview에 적용할 develop parameter를 갱신
// 입력: params: 적용할 develop parameter 값
// 출력: source가 있으면 previewReady 또는 previewFailed signal 발생
void DevelopPreviewController::setParams(const core::types::DevelopParams& params)
{
    m_params = params;
    schedulePreview();
}

// 목적: 최신 source image와 parameter에 대한 background preview 생성을 예약
// 입력: 없음
// 출력: debounce 후 background preview 작업 시작
void DevelopPreviewController::schedulePreview()
{
    if (m_sourceImage.isNull())
    {
        return;
    }

    ++m_currentRequestId;
    m_debounceTimer.start();
}

// 목적: 예약된 최신 source image와 parameter로 background preview 생성 시작
// 입력: 없음
// 출력: 작업 완료 시 handlePreviewFinished 호출
void DevelopPreviewController::startPreview()
{
    if (m_sourceImage.isNull() || m_previewWatcher.isRunning())
    {
        return;
    }

    m_activeRequestId = m_currentRequestId;
    const QImage sourceImage = m_sourceImage;
    const core::types::DevelopParams params = m_params;
    m_previewWatcher.setFuture(QtConcurrent::run([sourceImage, params] { return renderPreview(sourceImage, params); }));
}

// 목적: worker thread에서 develop image와 그 histogram을 함께 계산
// 입력: sourceImage: 현상 전 preview, params: 적용할 develop parameter 값
// 출력: 현상 image와 histogram 또는 구조화된 오류
DevelopPreviewRenderResult DevelopPreviewController::renderPreview(const QImage& sourceImage,
                                                                   const core::types::DevelopParams& params)
{
    const core::develop::DevelopImageResult developedImage = core::develop::applyDevelop(sourceImage, params);
    if (developedImage.hasError())
    {
        return DevelopPreviewRenderResult::failure(developedImage.error());
    }

    const core::develop::ImageHistogramResult histogram =
        core::develop::calculateImageHistogram(developedImage.value());
    if (histogram.hasError())
    {
        return DevelopPreviewRenderResult::failure(histogram.error());
    }

    const core::develop::ClippingSummaryResult clipping =
        core::develop::calculateClippingSummary(developedImage.value());
    if (clipping.hasError())
    {
        return DevelopPreviewRenderResult::failure(clipping.error());
    }

    return DevelopPreviewRenderResult::success({developedImage.value(), histogram.value(), clipping.value()});
}

// 목적: 완료된 background preview가 최신 요청이면 UI signal로 전달
// 입력: 없음
// 출력: previewReady 또는 previewFailed signal 발생 가능
void DevelopPreviewController::handlePreviewFinished()
{
    const DevelopPreviewRenderResult result = m_previewWatcher.result();
    const bool isLatestRequest = m_activeRequestId == m_currentRequestId && !m_sourceImage.isNull();

    if (isLatestRequest)
    {
        if (result.hasError())
        {
            emit previewFailed(result.error().message);
        }
        else
        {
            emit previewReady(result.value().image, result.value().histogram, result.value().clipping);
        }
    }

    if (m_activeRequestId != m_currentRequestId && !m_sourceImage.isNull())
    {
        m_debounceTimer.start(0);
    }
}

}  // namespace flexraw::ui::editor
