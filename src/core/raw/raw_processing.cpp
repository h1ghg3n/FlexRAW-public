#include "raw_processing.h"

#include <limits>
#include <memory>

#include <libraw/libraw.h>

namespace flexraw::core::raw
{
namespace
{

struct ProcessedImageDeleter
{
    // 목적: LibRaw가 할당한 processed image storage 해제
    // 입력: image: nullable LibRaw processed image pointer
    // 출력: 없음
    void operator()(libraw_processed_image_t* image) const noexcept
    {
        if (image != nullptr)
        {
            LibRaw::dcraw_clear_mem(image);
        }
    }
};

using ProcessedImagePtr = std::unique_ptr<libraw_processed_image_t, ProcessedImageDeleter>;

// 목적: LibRaw status와 decode 단계에 대응하는 neutral 오류 생성
// 입력: status: LibRaw status, fallback: 현재 단계의 기본 오류
// 출력: unsupported source를 보존한 project-owned 오류
[[nodiscard]] RawDecodeError makeDecodeError(const int status, const RawDecodeErrorCode fallback) noexcept
{
    if (status == LIBRAW_FILE_UNSUPPORTED)
    {
        return RawDecodeError{RawDecodeErrorCode::UnsupportedFormat};
    }

    return RawDecodeError{fallback};
}

// 목적: 8-bit interleaved layout의 channel 수 반환
// 입력: layout: RGB 또는 RGBA layout
// 출력: logical pixel당 byte 수
[[nodiscard]] constexpr std::size_t layoutChannelCount(const RawRasterLayout layout) noexcept
{
    return layout == RawRasterLayout::Rgb8 ? 3U : 4U;
}

}  // namespace

RawDecodedRaster8::RawDecodedRaster8(std::vector<std::uint8_t> bytes,
                                     const std::uint32_t width,
                                     const std::uint32_t height,
                                     const std::size_t rowStride,
                                     const RawRasterLayout layout,
                                     const RawOrientation pendingOrientation,
                                     RawDecodeObservation observation) noexcept
    : m_bytes(std::move(bytes)),
      m_width(width),
      m_height(height),
      m_rowStride(rowStride),
      m_layout(layout),
      m_pendingOrientation(pendingOrientation),
      m_observation(std::move(observation))
{}

std::optional<RawDecodedRaster8> RawDecodedRaster8::create(std::vector<std::uint8_t> bytes,
                                                           const std::uint32_t width,
                                                           const std::uint32_t height,
                                                           const std::size_t rowStride,
                                                           const RawRasterLayout layout,
                                                           const RawOrientation pendingOrientation,
                                                           RawDecodeObservation observation) noexcept
{
    if (layout != RawRasterLayout::Rgb8 && layout != RawRasterLayout::Rgba8)
    {
        return std::nullopt;
    }

    const std::size_t channels = layoutChannelCount(layout);
    if (width == 0 || height == 0 ||
        static_cast<std::size_t>(width) > std::numeric_limits<std::size_t>::max() / channels)
    {
        return std::nullopt;
    }

    const std::size_t logicalRowBytes = static_cast<std::size_t>(width) * channels;
    if (rowStride < logicalRowBytes ||
        static_cast<std::size_t>(height) > std::numeric_limits<std::size_t>::max() / rowStride)
    {
        return std::nullopt;
    }

    const std::size_t requiredBytes = rowStride * static_cast<std::size_t>(height);
    if (bytes.size() < requiredBytes || observation.decodedWidth == 0 || observation.decodedHeight == 0 ||
        observation.processedWidth != width || observation.processedHeight != height)
    {
        return std::nullopt;
    }

    return RawDecodedRaster8(
        std::move(bytes), width, height, rowStride, layout, pendingOrientation, std::move(observation));
}

std::uint32_t RawDecodedRaster8::width() const noexcept
{
    return m_width;
}

std::uint32_t RawDecodedRaster8::height() const noexcept
{
    return m_height;
}

std::size_t RawDecodedRaster8::rowStride() const noexcept
{
    return m_rowStride;
}

RawRasterLayout RawDecodedRaster8::layout() const noexcept
{
    return m_layout;
}

std::uint32_t RawDecodedRaster8::channelCount() const noexcept
{
    return static_cast<std::uint32_t>(layoutChannelCount(m_layout));
}

RawOrientation RawDecodedRaster8::pendingOrientation() const noexcept
{
    return m_pendingOrientation;
}

const RawDecodeObservation& RawDecodedRaster8::observation() const noexcept
{
    return m_observation;
}

std::span<const std::uint8_t> RawDecodedRaster8::bytes() const noexcept
{
    return m_bytes;
}

// 목적: decoded raster를 소비하는 downstream으로 pixel storage 소유권 이관
// 입력: rvalue로 호출된 현재 owner
// 출력: 기존 allocation을 유지하는 byte vector
std::vector<std::uint8_t> RawDecodedRaster8::takeBytes() && noexcept
{
    return std::move(m_bytes);
}

RawOrientation rawOrientationFromSourceFlip(const int sourceFlip) noexcept
{
    switch (sourceFlip)
    {
    case 3:
    case 180:
        return RawOrientation::Rotate180;
    case 5:
    case 270:
        return RawOrientation::Rotate90CounterClockwise;
    case 6:
    case 90:
        return RawOrientation::Rotate90Clockwise;
    default:
        return RawOrientation::AsDecoded;
    }
}

RawDecodeResult decodeRawRaster8(const std::filesystem::path& path)
{
    if (path.empty())
    {
        return RawDecodeResult::failure(RawDecodeError{RawDecodeErrorCode::InvalidPath});
    }

    LibRaw decoder;
    int status = decoder.open_file(path.c_str());
    if (status != LIBRAW_SUCCESS)
    {
        return RawDecodeResult::failure(makeDecodeError(status, RawDecodeErrorCode::OpenFailed));
    }

    // 현재 product의 As Shot white balance, sRGB transfer와 8-bit output semantics를 고정한다.
    decoder.imgdata.params.output_bps = 8;
    decoder.imgdata.params.output_color = 1;
    decoder.imgdata.params.use_camera_wb = 1;
    decoder.imgdata.params.gamm[0] = 1.0 / 2.4;
    decoder.imgdata.params.gamm[1] = 12.92;

    status = decoder.unpack();
    if (status != LIBRAW_SUCCESS)
    {
        return RawDecodeResult::failure(makeDecodeError(status, RawDecodeErrorCode::UnpackFailed));
    }

    const int decodedWidth = decoder.imgdata.sizes.width;
    const int decodedHeight = decoder.imgdata.sizes.height;
    const int sourceFlip = decoder.imgdata.sizes.flip;

    status = decoder.dcraw_process();
    if (status != LIBRAW_SUCCESS)
    {
        return RawDecodeResult::failure(makeDecodeError(status, RawDecodeErrorCode::ProcessFailed));
    }

    ProcessedImagePtr processedImage(decoder.dcraw_make_mem_image(&status));
    if (status != LIBRAW_SUCCESS || processedImage == nullptr)
    {
        return RawDecodeResult::failure(makeDecodeError(status, RawDecodeErrorCode::OutputCreationFailed));
    }

    if (processedImage->type != LIBRAW_IMAGE_BITMAP || processedImage->data == nullptr || processedImage->bits != 8 ||
        (processedImage->colors != 3 && processedImage->colors != 4))
    {
        return RawDecodeResult::failure(RawDecodeError{RawDecodeErrorCode::UnsupportedLayout});
    }

    if (decodedWidth <= 0 || decodedHeight <= 0 || processedImage->width == 0 || processedImage->height == 0)
    {
        return RawDecodeResult::failure(RawDecodeError{RawDecodeErrorCode::UnsupportedGeometry});
    }

    const RawRasterLayout layout = processedImage->colors == 3 ? RawRasterLayout::Rgb8 : RawRasterLayout::Rgba8;
    const std::size_t channels = layoutChannelCount(layout);
    if (static_cast<std::size_t>(processedImage->width) > std::numeric_limits<std::size_t>::max() / channels)
    {
        return RawDecodeResult::failure(RawDecodeError{RawDecodeErrorCode::UnsupportedGeometry});
    }

    const std::size_t rowStride = static_cast<std::size_t>(processedImage->width) * channels;
    if (static_cast<std::size_t>(processedImage->height) > std::numeric_limits<std::size_t>::max() / rowStride ||
        static_cast<std::size_t>(processedImage->data_size) <
            rowStride * static_cast<std::size_t>(processedImage->height))
    {
        return RawDecodeResult::failure(RawDecodeError{RawDecodeErrorCode::UnsupportedGeometry});
    }

    const bool outputRemainsUnrotated = processedImage->width == static_cast<unsigned int>(decodedWidth) &&
                                        processedImage->height == static_cast<unsigned int>(decodedHeight);
    const RawOrientation orientation =
        outputRemainsUnrotated ? rawOrientationFromSourceFlip(sourceFlip) : RawOrientation::AsDecoded;
    RawDecodeObservation observation{
        static_cast<std::uint32_t>(decodedWidth),
        static_cast<std::uint32_t>(decodedHeight),
        processedImage->width,
        processedImage->height,
        sourceFlip,
    };
    std::vector<std::uint8_t> bytes(processedImage->data,
                                    processedImage->data + static_cast<std::size_t>(processedImage->data_size));
    std::optional<RawDecodedRaster8> raster = RawDecodedRaster8::create(
        std::move(bytes), processedImage->width, processedImage->height, rowStride, layout, orientation, observation);
    if (!raster.has_value())
    {
        return RawDecodeResult::failure(RawDecodeError{RawDecodeErrorCode::UnsupportedGeometry});
    }

    return RawDecodeResult::success(std::move(*raster));
}

}  // namespace flexraw::core::raw
