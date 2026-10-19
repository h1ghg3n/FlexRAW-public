#pragma once

#include <cstddef>

namespace flexraw::core::develop
{

struct Rgba8NeighborhoodView
{
    const unsigned char* sourcePixels{nullptr};
    std::ptrdiff_t sourceBytesPerLine{0};
    unsigned char* destinationPixels{nullptr};
    std::ptrdiff_t destinationBytesPerLine{0};
    int width{0};
    int height{0};
};

struct Float32NeighborhoodParameters
{
    float luminanceNoiseReduction{0.0F};
    float colorNoiseReduction{0.0F};
    float clarity{0.0F};
    float sharpeningAmount{0.0F};
    float sharpeningRadius{1.0F};
    float sharpeningDetail{0.0F};
    float sharpeningMasking{0.0F};
    int grantedCpuPermits{1};
};

// 목적: 지정 image 높이를 처리할 bounded Float32 stripe task 수 계산
// 입력: imageHeight — 처리할 전체 row 수
// 출력: 현재 internal stripe decomposition의 task 수
[[nodiscard]] int boundedFloat32NeighborhoodTaskCount(int imageHeight);

// 목적: RGBA8 경계 사이의 NR→Clarity→Sharpening 계산을 bounded Float32 stripe로 실행
// 입력: view — 분리된 source/destination RGBA8 view, params — 보정 강도와 허가된 CPU permit 수
// 출력: 모든 stripe가 join된 destination RGB와 보존된 alpha
void applyBoundedFloat32Neighborhood(const Rgba8NeighborhoodView& view, const Float32NeighborhoodParameters& params);

}  // namespace flexraw::core::develop
