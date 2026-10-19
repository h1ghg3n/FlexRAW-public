#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "result.h"
#include "rgba8_image_view.h"

namespace flexraw::core::export_
{

enum class PngEncodingErrorCode : std::uint8_t
{
    InvalidImage,
    EncodingFailed,
    OutputTooLarge,
};

struct PngEncodingError
{
    PngEncodingErrorCode code{PngEncodingErrorCode::EncodingFailed};

    [[nodiscard]] bool operator==(const PngEncodingError& other) const = default;
};

class EncodedPng8 final
{
public:
    EncodedPng8(const EncodedPng8&) = delete;
    EncodedPng8& operator=(const EncodedPng8&) = delete;
    EncodedPng8(EncodedPng8&&) noexcept = default;
    EncodedPng8& operator=(EncodedPng8&&) noexcept = default;

    // 목적: encoded PNG pixel 너비 반환
    // 입력: 없음
    // 출력: 0보다 큰 pixel 수
    [[nodiscard]] std::uint32_t width() const noexcept;

    // 목적: encoded PNG pixel 높이 반환
    // 입력: 없음
    // 출력: 0보다 큰 pixel 수
    [[nodiscard]] std::uint32_t height() const noexcept;

    // 목적: owned encoded PNG byte view 반환
    // 입력: 없음
    // 출력: PNG signature부터 IEND까지의 immutable bytes
    [[nodiscard]] std::span<const std::uint8_t> bytes() const noexcept;

private:
    // 목적: 성공적으로 종료된 libpng output과 geometry 저장
    // 입력: bytes: complete PNG payload, width/height: encoded geometry
    // 출력: encodePng8Srgb()만 구성할 수 있는 move-only owner
    EncodedPng8(std::vector<std::uint8_t> bytes, std::uint32_t width, std::uint32_t height) noexcept;

    friend types::Result<EncodedPng8, PngEncodingError> encodePng8Srgb(image::ConstRgba8ImageView source);

    std::vector<std::uint8_t> m_bytes;
    std::uint32_t m_width{0};
    std::uint32_t m_height{0};
};

using PngEncodingResult = types::Result<EncodedPng8, PngEncodingError>;

// 목적: concrete RGBA8/sRGB image를 owned 8-bit PNG payload로 encode
// 입력: source: top-down RGBA8/sRGB/straight-alpha image view
// 출력: complete encoded bytes 또는 neutral codec 오류; 메모리 부족은 예외
[[nodiscard]] PngEncodingResult encodePng8Srgb(image::ConstRgba8ImageView source);

}  // namespace flexraw::core::export_
