#include "supported_extensions.h"

#include "file_format_registry.h"

namespace flexraw::core::util {
namespace {

// 목적: 확장자 비교를 위한 표준 형태 생성
// 입력: extension: 점이 있거나 없는 파일 확장자
// 출력: 공백과 leading dot 을 제거하고 소문자로 변환한 확장자
[[nodiscard]] QString normalizeExtension(const QString& extension)
{
    QString normalized = extension.trimmed();

    while (normalized.startsWith('.')) {
        normalized.remove(0, 1);
    }

    return normalized.toLower();
}

// 목적: 정규화된 확장자가 RAW 파일 확장자인지 확인
// 입력: normalizedExtension: normalizeExtension 을 거친 확장자
// 출력: RAW 파일 확장자 여부
[[nodiscard]] bool isRawExtension(const QString& normalizedExtension)
{
    return rawExtensions().contains(normalizedExtension);
}

// 목적: 정규화된 확장자가 raster image 파일 확장자인지 확인
// 입력: normalizedExtension: normalizeExtension 을 거친 확장자
// 출력: raster image 파일 확장자 여부
[[nodiscard]] bool isRasterImageExtension(const QString& normalizedExtension)
{
    return rasterImageExtensions().contains(normalizedExtension);
}

} // namespace

// 목적: 파일 확장자를 지원 파일 종류로 분류
// 입력: extension: 점이 있거나 없는 파일 확장자
// 출력: RAW, raster image, unknown 중 하나
types::SupportedFileKind classifyExtension(const QString& extension)
{
    const QString normalizedExtension = normalizeExtension(extension);

    if (isRawExtension(normalizedExtension)) {
        return types::SupportedFileKind::Raw;
    }

    if (isRasterImageExtension(normalizedExtension)) {
        return types::SupportedFileKind::RasterImage;
    }

    return types::SupportedFileKind::Unknown;
}

// 목적: 확장자가 Flexraw 의 첫 vertical slice 에서 지원되는지 확인
// 입력: extension: 점이 있거나 없는 파일 확장자
// 출력: 지원 여부
bool isSupportedExtension(const QString& extension)
{
    return classifyExtension(extension) != types::SupportedFileKind::Unknown;
}

} // namespace flexraw::core::util
