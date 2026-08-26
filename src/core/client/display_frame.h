#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace flexraw::core::client
{

enum class DisplayPixelFormat : std::uint8_t
{
    Bgra8,
};

enum class ColorEncoding : std::uint8_t
{
    Srgb,
};

class DisplayFrame final
{
public:
    // 목적: checked BGRA8/top-down/sRGB immutable display frame 생성
    // 입력: width/height: pixel 크기, rowStride: row byte 간격, previewRevision: preview 순서, bytes: owned buffer
    // 출력: contract가 유효하면 shared immutable frame, 아니면 빈 값
    [[nodiscard]] static std::optional<DisplayFrame> create(std::uint32_t width,
                                                            std::uint32_t height,
                                                            std::size_t rowStride,
                                                            std::uint64_t previewRevision,
                                                            std::vector<std::uint8_t> bytes);

    // 목적: frame pixel 너비 반환
    // 입력: 없음
    // 출력: 0보다 큰 pixel 수
    [[nodiscard]] std::uint32_t width() const noexcept;

    // 목적: frame pixel 높이 반환
    // 입력: 없음
    // 출력: 0보다 큰 pixel 수
    [[nodiscard]] std::uint32_t height() const noexcept;

    // 목적: 인접 top-down row 사이의 byte 간격 반환
    // 입력: 없음
    // 출력: width * 4 이상의 stride
    [[nodiscard]] std::size_t rowStride() const noexcept;

    // 목적: frame과 연결된 preview revision 반환
    // 입력: 없음
    // 출력: producer가 전달한 단조 증가 revision
    [[nodiscard]] std::uint64_t previewRevision() const noexcept;

    // 목적: v1 display pixel format 반환
    // 입력: 없음
    // 출력: BGRA8
    [[nodiscard]] DisplayPixelFormat pixelFormat() const noexcept;

    // 목적: v1 display color encoding 반환
    // 입력: 없음
    // 출력: display-ready sRGB
    [[nodiscard]] ColorEncoding colorEncoding() const noexcept;

    // 목적: immutable frame 전체 byte view 반환
    // 입력: 없음
    // 출력: rowStride * height 크기의 read-only span
    [[nodiscard]] std::span<const std::uint8_t> bytes() const noexcept;

    // 목적: top-down row의 immutable byte view 반환
    // 입력: row: 0부터 시작하는 row index
    // 출력: 유효하면 rowStride 크기의 read-only span, 아니면 빈 span
    [[nodiscard]] std::span<const std::uint8_t> rowBytes(std::uint32_t row) const noexcept;

private:
    // 목적: validation된 display frame state 저장
    // 입력: width/height/rowStride/previewRevision: metadata, bytes: 단독 소유 immutable buffer
    // 출력: create()만 구성할 수 있는 frame
    DisplayFrame(std::uint32_t width,
                 std::uint32_t height,
                 std::size_t rowStride,
                 std::uint64_t previewRevision,
                 std::vector<std::uint8_t> bytes);

    std::uint32_t m_width{0};
    std::uint32_t m_height{0};
    std::size_t m_rowStride{0};
    std::uint64_t m_previewRevision{0};
    std::shared_ptr<const std::vector<std::uint8_t>> m_bytes;
};

}  // namespace flexraw::core::client
