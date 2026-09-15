#pragma once

#include <string>

#include <QJsonObject>
#include <QString>

namespace flexraw::mcp
{

struct McpToolCallResult
{
    bool argumentsValid{false};
    QString validationError;
    bool isError{false};
    QJsonObject structuredContent;
    std::string diagnostic;
};

}  // namespace flexraw::mcp
