#include "rgba8_image_view.h"

#include <limits>

namespace flexraw::core::image
{
namespace
{

constexpr std::size_t Rgba8BytesPerPixel = 4;

// 목적: top-down RGBA8 layout과 backing storage 크기를 overflow 없이 검증
// 입력: width/height: pixel 크기, rowStride: row byte 간격, storageSize: 사용 가능한 전체 byte 수
// 출력: 유효하면 image가 사용하는 byte 수, 아니면 빈 값
[[nodiscard]] std::optional<std::size_t> checkedRgba8ByteSize(std::uint32_t width,
                                                              std::uint32_t height,
                                                              std::size_t rowStride,
                                                              std::size_t storageSize) noexcept
{
    if (width == 0 || height == 0 || width > std::numeric_limits<std::size_t>::max() / Rgba8BytesPerPixel)
    {
        return std::nullopt;
    }

    const std::size_t minimumRowStride = static_cast<std::size_t>(width) * Rgba8BytesPerPixel;
    if (rowStride < minimumRowStride || rowStride > std::numeric_limits<std::size_t>::max() / height)
    {
        return std::nullopt;
    }

    const std::size_t requiredByteSize = rowStride * height;
    if (storageSize < requiredByteSize)
    {
        return std::nullopt;
    }
    return requiredByteSize;
}

}  // namespace

// 목적: caller-owned top-down RGBA8/sRGB storage의 validated read-only view 생성
// 입력: width/height: pixel 크기, rowStride: row byte 간격, bytes: 호출 기간 동안 유지되는 backing storage
// 출력: layout과 storage가 유효하면 bounded view, 아니면 빈 값
std::optional<ConstRgba8ImageView> ConstRgba8ImageView::create(std::uint32_t width,
                                                               std::uint32_t height,
                                                               std::size_t rowStride,
                                                               std::span<const std::uint8_t> bytes) noexcept
{
    const std::optional<std::size_t> byteSize = checkedRgba8ByteSize(width, height, rowStride, bytes.size());
    if (!byteSize.has_value() || bytes.data() == nullptr)
    {
        return std::nullopt;
    }
    return ConstRgba8ImageView(width, height, rowStride, *byteSize, bytes);
}

// 목적: image pixel 너비 반환
// 입력: 없음
// 출력: 0보다 큰 pixel 수
std::uint32_t ConstRgba8ImageView::width() const noexcept
{
    return m_width;
}

// 목적: image pixel 높이 반환
// 입력: 없음
// 출력: 0보다 큰 pixel 수
std::uint32_t ConstRgba8ImageView::height() const noexcept
{
    return m_height;
}

// 목적: 인접 top-down row 사이의 byte 간격 반환
// 입력: 없음
// 출력: width * 4 이상의 stride
std::size_t ConstRgba8ImageView::rowStride() const noexcept
{
    return m_rowStride;
}

// 목적: validated image 범위의 immutable byte view 반환
// 입력: 없음
// 출력: rowStride * height 크기의 read-only span
std::span<const std::uint8_t> ConstRgba8ImageView::bytes() const noexcept
{
    return m_bytes.first(m_byteSize);
}

// 목적: 지정 top-down row의 immutable byte view 반환
// 입력: row: 0부터 시작하는 row index
// 출력: 유효하면 width * 4 크기의 pixel-only read-only span, 아니면 빈 span
std::span<const std::uint8_t> ConstRgba8ImageView::rowBytes(std::uint32_t row) const noexcept
{
    if (row >= m_height)
    {
        return {};
    }
    return bytes().subspan(static_cast<std::size_t>(row) * m_rowStride,
                           static_cast<std::size_t>(m_width) * Rgba8BytesPerPixel);
}

// 목적: validation된 caller-owned RGBA8/sRGB view state 저장
// 입력: width/height/rowStride/byteSize: 검증된 layout, bytes: backing storage
// 출력: create()만 구성할 수 있는 read-only view
ConstRgba8ImageView::ConstRgba8ImageView(std::uint32_t width,
                                         std::uint32_t height,
                                         std::size_t rowStride,
                                         std::size_t byteSize,
                                         std::span<const std::uint8_t> bytes) noexcept
    : m_width(width), m_height(height), m_rowStride(rowStride), m_byteSize(byteSize), m_bytes(bytes)
{}

// 목적: caller-owned top-down RGBA8/sRGB storage의 validated mutable view 생성
// 입력: width/height: pixel 크기, rowStride: row byte 간격, bytes: 호출 기간 동안 유지되는 backing storage
// 출력: layout과 storage가 유효하면 bounded view, 아니면 빈 값
std::optional<MutableRgba8ImageView> MutableRgba8ImageView::create(std::uint32_t width,
                                                                   std::uint32_t height,
                                                                   std::size_t rowStride,
                                                                   std::span<std::uint8_t> bytes) noexcept
{
    const std::optional<std::size_t> byteSize = checkedRgba8ByteSize(width, height, rowStride, bytes.size());
    if (!byteSize.has_value() || bytes.data() == nullptr)
    {
        return std::nullopt;
    }
    return MutableRgba8ImageView(width, height, rowStride, *byteSize, bytes);
}

// 목적: image pixel 너비 반환
// 입력: 없음
// 출력: 0보다 큰 pixel 수
std::uint32_t MutableRgba8ImageView::width() const noexcept
{
    return m_width;
}

// 목적: image pixel 높이 반환
// 입력: 없음
// 출력: 0보다 큰 pixel 수
std::uint32_t MutableRgba8ImageView::height() const noexcept
{
    return m_height;
}

// 목적: 인접 top-down row 사이의 byte 간격 반환
// 입력: 없음
// 출력: width * 4 이상의 stride
std::size_t MutableRgba8ImageView::rowStride() const noexcept
{
    return m_rowStride;
}

// 목적: validated image 범위의 mutable byte view 반환
// 입력: 없음
// 출력: rowStride * height 크기의 writable span
std::span<std::uint8_t> MutableRgba8ImageView::bytes() const noexcept
{
    return m_bytes.first(m_byteSize);
}

// 목적: 지정 top-down row의 mutable byte view 반환
// 입력: row: 0부터 시작하는 row index
// 출력: 유효하면 width * 4 크기의 pixel-only writable span, 아니면 빈 span
std::span<std::uint8_t> MutableRgba8ImageView::rowBytes(std::uint32_t row) const noexcept
{
    if (row >= m_height)
    {
        return {};
    }
    return bytes().subspan(static_cast<std::size_t>(row) * m_rowStride,
                           static_cast<std::size_t>(m_width) * Rgba8BytesPerPixel);
}

// 목적: validation된 caller-owned RGBA8/sRGB view state 저장
// 입력: width/height/rowStride/byteSize: 검증된 layout, bytes: backing storage
// 출력: create()만 구성할 수 있는 mutable view
MutableRgba8ImageView::MutableRgba8ImageView(std::uint32_t width,
                                             std::uint32_t height,
                                             std::size_t rowStride,
                                             std::size_t byteSize,
                                             std::span<std::uint8_t> bytes) noexcept
    : m_width(width), m_height(height), m_rowStride(rowStride), m_byteSize(byteSize), m_bytes(bytes)
{}

}  // namespace flexraw::core::image
