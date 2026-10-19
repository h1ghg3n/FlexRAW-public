#include "png_encoder.h"

#include <cstddef>
#include <limits>
#include <utility>

#include <png.h>

namespace flexraw::core::export_
{

EncodedPng8::EncodedPng8(std::vector<std::uint8_t> bytes,
                         const std::uint32_t width,
                         const std::uint32_t height) noexcept
    : m_bytes(std::move(bytes)), m_width(width), m_height(height)
{}

std::uint32_t EncodedPng8::width() const noexcept
{
    return m_width;
}

std::uint32_t EncodedPng8::height() const noexcept
{
    return m_height;
}

std::span<const std::uint8_t> EncodedPng8::bytes() const noexcept
{
    return m_bytes;
}

PngEncodingResult encodePng8Srgb(const image::ConstRgba8ImageView source)
{
    if (source.width() == 0 || source.height() == 0 || source.bytes().empty() ||
        source.rowStride() > static_cast<std::size_t>(std::numeric_limits<png_int_32>::max()))
    {
        return PngEncodingResult::failure(PngEncodingError{PngEncodingErrorCode::InvalidImage});
    }

    png_image png{};
    png.version = PNG_IMAGE_VERSION;
    png.width = source.width();
    png.height = source.height();
    png.format = PNG_FORMAT_RGBA;

    png_alloc_size_t requiredBytes = 0;
    if (png_image_write_to_memory(&png,
                                  nullptr,
                                  &requiredBytes,
                                  0,
                                  source.bytes().data(),
                                  static_cast<png_int_32>(source.rowStride()),
                                  nullptr) == 0)
    {
        png_image_free(&png);
        return PngEncodingResult::failure(PngEncodingError{PngEncodingErrorCode::EncodingFailed});
    }
    if (requiredBytes > std::numeric_limits<std::size_t>::max())
    {
        png_image_free(&png);
        return PngEncodingResult::failure(PngEncodingError{PngEncodingErrorCode::OutputTooLarge});
    }

    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(requiredBytes));
    if (png_image_write_to_memory(&png,
                                  bytes.data(),
                                  &requiredBytes,
                                  0,
                                  source.bytes().data(),
                                  static_cast<png_int_32>(source.rowStride()),
                                  nullptr) == 0)
    {
        png_image_free(&png);
        return PngEncodingResult::failure(PngEncodingError{PngEncodingErrorCode::EncodingFailed});
    }
    png_image_free(&png);
    bytes.resize(static_cast<std::size_t>(requiredBytes));

    return PngEncodingResult::success(EncodedPng8(std::move(bytes), source.width(), source.height()));
}

}  // namespace flexraw::core::export_
