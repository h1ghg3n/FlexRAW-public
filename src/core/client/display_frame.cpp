#include "display_frame.h"

#include <limits>
#include <utility>

namespace flexraw::core::client
{

// 목적: checked BGRA8/top-down/sRGB immutable display frame 생성
// 입력: width/height: pixel 크기, rowStride: row byte 간격, previewRevision: preview 순서, bytes: owned buffer
// 출력: contract가 유효하면 shared immutable frame, 아니면 빈 값
std::optional<DisplayFrame> DisplayFrame::create(std::uint32_t width,
                                                 std::uint32_t height,
                                                 std::size_t rowStride,
                                                 std::uint64_t previewRevision,
                                                 std::vector<std::uint8_t> bytes)
{
    if (width == 0 || height == 0 || width > std::numeric_limits<std::size_t>::max() / 4)
    {
        return std::nullopt;
    }

    const std::size_t minimumRowStride = static_cast<std::size_t>(width) * 4;
    if (rowStride < minimumRowStride || rowStride > std::numeric_limits<std::size_t>::max() / height)
    {
        return std::nullopt;
    }

    const std::size_t requiredByteSize = rowStride * height;
    if (bytes.size() != requiredByteSize)
    {
        return std::nullopt;
    }
    for (std::size_t row = 0; row < height; ++row)
    {
        const std::size_t rowOffset = row * rowStride;
        for (std::size_t alphaOffset = 3; alphaOffset < minimumRowStride; alphaOffset += 4)
        {
            if (bytes[rowOffset + alphaOffset] != 255)
            {
                return std::nullopt;
            }
        }
    }
    return DisplayFrame(width, height, rowStride, previewRevision, std::move(bytes));
}

// 목적: frame pixel 너비 반환
// 입력: 없음
// 출력: 0보다 큰 pixel 수
std::uint32_t DisplayFrame::width() const noexcept
{
    return m_width;
}

// 목적: frame pixel 높이 반환
// 입력: 없음
// 출력: 0보다 큰 pixel 수
std::uint32_t DisplayFrame::height() const noexcept
{
    return m_height;
}

// 목적: 인접 top-down row 사이의 byte 간격 반환
// 입력: 없음
// 출력: width * 4 이상의 stride
std::size_t DisplayFrame::rowStride() const noexcept
{
    return m_rowStride;
}

// 목적: frame과 연결된 preview revision 반환
// 입력: 없음
// 출력: producer가 전달한 단조 증가 revision
std::uint64_t DisplayFrame::previewRevision() const noexcept
{
    return m_previewRevision;
}

// 목적: v1 display pixel format 반환
// 입력: 없음
// 출력: BGRA8
DisplayPixelFormat DisplayFrame::pixelFormat() const noexcept
{
    return DisplayPixelFormat::Bgra8;
}

// 목적: v1 display color encoding 반환
// 입력: 없음
// 출력: display-ready sRGB
ColorEncoding DisplayFrame::colorEncoding() const noexcept
{
    return ColorEncoding::Srgb;
}

// 목적: immutable frame 전체 byte view 반환
// 입력: 없음
// 출력: rowStride * height 크기의 read-only span
std::span<const std::uint8_t> DisplayFrame::bytes() const noexcept
{
    return {m_bytes->data(), m_bytes->size()};
}

// 목적: top-down row의 immutable byte view 반환
// 입력: row: 0부터 시작하는 row index
// 출력: 유효하면 rowStride 크기의 read-only span, 아니면 빈 span
std::span<const std::uint8_t> DisplayFrame::rowBytes(std::uint32_t row) const noexcept
{
    if (row >= m_height)
    {
        return {};
    }
    return bytes().subspan(static_cast<std::size_t>(row) * m_rowStride, m_rowStride);
}

// 목적: validation된 display frame state 저장
// 입력: width/height/rowStride/previewRevision: metadata, bytes: 단독 소유 immutable buffer
// 출력: create()만 구성할 수 있는 frame
DisplayFrame::DisplayFrame(std::uint32_t width,
                           std::uint32_t height,
                           std::size_t rowStride,
                           std::uint64_t previewRevision,
                           std::vector<std::uint8_t> bytes)
    : m_width(width),
      m_height(height),
      m_rowStride(rowStride),
      m_previewRevision(previewRevision),
      m_bytes(std::make_shared<const std::vector<std::uint8_t>>(std::move(bytes)))
{}

}  // namespace flexraw::core::client
