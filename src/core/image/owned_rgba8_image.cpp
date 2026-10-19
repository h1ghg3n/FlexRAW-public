#include "owned_rgba8_image.h"

#include <limits>
#include <utility>

namespace flexraw::core::image
{
namespace
{

constexpr std::size_t Rgba8BytesPerPixel = 4U;

// 목적: RGBA8 geometry에서 required byte 수와 tight row stride 계산
// 입력: width/height: 검증할 pixel geometry
// 출력: 유효하면 row stride와 byte 수, overflow 또는 zero geometry면 빈 값
[[nodiscard]] std::optional<std::pair<std::size_t, std::size_t>> checkedRgba8Size(const std::uint32_t width,
                                                                                  const std::uint32_t height) noexcept
{
    if (width == 0 || height == 0 ||
        static_cast<std::size_t>(width) > std::numeric_limits<std::size_t>::max() / Rgba8BytesPerPixel)
    {
        return std::nullopt;
    }

    const std::size_t rowStride = static_cast<std::size_t>(width) * Rgba8BytesPerPixel;
    if (static_cast<std::size_t>(height) > std::numeric_limits<std::size_t>::max() / rowStride)
    {
        return std::nullopt;
    }
    return std::pair{rowStride, rowStride * static_cast<std::size_t>(height)};
}

}  // namespace

OwnedRgba8Image::OwnedRgba8Image(const std::uint32_t width,
                                 const std::uint32_t height,
                                 const std::size_t rowStride,
                                 std::vector<std::uint8_t> bytes) noexcept
    : m_width(width), m_height(height), m_rowStride(rowStride), m_bytes(std::move(bytes))
{}

std::optional<OwnedRgba8Image> OwnedRgba8Image::create(const std::uint32_t width,
                                                       const std::uint32_t height,
                                                       const std::size_t rowStride,
                                                       std::vector<std::uint8_t> bytes) noexcept
{
    const std::optional<std::pair<std::size_t, std::size_t>> tightSize = checkedRgba8Size(width, height);
    if (!tightSize.has_value() || rowStride < tightSize->first ||
        static_cast<std::size_t>(height) > std::numeric_limits<std::size_t>::max() / rowStride)
    {
        return std::nullopt;
    }

    const std::size_t requiredBytes = rowStride * static_cast<std::size_t>(height);
    if (bytes.size() != requiredBytes)
    {
        return std::nullopt;
    }
    return OwnedRgba8Image(width, height, rowStride, std::move(bytes));
}

std::optional<OwnedRgba8Image> OwnedRgba8Image::allocate(const std::uint32_t width, const std::uint32_t height)
{
    const std::optional<std::pair<std::size_t, std::size_t>> size = checkedRgba8Size(width, height);
    if (!size.has_value())
    {
        return std::nullopt;
    }
    return OwnedRgba8Image(width, height, size->first, std::vector<std::uint8_t>(size->second));
}

std::uint32_t OwnedRgba8Image::width() const noexcept
{
    return m_width;
}

std::uint32_t OwnedRgba8Image::height() const noexcept
{
    return m_height;
}

std::size_t OwnedRgba8Image::rowStride() const noexcept
{
    return m_rowStride;
}

std::span<const std::uint8_t> OwnedRgba8Image::bytes() const noexcept
{
    return m_bytes;
}

std::span<std::uint8_t> OwnedRgba8Image::mutableBytes() noexcept
{
    return m_bytes;
}

ConstRgba8ImageView OwnedRgba8Image::constView() const noexcept
{
    return *ConstRgba8ImageView::create(m_width, m_height, m_rowStride, m_bytes);
}

MutableRgba8ImageView OwnedRgba8Image::mutableView() noexcept
{
    return *MutableRgba8ImageView::create(m_width, m_height, m_rowStride, m_bytes);
}

}  // namespace flexraw::core::image
