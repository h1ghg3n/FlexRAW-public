#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "rgba8_image_view.h"

namespace flexraw::core::image
{

// Top-down RGBA8/sRGB/straight-alpha pixel storage를 단일 owner로 보유합니다.
class OwnedRgba8Image final
{
public:
    OwnedRgba8Image(const OwnedRgba8Image&) = delete;
    OwnedRgba8Image& operator=(const OwnedRgba8Image&) = delete;
    OwnedRgba8Image(OwnedRgba8Image&&) noexcept = default;
    OwnedRgba8Image& operator=(OwnedRgba8Image&&) noexcept = default;

    // 목적: caller가 제공한 byte storage로 validated RGBA8 image owner 생성
    // 입력: width/height/rowStride: image layout, bytes: 이동할 exact backing storage
    // 출력: geometry와 storage가 유효하면 move-only owner, 아니면 빈 값
    [[nodiscard]] static std::optional<OwnedRgba8Image> create(std::uint32_t width,
                                                               std::uint32_t height,
                                                               std::size_t rowStride,
                                                               std::vector<std::uint8_t> bytes) noexcept;

    // 목적: tight RGBA8 row를 가진 zero-initialized image storage 생성
    // 입력: width/height: 0보다 큰 pixel geometry
    // 출력: geometry가 유효하면 allocated owner, 아니면 빈 값; 메모리 부족은 예외
    [[nodiscard]] static std::optional<OwnedRgba8Image> allocate(std::uint32_t width, std::uint32_t height);

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

    // 목적: owned pixel storage의 immutable byte view 반환
    // 입력: 없음
    // 출력: rowStride * height 크기의 read-only span
    [[nodiscard]] std::span<const std::uint8_t> bytes() const noexcept;

    // 목적: owned pixel storage의 mutable byte view 반환
    // 입력: 없음
    // 출력: rowStride * height 크기의 writable span
    [[nodiscard]] std::span<std::uint8_t> mutableBytes() noexcept;

    // 목적: owned storage를 가리키는 validated read-only processing view 생성
    // 입력: 없음
    // 출력: 현재 owner lifetime 동안 유효한 RGBA8 view
    [[nodiscard]] ConstRgba8ImageView constView() const noexcept;

    // 목적: owned storage를 가리키는 validated mutable processing view 생성
    // 입력: 없음
    // 출력: 현재 owner lifetime 동안 유효한 RGBA8 view
    [[nodiscard]] MutableRgba8ImageView mutableView() noexcept;

private:
    // 목적: validation을 통과한 concrete RGBA8 storage 저장
    // 입력: width/height/rowStride: validated layout, bytes: exact owned storage
    // 출력: create()와 allocate()만 구성할 수 있는 owner
    OwnedRgba8Image(std::uint32_t width,
                    std::uint32_t height,
                    std::size_t rowStride,
                    std::vector<std::uint8_t> bytes) noexcept;

    std::uint32_t m_width{0};
    std::uint32_t m_height{0};
    std::size_t m_rowStride{0};
    std::vector<std::uint8_t> m_bytes;
};

}  // namespace flexraw::core::image
