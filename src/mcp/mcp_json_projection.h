#pragma once

#include <cstdint>
#include <string>

#include <QJsonValue>
#include <QString>

namespace flexraw::mcp
{

// 목적: client UTF-8 string을 MCP JSON용 QString으로 변환
// 입력: value: byte 길이가 보존된 UTF-8 string
// 출력: 같은 Unicode text를 가진 QString
[[nodiscard]] QString toJsonString(const std::string& value);

// 목적: MCP JSON QString을 client UTF-8 string으로 변환
// 입력: value: JSON text
// 출력: 같은 Unicode text를 가진 UTF-8 string
[[nodiscard]] std::string fromJsonString(const QString& value);

// 목적: JSON string identity를 canonical positive signed 64-bit 값으로 검증·복원
// 입력: value: decimal string JSON 값, parsed: 성공 시 채울 identity
// 출력: leading zero/sign/whitespace/overflow 없이 양수로 변환되면 true
[[nodiscard]] bool parsePositiveIdentity(const QJsonValue& value, std::int64_t& parsed);

}  // namespace flexraw::mcp
