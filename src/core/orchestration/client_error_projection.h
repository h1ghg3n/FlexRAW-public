#pragma once

#include "client_error.h"
#include "error.h"

namespace flexraw::core::orchestration
{

// 목적: Core 오류 분류와 진단문을 Qt-free client 오류로 투영
// 입력: error: Orchestration 또는 domain 오류
// 출력: 같은 분류와 UTF-8 technical message를 가진 client 오류
[[nodiscard]] client::ClientError toClientError(const types::CoreError& error);

// 목적: Qt-free client 오류를 transitional Qt 내부 오류로 복원
// 입력: error: client command가 반환한 오류
// 출력: 같은 분류와 QString technical message를 가진 Core 오류
[[nodiscard]] types::CoreError fromClientError(const client::ClientError& error);

}  // namespace flexraw::core::orchestration
