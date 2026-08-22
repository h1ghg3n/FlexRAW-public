#include "progressive_preview_controller.h"

#include "log.h"
#include "standard_preview_worker.h"
#include "thumbnail_preview.h"

#include <QMetaObject>

#include <utility>

namespace flexraw::core::preview {

// 목적: thumbnail과 standard preview 요청을 조정하는 controller 초기화
// 입력: cacheRoot: preview cache root directory, parent: Qt 부모 object
// 출력: 초기화된 controller 객체
ProgressivePreviewController::ProgressivePreviewController(QString cacheRoot, QObject* parent)
    : QObject(parent)
    , m_cache(std::move(cacheRoot))
    , m_standardWorker(new StandardPreviewWorker)
{
    m_standardWorker->moveToThread(&m_workerThread);
    connect(&m_workerThread, &QThread::finished, m_standardWorker, &QObject::deleteLater);
    connect(
        m_standardWorker,
        &StandardPreviewWorker::standardPreviewReady,
        this,
        &ProgressivePreviewController::handleStandardPreviewReady);
    m_workerThread.start();
}

// 목적: worker thread를 중단하고 controller가 소유한 resource 정리
// 입력: 없음
// 출력: 없음
ProgressivePreviewController::~ProgressivePreviewController()
{
    cancelPreview();
    m_workerThread.quit();
    m_workerThread.wait();
}

// 목적: 선택 파일의 thumbnail을 즉시 전달하고 RAW standard preview를 background 요청
// 입력: file: preview를 만들 파일 정보, targetSize: 최대 preview 크기
// 출력: 없음
void ProgressivePreviewController::requestPreview(const types::FileDescriptor& file, const QSize& targetSize)
{
    cancelPreview();
    ++m_currentRequestId;
    m_currentFile = file;
    m_currentTargetSize = targetSize;

    const ThumbnailPreviewResult thumbnail = loadThumbnailPreview(file, targetSize);

    if (thumbnail.hasValue()) {
        emit thumbnailPreviewReady(file.path, file.displayName, thumbnail.value());
    } else {
        LOG_WARN(
            "preview",
            "Thumbnail preview failed: {}",
            thumbnail.error().message.toStdString());
        emit thumbnailPreviewFailed(file.path, file.displayName, thumbnail.error().code, thumbnail.error().message);
    }

    if (file.kind != types::SupportedFileKind::Raw) {
        return;
    }

    const PreviewCacheLookupResult cachedStandard = m_cache.load(file.path, PreviewCacheTier::Standard, targetSize);

    if (cachedStandard.hasValue() && cachedStandard.value().hit) {
        emit standardPreviewReady(file.path, file.displayName, cachedStandard.value().image);
        return;
    }

    QMetaObject::invokeMethod(
        m_standardWorker,
        "generateStandardPreview",
        Qt::QueuedConnection,
        Q_ARG(quint64, m_currentRequestId),
        Q_ARG(QString, file.path),
        Q_ARG(QSize, targetSize));
}

// 목적: 현재 preview 요청과 이전 queued standard preview 결과를 취소
// 입력: 없음
// 출력: 없음
void ProgressivePreviewController::cancelPreview()
{
    m_standardWorker->cancelRequestsThrough(m_currentRequestId);
}

// 목적: cache를 우선 조회해 thumbnail 또는 raster preview 생성
// 입력: file: preview를 만들 파일 정보, targetSize: 최대 preview 크기
// 출력: QImage 값 또는 구조화된 오류
ThumbnailPreviewResult ProgressivePreviewController::loadThumbnailPreview(
    const types::FileDescriptor& file,
    const QSize& targetSize)
{
    const PreviewCacheLookupResult cachedPreview = m_cache.load(file.path, PreviewCacheTier::Thumbnail, targetSize);

    if (cachedPreview.hasValue() && cachedPreview.value().hit) {
        return ThumbnailPreviewResult::success(cachedPreview.value().image);
    }

    const ThumbnailPreviewResult preview = loadFilePreview(file, targetSize);

    if (preview.hasValue()) {
        (void)m_cache.store(file.path, PreviewCacheTier::Thumbnail, targetSize, preview.value());
    }

    return preview;
}

// 목적: standard preview worker 결과가 현재 선택 요청에 해당하는지 확인
// 입력: requestId: 확인할 요청 식별자, filePath: 확인할 원본 파일 경로
// 출력: 현재 요청이면 true
bool ProgressivePreviewController::isCurrentRequest(quint64 requestId, const QString& filePath) const
{
    return requestId == m_currentRequestId && filePath == m_currentFile.path;
}

// 목적: worker가 생성한 standard preview를 cache에 저장하고 현재 요청에 전달
// 입력: requestId: 완료된 요청 식별자, filePath: 원본 파일 경로, image: 생성된 preview
// 출력: 없음
void ProgressivePreviewController::handleStandardPreviewReady(
    quint64 requestId,
    const QString& filePath,
    const QImage& image)
{
    if (!isCurrentRequest(requestId, filePath)) {
        return;
    }

    (void)m_cache.store(filePath, PreviewCacheTier::Standard, m_currentTargetSize, image);
    emit standardPreviewReady(filePath, m_currentFile.displayName, image);
}

} // namespace flexraw::core::preview
