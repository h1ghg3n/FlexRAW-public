#pragma once

#include "file_types.h"

#include <QString>

namespace flexraw::core::util {

// 목적: 확장자가 Flexraw 의 첫 vertical slice 에서 지원되는지 확인
// 입력: extension: 점이 있거나 없는 파일 확장자
// 출력: 지원 여부
[[nodiscard]] bool isSupportedExtension(const QString& extension);

// 목적: 파일 확장자를 지원 파일 종류로 분류
// 입력: extension: 점이 있거나 없는 파일 확장자
// 출력: RAW, raster image, unknown 중 하나
[[nodiscard]] types::SupportedFileKind classifyExtension(const QString& extension);

} // namespace flexraw::core::util
