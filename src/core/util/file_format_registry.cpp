#include "file_format_registry.h"

namespace flexraw::core::util {

// 목적: catalog discovery 에서 RAW 후보로 취급할 확장자 목록 반환
// 입력: 없음
// 출력: 점 없는 소문자 RAW 확장자 목록
const QStringList& rawExtensions()
{
    static const QStringList Extensions{
        "cr2",
        "cr3",
        "nef",
        "nefx",
        "arw",
        "arq",
        "dng",
        "raf",
        "orf",
        "ori",
        "rw2",
        "pef",
        "srw",
    };

    return Extensions;
}

// 목적: catalog discovery 에서 raster image 후보로 취급할 확장자 목록 반환
// 입력: 없음
// 출력: 점 없는 소문자 raster image 확장자 목록
const QStringList& rasterImageExtensions()
{
    static const QStringList Extensions{
        "jpg",
        "jpeg",
        "png",
        "tif",
        "tiff",
    };

    return Extensions;
}

} // namespace flexraw::core::util
