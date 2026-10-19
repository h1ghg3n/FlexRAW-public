#include "raw_rgba8_bridge.h"

#include <cstddef>
#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace flexraw::core::raw
{
namespace
{

constexpr std::size_t Rgba8BytesPerPixel = 4U;

// 목적: bridge failure result 생성
// 입력: code: geometry 또는 storage 오류 분류
// 출력: 지정 오류를 보유한 실패 결과
[[nodiscard]] RawRgba8BridgeResult makeFailure(const RawRgba8BridgeErrorCode code)
{
    return RawRgba8BridgeResult::failure(RawRgba8BridgeError{code});
}

// 목적: pending orientation을 적용한 output geometry 계산
// 입력: raster: source geometry와 orientation을 가진 decoded owner
// 출력: width/height가 유효하면 oriented geometry, 아니면 빈 값
[[nodiscard]] std::optional<std::pair<std::uint32_t, std::uint32_t>> orientedGeometry(
    const RawDecodedRaster8& raster) noexcept
{
    if (raster.width() == 0 || raster.height() == 0)
    {
        return std::nullopt;
    }

    if (raster.pendingOrientation() == RawOrientation::Rotate90Clockwise ||
        raster.pendingOrientation() == RawOrientation::Rotate90CounterClockwise)
    {
        return std::pair{raster.height(), raster.width()};
    }
    return std::pair{raster.width(), raster.height()};
}

// 목적: oriented output 좌표에 대응하는 source 좌표 계산
// 입력: x/y: output 좌표, sourceWidth/sourceHeight: source geometry, orientation: 적용할 회전
// 출력: bounds 안의 source x/y 좌표
[[nodiscard]] std::pair<std::uint32_t, std::uint32_t> sourceCoordinate(const std::uint32_t x,
                                                                       const std::uint32_t y,
                                                                       const std::uint32_t sourceWidth,
                                                                       const std::uint32_t sourceHeight,
                                                                       const RawOrientation orientation) noexcept
{
    switch (orientation)
    {
    case RawOrientation::Rotate180:
        return {sourceWidth - 1U - x, sourceHeight - 1U - y};
    case RawOrientation::Rotate90CounterClockwise:
        return {sourceWidth - 1U - y, x};
    case RawOrientation::Rotate90Clockwise:
        return {y, sourceHeight - 1U - x};
    case RawOrientation::AsDecoded:
        return {x, y};
    }
    return {x, y};
}

}  // namespace

RawRgba8BridgeResult consumeRawRasterToRgba8(RawDecodedRaster8 raster)
{
    const std::optional<std::pair<std::uint32_t, std::uint32_t>> outputGeometry = orientedGeometry(raster);
    if (!outputGeometry.has_value())
    {
        return makeFailure(RawRgba8BridgeErrorCode::UnsupportedGeometry);
    }

    if (raster.layout() == RawRasterLayout::Rgba8 && raster.pendingOrientation() == RawOrientation::AsDecoded)
    {
        if (static_cast<std::size_t>(raster.height()) > std::numeric_limits<std::size_t>::max() / raster.rowStride())
        {
            return makeFailure(RawRgba8BridgeErrorCode::UnsupportedGeometry);
        }

        const std::size_t rowStride = raster.rowStride();
        const std::size_t requiredBytes = rowStride * static_cast<std::size_t>(raster.height());
        std::vector<std::uint8_t> storage = std::move(raster).takeBytes();
        storage.resize(requiredBytes);
        std::optional<image::OwnedRgba8Image> image = image::OwnedRgba8Image::create(
            outputGeometry->first, outputGeometry->second, rowStride, std::move(storage));
        if (!image.has_value())
        {
            return makeFailure(RawRgba8BridgeErrorCode::InvalidStorage);
        }
        return RawRgba8BridgeResult::success(std::move(*image));
    }

    std::optional<image::OwnedRgba8Image> output =
        image::OwnedRgba8Image::allocate(outputGeometry->first, outputGeometry->second);
    if (!output.has_value())
    {
        return makeFailure(RawRgba8BridgeErrorCode::UnsupportedGeometry);
    }

    const std::span<const std::uint8_t> sourceBytes = raster.bytes();
    image::MutableRgba8ImageView destination = output->mutableView();
    const std::size_t sourceChannels = raster.channelCount();
    for (std::uint32_t y = 0; y < outputGeometry->second; ++y)
    {
        std::span<std::uint8_t> destinationRow = destination.rowBytes(y);
        for (std::uint32_t x = 0; x < outputGeometry->first; ++x)
        {
            const auto [sourceX, sourceY] =
                sourceCoordinate(x, y, raster.width(), raster.height(), raster.pendingOrientation());
            const std::size_t sourceOffset = static_cast<std::size_t>(sourceY) * raster.rowStride() +
                                             static_cast<std::size_t>(sourceX) * sourceChannels;
            const std::size_t destinationOffset = static_cast<std::size_t>(x) * Rgba8BytesPerPixel;
            if (sourceOffset > sourceBytes.size() || sourceChannels > sourceBytes.size() - sourceOffset)
            {
                return makeFailure(RawRgba8BridgeErrorCode::InvalidStorage);
            }

            destinationRow[destinationOffset] = sourceBytes[sourceOffset];
            destinationRow[destinationOffset + 1U] = sourceBytes[sourceOffset + 1U];
            destinationRow[destinationOffset + 2U] = sourceBytes[sourceOffset + 2U];
            destinationRow[destinationOffset + 3U] =
                sourceChannels == Rgba8BytesPerPixel ? sourceBytes[sourceOffset + 3U] : 255U;
        }
    }
    return RawRgba8BridgeResult::success(std::move(*output));
}

}  // namespace flexraw::core::raw
