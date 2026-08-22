#pragma once

#include <QStringList>

namespace flexraw::core::util {

// 목적: catalog discovery 에서 RAW 후보로 취급할 확장자 목록 반환
// 입력: 없음
// 출력: 점 없는 소문자 RAW 확장자 목록
[[nodiscard]] const QStringList& rawExtensions();

// 목적: catalog discovery 에서 raster image 후보로 취급할 확장자 목록 반환
// 입력: 없음
// 출력: 점 없는 소문자 raster image 확장자 목록
[[nodiscard]] const QStringList& rasterImageExtensions();

} // namespace flexraw::core::util
