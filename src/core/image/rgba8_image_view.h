#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace flexraw::core::image
{

// Top-down row order, R/G/B/A byte order, sRGB-encoded RGB와 straight alpha를 고정하는 read-only view입니다.
class ConstRgba8ImageView final
{
public:
    // 목적: caller-owned top-down RGBA8/sRGB storage의 validated read-only view 생성
    // 입력: width/height: pixel 크기, rowStride: row byte 간격, bytes: 호출 기간 동안 유지되는 backing storage
    // 출력: layout과 storage가 유효하면 bounded view, 아니면 빈 값
    [[nodiscard]] static std::optional<ConstRgba8ImageView> create(std::uint32_t width,
                                                                   std::uint32_t height,
                                                                   std::size_t rowStride,
                                                                   std::span<const std::uint8_t> bytes) noexcept;

    // 목적: image pixel 너비 반환
    // 입력: 없음
    // 출력: 0보다 큰 pixel 수
    [[nodiscard]] std::uint32_t width() const noexcept;

    // 목적: image pixel 높이 반환
    // 입력: 없음
    // 출력: 0보다 큰 pixel 수
    [[nodiscard]] std::uint32_t height() const noexcept;

    // 목적: 인접 top-down row 사이의 byte 간격 반환
    // 입력: 없음
    // 출력: width * 4 이상의 stride
    [[nodiscard]] std::size_t rowStride() const noexcept;

    // 목적: validated image 범위의 immutable byte view 반환
    // 입력: 없음
    // 출력: rowStride * height 크기의 read-only span
    [[nodiscard]] std::span<const std::uint8_t> bytes() const noexcept;

    // 목적: 지정 top-down row의 immutable byte view 반환
    // 입력: row: 0부터 시작하는 row index
    // 출력: 유효하면 width * 4 크기의 pixel-only read-only span, 아니면 빈 span
    [[nodiscard]] std::span<const std::uint8_t> rowBytes(std::uint32_t row) const noexcept;

private:
    // 목적: validation된 caller-owned RGBA8/sRGB view state 저장
    // 입력: width/height/rowStride/byteSize: 검증된 layout, bytes: backing storage
    // 출력: create()만 구성할 수 있는 read-only view
    ConstRgba8ImageView(std::uint32_t width,
                        std::uint32_t height,
                        std::size_t rowStride,
                        std::size_t byteSize,
                        std::span<const std::uint8_t> bytes) noexcept;

    std::uint32_t m_width{0};
    std::uint32_t m_height{0};
    std::size_t m_rowStride{0};
    std::size_t m_byteSize{0};
    std::span<const std::uint8_t> m_bytes;
};

// Top-down row order, R/G/B/A byte order, sRGB-encoded RGB와 straight alpha를 고정하는 mutable view입니다.
class MutableRgba8ImageView final
{
public:
    // 목적: caller-owned top-down RGBA8/sRGB storage의 validated mutable view 생성
    // 입력: width/height: pixel 크기, rowStride: row byte 간격, bytes: 호출 기간 동안 유지되는 backing storage
    // 출력: layout과 storage가 유효하면 bounded view, 아니면 빈 값
    [[nodiscard]] static std::optional<MutableRgba8ImageView> create(std::uint32_t width,
                                                                     std::uint32_t height,
                                                                     std::size_t rowStride,
                                                                     std::span<std::uint8_t> bytes) noexcept;

    // 목적: image pixel 너비 반환
    // 입력: 없음
    // 출력: 0보다 큰 pixel 수
    [[nodiscard]] std::uint32_t width() const noexcept;

    // 목적: image pixel 높이 반환
    // 입력: 없음
    // 출력: 0보다 큰 pixel 수
    [[nodiscard]] std::uint32_t height() const noexcept;

    // 목적: 인접 top-down row 사이의 byte 간격 반환
    // 입력: 없음
    // 출력: width * 4 이상의 stride
    [[nodiscard]] std::size_t rowStride() const noexcept;

    // 목적: validated image 범위의 mutable byte view 반환
    // 입력: 없음
    // 출력: rowStride * height 크기의 writable span
    [[nodiscard]] std::span<std::uint8_t> bytes() const noexcept;

    // 목적: 지정 top-down row의 mutable byte view 반환
    // 입력: row: 0부터 시작하는 row index
    // 출력: 유효하면 width * 4 크기의 pixel-only writable span, 아니면 빈 span
    [[nodiscard]] std::span<std::uint8_t> rowBytes(std::uint32_t row) const noexcept;

private:
    // 목적: validation된 caller-owned RGBA8/sRGB view state 저장
    // 입력: width/height/rowStride/byteSize: 검증된 layout, bytes: backing storage
    // 출력: create()만 구성할 수 있는 mutable view
    MutableRgba8ImageView(std::uint32_t width,
                          std::uint32_t height,
                          std::size_t rowStride,
                          std::size_t byteSize,
                          std::span<std::uint8_t> bytes) noexcept;

    std::uint32_t m_width{0};
    std::uint32_t m_height{0};
    std::size_t m_rowStride{0};
    std::size_t m_byteSize{0};
    std::span<std::uint8_t> m_bytes;
};

}  // namespace flexraw::core::image
