#pragma once

#include "develop_params.h"

namespace flexraw::benchmarks::render
{

inline constexpr char RepresentativeDevelopProfileId[] = "representative-v1";

// 목적: image regression candidate에 고정할 대표 Develop parameter 생성
// 입력: 없음
// 출력: 주요 Develop stage를 활성화하는 version 1 DevelopParams
[[nodiscard]] core::types::DevelopParams makeRepresentativeDevelopParams();

}  // namespace flexraw::benchmarks::render
