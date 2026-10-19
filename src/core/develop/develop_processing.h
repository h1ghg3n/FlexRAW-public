#pragma once

#include <cstdint>
#include <variant>

#include "develop_params.h"
#include "result.h"
#include "rgba8_image_view.h"

namespace flexraw::core::develop
{

enum class DevelopProcessingErrorCode : std::uint8_t
{
    InvalidParameters,
    InvalidExecutionGrant,
    GeometryMismatch,
    AliasedStorage,
    UnsupportedGeometry,
};

struct DevelopProcessingError
{
    DevelopProcessingErrorCode code{DevelopProcessingErrorCode::InvalidParameters};

    [[nodiscard]] bool operator==(const DevelopProcessingError& other) const = default;
};

using DevelopProcessingResult = types::Result<std::monostate, DevelopProcessingError>;

// 목적: caller-owned RGBA8/sRGB source를 별도 destination에 Develop semantics로 처리
// 입력: source/destination: non-overlapping validated view, params: 보정값, grantedCpuPermits: 허가된 CPU 상한
// 출력: destination 갱신 성공 또는 neutral processing 오류, 메모리 부족은 예외
[[nodiscard]] DevelopProcessingResult applyDevelop(image::ConstRgba8ImageView source,
                                                   image::MutableRgba8ImageView destination,
                                                   const types::DevelopParams& params,
                                                   int grantedCpuPermits);

// 목적: Develop parameter와 image 높이에서 실제 사용할 수 있는 최대 CPU permit 수 계산
// 입력: imageHeight/params — processing geometry와 활성화된 neighborhood 보정
// 출력: 현재 bounded decomposition이 활용할 수 있는 1 이상의 permit 수
[[nodiscard]] int maximumUsefulDevelopCpuPermits(int imageHeight, const types::DevelopParams& params);

}  // namespace flexraw::core::develop
