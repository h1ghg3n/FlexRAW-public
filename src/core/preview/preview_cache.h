#pragma once

#include "error.h"
#include "result.h"

#include <QImage>
#include <QSize>
#include <QString>

#include <variant>

namespace flexraw::core::preview {

// 목적: 설정 UI가 없을 때 사용할 기본 preview cache root 경로 반환
// 입력: 없음
// 출력: 사용자 writable directory 아래 preview cache root 경로
[[nodiscard]] QString defaultPreviewCacheRoot();

enum class PreviewCacheTier {
    Thumbnail,
    Standard,
};

struct PreviewCacheLookup {
    bool hit{false};
    QImage image;
};

using PreviewCacheLookupResult = types::Result<PreviewCacheLookup, types::CoreError>;
using PreviewCacheStoreResult = types::Result<std::monostate, types::CoreError>;

class PreviewCache final
{
public:
    // 목적: 명시된 root directory를 사용하는 preview cache 초기화
    // 입력: rootPath: thumbnail과 standard preview를 저장할 root directory
    // 출력: 초기화된 preview cache 객체
    explicit PreviewCache(QString rootPath);

    // 목적: 원본 파일과 target 크기에 일치하는 유효한 preview cache 항목 조회
    // 입력: sourcePath: 원본 파일 경로, tier: preview 품질 단계, targetSize: 최대 preview 크기
    // 출력: cache hit 여부와 QImage 또는 구조화된 오류
    [[nodiscard]] PreviewCacheLookupResult load(
        const QString& sourcePath,
        PreviewCacheTier tier,
        const QSize& targetSize) const;

    // 목적: 원본 파일의 현재 mtime에 연결된 preview image를 cache에 원자적으로 저장
    // 입력: sourcePath: 원본 파일 경로, tier: preview 품질 단계, targetSize: 최대 preview 크기, image: 저장할 preview
    // 출력: 성공 표식 또는 구조화된 오류
    [[nodiscard]] PreviewCacheStoreResult store(
        const QString& sourcePath,
        PreviewCacheTier tier,
        const QSize& targetSize,
        const QImage& image) const;

private:
    QString m_rootPath;
};

} // namespace flexraw::core::preview
