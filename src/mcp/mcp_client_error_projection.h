#pragma once

#include "client_error.h"
#include "mcp_tool_result.h"

namespace flexraw::mcp
{

// 목적: frontend-neutral ClientError를 diagnostic이 제거된 model-safe JSON으로 투영
// 입력: error: common error category와 diagnostic-only technical message
// 출력: stable code와 일반 설명만 포함한 error object
[[nodiscard]] QJsonObject clientErrorToJson(const core::client::ClientError& error);

// 목적: frontend-neutral ClientError를 model-safe MCP tool failure로 투영
// 입력: error: common error category와 diagnostic-only technical message
// 출력: stable code/message structured content와 분리된 diagnostic
[[nodiscard]] McpToolCallResult makeClientErrorToolResult(const core::client::ClientError& error);

}  // namespace flexraw::mcp
