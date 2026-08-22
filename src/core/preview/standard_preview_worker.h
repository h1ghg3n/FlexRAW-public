#pragma once

#include "error.h"

#include <QImage>
#include <QObject>
#include <QSize>
#include <QString>

#include <atomic>

namespace flexraw::core::preview {

class StandardPreviewWorker final : public QObject
{
    Q_OBJECT

public:
    // 목적: standard preview worker의 Qt object 초기화
    // 입력: parent: Qt 부모 object
    // 출력: 초기화된 worker 객체
    explicit StandardPreviewWorker(QObject* parent = nullptr);

    // 목적: 지정 request ID 이하의 표준 preview 결과 publish를 취소
    // 입력: requestId: 취소할 마지막 request ID
    // 출력: 없음
    void cancelRequestsThrough(quint64 requestId) noexcept;

public slots:
    // 목적: RAW 파일의 standard preview를 동기 decode 후 signal로 전달
    // 입력: requestId: 단조 증가 요청 식별자, filePath: RAW 파일 경로, targetSize: 최대 preview 크기
    // 출력: 없음
    void generateStandardPreview(quint64 requestId, const QString& filePath, const QSize& targetSize);

signals:
    // 목적: 취소되지 않은 standard preview 결과를 전달
    // 입력: requestId: 완료된 요청 식별자, filePath: RAW 파일 경로, image: 생성된 preview
    // 출력: 없음
    void standardPreviewReady(quint64 requestId, const QString& filePath, const QImage& image);

    // 목적: standard preview 생성 실패 정보를 전달
    // 입력: requestId: 실패한 요청 식별자, filePath: RAW 파일 경로, code: 오류 분류, message: 오류 설명
    // 출력: 없음
    void standardPreviewFailed(
        quint64 requestId,
        const QString& filePath,
        types::ErrorCode code,
        const QString& message);

    // 목적: 취소된 standard preview 요청을 전달
    // 입력: requestId: 취소된 요청 식별자, filePath: RAW 파일 경로
    // 출력: 없음
    void standardPreviewCancelled(quint64 requestId, const QString& filePath);

private:
    // 목적: request ID가 취소 범위에 포함되는지 확인
    // 입력: requestId: 확인할 요청 식별자
    // 출력: 결과 publish를 중단해야 하면 true
    [[nodiscard]] bool isCancelled(quint64 requestId) const noexcept;

    std::atomic<quint64> m_cancelledThrough{0};
};

} // namespace flexraw::core::preview
