#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "result.h"

namespace flexraw::core::raw
{

enum class RawRasterLayout : std::uint8_t
{
    Rgb8,
    Rgba8,
};

enum class RawOrientation : std::uint8_t
{
    AsDecoded,
    Rotate180,
    Rotate90CounterClockwise,
    Rotate90Clockwise,
};

struct RawDecodeObservation
{
    std::uint32_t decodedWidth{0};
    std::uint32_t decodedHeight{0};
    std::uint32_t processedWidth{0};
    std::uint32_t processedHeight{0};
    int sourceFlip{0};

    [[nodiscard]] bool operator==(const RawDecodeObservation& other) const = default;
};

class RawDecodedRaster8 final
{
public:
    RawDecodedRaster8(const RawDecodedRaster8&) = delete;
    RawDecodedRaster8& operator=(const RawDecodedRaster8&) = delete;
    RawDecodedRaster8(RawDecodedRaster8&&) noexcept = default;
    RawDecodedRaster8& operator=(RawDecodedRaster8&&) noexcept = default;

    // 목적: caller가 제공한 storage와 layout으로 validated RAW raster owner 생성
    // 입력: bytes: 이동할 pixel storage, geometry/layout/orientation/observation: decode 결과 의미
    // 출력: storage와 geometry가 유효하면 move-only raster, 아니면 빈 값
    [[nodiscard]] static std::optional<RawDecodedRaster8> create(std::vector<std::uint8_t> bytes,
                                                                 std::uint32_t width,
                                                                 std::uint32_t height,
                                                                 std::size_t rowStride,
                                                                 RawRasterLayout layout,
                                                                 RawOrientation pendingOrientation,
                                                                 RawDecodeObservation observation) noexcept;

    // 목적: decoded raster의 pixel 너비 반환
    // 입력: 없음
    // 출력: 0보다 큰 pixel 수
    [[nodiscard]] std::uint32_t width() const noexcept;

    // 목적: decoded raster의 pixel 높이 반환
    // 입력: 없음
    // 출력: 0보다 큰 pixel 수
    [[nodiscard]] std::uint32_t height() const noexcept;

    // 목적: 인접 row 사이의 byte 간격 반환
    // 입력: 없음
    // 출력: logical row byte 수 이상의 stride
    [[nodiscard]] std::size_t rowStride() const noexcept;

    // 목적: decoded raster의 RGB/RGBA byte layout 반환
    // 입력: 없음
    // 출력: 현재 8-bit interleaved layout
    [[nodiscard]] RawRasterLayout layout() const noexcept;

    // 목적: decoded raster의 channel 수 반환
    // 입력: 없음
    // 출력: RGB는 3, RGBA는 4
    [[nodiscard]] std::uint32_t channelCount() const noexcept;

    // 목적: pixel storage에 아직 적용되지 않은 camera orientation 반환
    // 입력: 없음
    // 출력: downstream projection이 적용할 orientation
    [[nodiscard]] RawOrientation pendingOrientation() const noexcept;

    // 목적: RAW decode geometry와 source flip 관측값 반환
    // 입력: 없음
    // 출력: immutable decode observation
    [[nodiscard]] const RawDecodeObservation& observation() const noexcept;

    // 목적: decoded pixel storage의 read-only byte view 반환
    // 입력: 없음
    // 출력: 최소 rowStride * height 크기의 immutable storage
    [[nodiscard]] std::span<const std::uint8_t> bytes() const noexcept;

    // 목적: 단일 downstream owner에게 decoded pixel storage 소유권 이관
    // 입력: rvalue로 소비될 decoded raster owner
    // 출력: geometry/layout 정보와 분리된 contiguous pixel storage
    [[nodiscard]] std::vector<std::uint8_t> takeBytes() && noexcept;

private:
    // 목적: validation을 통과한 decoded RAW storage와 의미 저장
    // 입력: validated storage, geometry, layout, orientation과 observation
    // 출력: create()만 구성할 수 있는 move-only owner
    RawDecodedRaster8(std::vector<std::uint8_t> bytes,
                      std::uint32_t width,
                      std::uint32_t height,
                      std::size_t rowStride,
                      RawRasterLayout layout,
                      RawOrientation pendingOrientation,
                      RawDecodeObservation observation) noexcept;

    std::vector<std::uint8_t> m_bytes;
    std::uint32_t m_width{0};
    std::uint32_t m_height{0};
    std::size_t m_rowStride{0};
    RawRasterLayout m_layout{RawRasterLayout::Rgb8};
    RawOrientation m_pendingOrientation{RawOrientation::AsDecoded};
    RawDecodeObservation m_observation;
};

enum class RawDecodeErrorCode : std::uint8_t
{
    InvalidPath,
    OpenFailed,
    UnsupportedFormat,
    UnpackFailed,
    ProcessFailed,
    OutputCreationFailed,
    UnsupportedLayout,
    UnsupportedGeometry,
};

struct RawDecodeError
{
    RawDecodeErrorCode code{RawDecodeErrorCode::OpenFailed};

    [[nodiscard]] bool operator==(const RawDecodeError& other) const = default;
};

using RawDecodeResult = types::Result<RawDecodedRaster8, RawDecodeError>;

// 목적: LibRaw source flip 값을 project-owned pending orientation으로 변환
// 입력: sourceFlip: LibRaw가 보고한 flip 또는 degree 값
// 출력: downstream이 적용할 neutral orientation
[[nodiscard]] RawOrientation rawOrientationFromSourceFlip(int sourceFlip) noexcept;

// 목적: RAW 파일을 현재 product와 동일한 As Shot/sRGB 8-bit raster로 decode
// 입력: path: native filesystem RAW 경로
// 출력: move-only RGB/RGBA raster와 observation 또는 neutral decode 오류
[[nodiscard]] RawDecodeResult decodeRawRaster8(const std::filesystem::path& path);

}  // namespace flexraw::core::raw
