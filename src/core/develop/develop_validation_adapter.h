#pragma once

#include "develop_params.h"
#include "error.h"
#include "result.h"

namespace flexraw::core::develop
{

using DevelopParamsValidationResult = types::Result<types::DevelopParams, types::CoreError>;

// 목적: Develop parameter 전체를 검사하고 기존 CoreError compatibility 결과로 변환
// 입력: params — 검사할 Develop parameter 값
// 출력: 유효한 parameter 복사본 또는 기존 diagnostic을 보존한 구조화된 오류
[[nodiscard]] DevelopParamsValidationResult validateDevelopParams(const types::DevelopParams& params);

}  // namespace flexraw::core::develop
