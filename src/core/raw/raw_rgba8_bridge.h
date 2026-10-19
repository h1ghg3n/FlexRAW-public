#pragma once

#include <cstdint>

#include "owned_rgba8_image.h"
#include "raw_processing.h"
#include "result.h"

namespace flexraw::core::raw
{

enum class RawRgba8BridgeErrorCode : std::uint8_t
{
    UnsupportedGeometry,
    InvalidStorage,
};

struct RawRgba8BridgeError
{
    RawRgba8BridgeErrorCode code{RawRgba8BridgeErrorCode::InvalidStorage};

    [[nodiscard]] bool operator==(const RawRgba8BridgeError& other) const = default;
};

using RawRgba8BridgeResult = types::Result<image::OwnedRgba8Image, RawRgba8BridgeError>;

// 목적: move-only RAW RGB8/RGBA8 storage를 top-down RGBA8/sRGB owner로 변환
// 입력: raster: 소비할 decoded storage와 pending orientation
// 출력: orientation과 alpha가 적용된 concrete image owner 또는 bridge 오류
[[nodiscard]] RawRgba8BridgeResult consumeRawRasterToRgba8(RawDecodedRaster8 raster);

}  // namespace flexraw::core::raw
