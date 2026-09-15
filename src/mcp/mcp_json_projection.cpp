#include "mcp_json_projection.h"

#include <cstddef>

#include <QByteArray>

namespace flexraw::mcp
{

// 목적: client UTF-8 string을 MCP JSON용 QString으로 변환
// 입력: value: byte 길이가 보존된 UTF-8 string
// 출력: 같은 Unicode text를 가진 QString
QString toJsonString(const std::string& value)
{
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

// 목적: MCP JSON QString을 client UTF-8 string으로 변환
// 입력: value: JSON text
// 출력: 같은 Unicode text를 가진 UTF-8 string
std::string fromJsonString(const QString& value)
{
    const QByteArray utf8 = value.toUtf8();
    return {utf8.constData(), static_cast<std::size_t>(utf8.size())};
}

// 목적: JSON string identity를 canonical positive signed 64-bit 값으로 검증·복원
// 입력: value: decimal string JSON 값, parsed: 성공 시 채울 identity
// 출력: leading zero/sign/whitespace/overflow 없이 양수로 변환되면 true
bool parsePositiveIdentity(const QJsonValue& value, std::int64_t& parsed)
{
    if (!value.isString())
    {
        return false;
    }
    const QString text = value.toString();
    if (text.isEmpty() || text.front() < QLatin1Char('1') || text.front() > QLatin1Char('9'))
    {
        return false;
    }
    for (const QChar character : text)
    {
        if (character < QLatin1Char('0') || character > QLatin1Char('9'))
        {
            return false;
        }
    }
    bool converted = false;
    const qlonglong candidate = text.toLongLong(&converted, 10);
    if (!converted || candidate <= 0)
    {
        return false;
    }
    parsed = static_cast<std::int64_t>(candidate);
    return true;
}

}  // namespace flexraw::mcp
