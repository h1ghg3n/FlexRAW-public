#pragma once

#include <vector>

namespace flexraw::core::develop::detail
{

struct LinearRgb
{
    float red{0.0F};
    float green{0.0F};
    float blue{0.0F};
};

struct Float32Region
{
    int width{0};
    int firstRow{0};
    int rowCount{0};
    std::vector<LinearRgb> pixels;
};

// 목적: source halo에서 luminance/color NR을 같은 3x3 평균으로 적용
// 입력: source region, 전체 image 높이, half-open output row 범위, 두 NR 강도
// 출력: 중간 정밀도를 유지한 Float32 NR region
[[nodiscard]] Float32Region applyNoiseReduction(const Float32Region& source,
                                                int imageHeight,
                                                int firstRow,
                                                int lastRow,
                                                float luminanceStrength,
                                                float colorStrength);

}  // namespace flexraw::core::develop::detail
