#include "standard_preview_worker.h"

#include "thumbnail_preview.h"

#include <QMetaType>

#include <utility>

namespace flexraw::core::preview {

// 목적: standard preview worker의 Qt object 초기화
// 입력: parent: Qt 부모 object
// 출력: 초기화된 worker 객체
StandardPreviewWorker::StandardPreviewWorker(QObject* parent)
    : QObject(parent)
{
    qRegisterMetaType<types::ErrorCode>("flexraw::core::types::ErrorCode");
}

// 목적: 지정 request ID 이하의 표준 preview 결과 publish를 취소
// 입력: requestId: 취소할 마지막 request ID
// 출력: 없음
void StandardPreviewWorker::cancelRequestsThrough(quint64 requestId) noexcept
{
    quint64 cancelledThrough = m_cancelledThrough.load(std::memory_order_relaxed);

    while (cancelledThrough < requestId
           && !m_cancelledThrough.compare_exchange_weak(
               cancelledThrough,
               requestId,
               std::memory_order_release,
               std::memory_order_relaxed)) {
    }
}

// 목적: RAW 파일의 standard preview를 동기 decode 후 signal로 전달
// 입력: requestId: 단조 증가 요청 식별자, filePath: RAW 파일 경로, targetSize: 최대 preview 크기
// 출력: 없음
void StandardPreviewWorker::generateStandardPreview(quint64 requestId, const QString& filePath, const QSize& targetSize)
{
    if (isCancelled(requestId)) {
        emit standardPreviewCancelled(requestId, filePath);
        return;
    }

    const ThumbnailPreviewResult result = loadStandardRawPreview(filePath, targetSize);

    if (isCancelled(requestId)) {
        emit standardPreviewCancelled(requestId, filePath);
        return;
    }

    if (result.hasError()) {
        emit standardPreviewFailed(requestId, filePath, result.error().code, result.error().message);
        return;
    }

    emit standardPreviewReady(requestId, filePath, result.value());
}

// 목적: request ID가 취소 범위에 포함되는지 확인
// 입력: requestId: 확인할 요청 식별자
// 출력: 결과 publish를 중단해야 하면 true
bool StandardPreviewWorker::isCancelled(quint64 requestId) const noexcept
{
    return requestId <= m_cancelledThrough.load(std::memory_order_acquire);
}

} // namespace flexraw::core::preview
