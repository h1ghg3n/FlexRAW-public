#pragma once

#include "file_types.h"
#include "preview_cache.h"
#include "thumbnail_preview.h"

#include <QImage>
#include <QObject>
#include <QSize>
#include <QString>
#include <QThread>

namespace flexraw::core::preview {

class StandardPreviewWorker;

class ProgressivePreviewController final : public QObject
{
    Q_OBJECT

public:
    // 목적: thumbnail과 standard preview 요청을 조정하는 controller 초기화
    // 입력: cacheRoot: preview cache root directory, parent: Qt 부모 object
    // 출력: 초기화된 controller 객체
    explicit ProgressivePreviewController(QString cacheRoot, QObject* parent = nullptr);

    // 목적: worker thread를 중단하고 controller가 소유한 resource 정리
    // 입력: 없음
    // 출력: 없음
    ~ProgressivePreviewController() override;

    // 목적: 선택 파일의 thumbnail을 즉시 전달하고 RAW standard preview를 background 요청
    // 입력: file: preview를 만들 파일 정보, targetSize: 최대 preview 크기
    // 출력: 없음
    void requestPreview(const types::FileDescriptor& file, const QSize& targetSize);

    // 목적: 현재 preview 요청과 이전 queued standard preview 결과를 취소
    // 입력: 없음
    // 출력: 없음
    void cancelPreview();

signals:
    // 목적: immediate thumbnail 또는 raster preview 결과를 전달
    // 입력: filePath: 원본 파일 경로, displayName: 표시 파일명, image: 생성된 preview
    // 출력: 없음
    void thumbnailPreviewReady(const QString& filePath, const QString& displayName, const QImage& image);

    // 목적: background standard RAW preview 결과를 전달
    // 입력: filePath: 원본 파일 경로, displayName: 표시 파일명, image: 생성된 preview
    // 출력: 없음
    void standardPreviewReady(const QString& filePath, const QString& displayName, const QImage& image);

    // 목적: immediate preview를 생성할 수 없을 때 오류를 전달
    // 입력: filePath: 원본 파일 경로, displayName: 표시 파일명, code: 오류 분류, message: 오류 설명
    // 출력: 없음
    void thumbnailPreviewFailed(
        const QString& filePath,
        const QString& displayName,
        types::ErrorCode code,
        const QString& message);

private:
    // 목적: cache를 우선 조회해 thumbnail 또는 raster preview 생성
    // 입력: file: preview를 만들 파일 정보, targetSize: 최대 preview 크기
    // 출력: QImage 값 또는 구조화된 오류
    [[nodiscard]] ThumbnailPreviewResult loadThumbnailPreview(
        const types::FileDescriptor& file,
        const QSize& targetSize);

    // 목적: standard preview worker 결과가 현재 선택 요청에 해당하는지 확인
    // 입력: requestId: 확인할 요청 식별자, filePath: 확인할 원본 파일 경로
    // 출력: 현재 요청이면 true
    [[nodiscard]] bool isCurrentRequest(quint64 requestId, const QString& filePath) const;

    // 목적: worker가 생성한 standard preview를 cache에 저장하고 현재 요청에 전달
    // 입력: requestId: 완료된 요청 식별자, filePath: 원본 파일 경로, image: 생성된 preview
    // 출력: 없음
    void handleStandardPreviewReady(quint64 requestId, const QString& filePath, const QImage& image);

    PreviewCache m_cache;
    QThread m_workerThread;
    StandardPreviewWorker* m_standardWorker{nullptr};
    types::FileDescriptor m_currentFile;
    QSize m_currentTargetSize;
    quint64 m_currentRequestId{0};
};

} // namespace flexraw::core::preview
