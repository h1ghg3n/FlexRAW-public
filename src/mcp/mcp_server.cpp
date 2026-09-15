#include "mcp_server.h"

#include <cstddef>
#include <istream>
#include <ostream>
#include <thread>
#include <utility>

#include <QByteArray>
#include <QCoreApplication>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QMetaObject>
#include <QStringList>
#include <QThread>

#include "mcp_catalog_tools.h"
#include "mcp_editor_tools.h"
#include "mcp_project_tools.h"
#include "mcp_source_resolution_tools.h"
#include "mcp_tool_result.h"

namespace flexraw::mcp
{
namespace
{

constexpr auto ModernProtocolVersion = "2026-07-28";
constexpr auto DefaultLegacyProtocolVersion = "2025-11-25";
constexpr int JsonRpcParseError = -32700;
constexpr int JsonRpcInvalidRequest = -32600;
constexpr int JsonRpcMethodNotFound = -32601;
constexpr int JsonRpcInvalidParams = -32602;
constexpr int ServerNotInitialized = -32002;
constexpr int UnsupportedProtocolVersion = -32022;

// 목적: std::string byte 길이를 보존해 QString UTF-8 값으로 변환
// 입력: value: server metadata UTF-8 bytes
// 출력: 같은 Unicode text를 가진 QString
[[nodiscard]] QString toQString(const std::string& value)
{
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

// 목적: QString을 byte 길이가 보존된 UTF-8 std::string으로 변환
// 입력: value: negotiated protocol text
// 출력: 같은 Unicode text를 가진 UTF-8 bytes
[[nodiscard]] std::string toStdString(const QString& value)
{
    const QByteArray utf8 = value.toUtf8();
    return {utf8.constData(), static_cast<std::size_t>(utf8.size())};
}

// 목적: legacy initialize에서 지원할 protocol version인지 확인
// 입력: version: client가 요청한 dated MCP version
// 출력: 현재 tool subset과 wire-compatible한 legacy version이면 true
[[nodiscard]] bool isSupportedLegacyVersion(const QString& version)
{
    static const QStringList SupportedVersions{QStringLiteral("2025-11-25"),
                                               QStringLiteral("2025-06-18"),
                                               QStringLiteral("2025-03-26"),
                                               QStringLiteral("2024-11-05")};
    return SupportedVersions.contains(version);
}

// 목적: 2026-era result에 completion/cache/server metadata 추가
// 입력: result: method별 result, serverVersion: server identity version, cacheable: cache hint 대상 여부
// 출력: modern wire requirement를 포함한 result
[[nodiscard]] QJsonObject decorateModernResult(QJsonObject result, const std::string& serverVersion, bool cacheable)
{
    result.insert(QStringLiteral("resultType"), QStringLiteral("complete"));
    if (cacheable)
    {
        result.insert(QStringLiteral("ttlMs"), 0);
        result.insert(QStringLiteral("cacheScope"), QStringLiteral("private"));
    }
    result.insert(QStringLiteral("_meta"),
                  QJsonObject{{QStringLiteral("io.modelcontextprotocol/serverInfo"),
                               QJsonObject{{QStringLiteral("name"), QStringLiteral("Flexraw MCP")},
                                           {QStringLiteral("version"), toQString(serverVersion)}}}});
    return result;
}

}  // namespace

// 목적: local STDIO MCP transport를 독립된 tool adapter와 stream에 연결
// 입력: 네 domain tool adapter, serverVersion과 STDIO streams
// 출력: 주어진 stream lifetime 안에서 동작하는 MCP server
McpServer::McpServer(McpCatalogTools& catalogTools,
                     McpProjectTools& projectTools,
                     McpEditorTools& editorTools,
                     McpSourceResolutionTools& sourceTools,
                     std::string serverVersion,
                     std::istream& input,
                     std::ostream& output,
                     std::ostream& diagnostics) noexcept
    : m_catalogTools(&catalogTools),
      m_projectTools(&projectTools),
      m_editorTools(&editorTools),
      m_sourceTools(&sourceTools),
      m_serverVersion(std::move(serverVersion)),
      m_input(&input),
      m_output(&output),
      m_diagnostics(&diagnostics)
{}

// 목적: stdin reader와 Qt event loop를 분리해 MCP request/event/stdout을 한 delivery context에서 직렬화
// 입력: 없음
// 출력: 정상 EOF는 0, stream 또는 delivery 실패는 non-zero process code
int McpServer::run()
{
    if (QCoreApplication::instance() == nullptr || QThread::currentThread() != thread())
    {
        *m_diagnostics << "MCP serialized delivery requires the QCoreApplication owner thread.\n";
        m_sourceTools->shutdown();
        return 3;
    }

    QEventLoop deliveryLoop;
    int inputExitCode = 0;
    bool deliveryQueueFailed = false;
    std::thread inputReader([this, &deliveryLoop, &inputExitCode, &deliveryQueueFailed] {
        std::string line;
        while (std::getline(*m_input, line))
        {
            const bool delivered = QMetaObject::invokeMethod(
                this, [this, line = std::move(line)] { processLine(line); }, Qt::BlockingQueuedConnection);
            if (!delivered)
            {
                deliveryQueueFailed = true;
                break;
            }
        }
        inputExitCode = m_input->bad() ? 1 : 0;
        const bool shutdownDelivered = QMetaObject::invokeMethod(
            this,
            [this, &deliveryLoop] {
                m_sourceTools->shutdown();
                deliveryLoop.quit();
            },
            Qt::BlockingQueuedConnection);
        if (!shutdownDelivered)
        {
            deliveryQueueFailed = true;
        }
    });

    deliveryLoop.exec();
    inputReader.join();
    if (deliveryQueueFailed)
    {
        return 3;
    }
    return m_writeFailed ? 2 : inputExitCode;
}

// 목적: delivery context에서 newline 하나를 parse하고 protocol response 기록
// 입력: line: newline이 제거된 UTF-8 JSON-RPC bytes
// 출력: write failure는 server state에 기록되고 이후 command 실행 차단
void McpServer::processLine(const std::string& line)
{
    if (m_writeFailed)
    {
        return;
    }
    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(QByteArray(line.data(), static_cast<qsizetype>(line.size())), &parseError);
    if (parseError.error != QJsonParseError::NoError)
    {
        m_writeFailed = !writeResponse(
            errorResponse(QJsonValue(QJsonValue::Null), JsonRpcParseError, QStringLiteral("Parse error")));
        return;
    }
    if (!document.isObject())
    {
        m_writeFailed = !writeResponse(
            errorResponse(QJsonValue(QJsonValue::Null), JsonRpcInvalidRequest, QStringLiteral("Invalid Request")));
        return;
    }
    const std::optional<QJsonObject> response = handleMessage(document.object());
    if (response.has_value())
    {
        m_writeFailed = !writeResponse(*response);
    }
}

// 목적: 단일 JSON-RPC object를 lifecycle 또는 tool handler로 routing
// 입력: request: parse가 완료된 JSON object
// 출력: request response 또는 notification이면 빈 값
std::optional<QJsonObject> McpServer::handleMessage(const QJsonObject& request)
{
    const bool hasId = request.contains(QStringLiteral("id"));
    const QJsonValue id = hasId ? request.value(QStringLiteral("id")) : QJsonValue(QJsonValue::Null);
    if (request.value(QStringLiteral("jsonrpc")).toString() != QStringLiteral("2.0") ||
        !request.value(QStringLiteral("method")).isString())
    {
        return errorResponse(id, JsonRpcInvalidRequest, QStringLiteral("Invalid Request"));
    }

    const QString method = request.value(QStringLiteral("method")).toString();
    if (!hasId)
    {
        if (method == QStringLiteral("notifications/initialized"))
        {
            m_legacyInitialized = !m_legacyProtocolVersion.empty();
        }
        return std::nullopt;
    }
    if (method == QStringLiteral("server/discover"))
    {
        if (m_connectionEra == ConnectionEra::Legacy)
        {
            return errorResponse(id, UnsupportedProtocolVersion, QStringLiteral("Protocol era is already selected"));
        }
        QJsonObject response = handleDiscover(request);
        if (response.contains(QStringLiteral("result")))
        {
            m_connectionEra = ConnectionEra::Modern;
        }
        return response;
    }
    if (method == QStringLiteral("initialize"))
    {
        if (m_connectionEra != ConnectionEra::Unset)
        {
            return errorResponse(id, UnsupportedProtocolVersion, QStringLiteral("Protocol era is already selected"));
        }
        return handleInitialize(request);
    }

    const bool modern = isModernRequest(request);
    const QJsonObject metadata =
        request.value(QStringLiteral("params")).toObject().value(QStringLiteral("_meta")).toObject();
    if (metadata.contains(QStringLiteral("io.modelcontextprotocol/protocolVersion")) && !modern)
    {
        return errorResponse(
            id,
            UnsupportedProtocolVersion,
            QStringLiteral("Unsupported protocol version"),
            QJsonObject{{QStringLiteral("supported"), QJsonArray{QString::fromLatin1(ModernProtocolVersion)}}});
    }
    if ((modern && m_connectionEra == ConnectionEra::Legacy) || (!modern && m_connectionEra == ConnectionEra::Modern))
    {
        return errorResponse(id, UnsupportedProtocolVersion, QStringLiteral("Protocol era is already selected"));
    }
    if (modern)
    {
        m_connectionEra = ConnectionEra::Modern;
    }
    if (!modern && !m_legacyInitialized)
    {
        return errorResponse(id, ServerNotInitialized, QStringLiteral("Server not initialized"));
    }
    if (method == QStringLiteral("ping"))
    {
        QJsonObject result;
        if (modern)
        {
            result = decorateModernResult(std::move(result), m_serverVersion, false);
        }
        return successResponse(id, std::move(result));
    }
    if (method == QStringLiteral("tools/list"))
    {
        return handleToolsList(request);
    }
    if (method == QStringLiteral("tools/call"))
    {
        return handleToolsCall(request);
    }
    return errorResponse(id, JsonRpcMethodNotFound, QStringLiteral("Method not found"));
}

// 목적: legacy initialize request에 지원 protocol과 tool capability 응답
// 입력: request: initialize params와 request identity
// 출력: initialize result 또는 version/params 오류
QJsonObject McpServer::handleInitialize(const QJsonObject& request)
{
    const QJsonValue id = request.value(QStringLiteral("id"));
    if (!request.value(QStringLiteral("params")).isObject())
    {
        return errorResponse(id, JsonRpcInvalidParams, QStringLiteral("Invalid initialize params"));
    }
    const QString requestedVersion =
        request.value(QStringLiteral("params")).toObject().value(QStringLiteral("protocolVersion")).toString();
    if (!isSupportedLegacyVersion(requestedVersion))
    {
        return errorResponse(
            id,
            JsonRpcInvalidParams,
            QStringLiteral("Unsupported protocol version"),
            QJsonObject{{QStringLiteral("supported"), QJsonArray{QString::fromLatin1(DefaultLegacyProtocolVersion)}}});
    }
    m_legacyProtocolVersion = toStdString(requestedVersion);
    m_connectionEra = ConnectionEra::Legacy;
    m_legacyInitialized = false;
    const QJsonObject result{
        {QStringLiteral("protocolVersion"), requestedVersion},
        {QStringLiteral("capabilities"), QJsonObject{{QStringLiteral("tools"), QJsonObject{}}}},
        {QStringLiteral("serverInfo"),
         QJsonObject{{QStringLiteral("name"), QStringLiteral("Flexraw MCP")},
                     {QStringLiteral("version"), toQString(m_serverVersion)}}},
        {QStringLiteral("instructions"),
         QStringLiteral("Use bounded Flexraw Catalog and Editor tools from the explicitly opened Catalog.")}};
    return successResponse(id, result);
}

// 목적: modern MCP client에 지원 version과 tool capability 광고
// 입력: request: server/discover request와 modern metadata
// 출력: cache되지 않는 private discovery result 또는 version 오류
QJsonObject McpServer::handleDiscover(const QJsonObject& request) const
{
    const QJsonValue id = request.value(QStringLiteral("id"));
    if (!isModernRequest(request))
    {
        return errorResponse(
            id,
            UnsupportedProtocolVersion,
            QStringLiteral("Unsupported protocol version"),
            QJsonObject{{QStringLiteral("supported"), QJsonArray{QString::fromLatin1(ModernProtocolVersion)}}});
    }
    QJsonObject result{
        {QStringLiteral("supportedVersions"), QJsonArray{QString::fromLatin1(ModernProtocolVersion)}},
        {QStringLiteral("capabilities"), QJsonObject{{QStringLiteral("tools"), QJsonObject{}}}},
        {QStringLiteral("instructions"),
         QStringLiteral("Use bounded Flexraw Catalog and Editor tools from the explicitly opened Catalog.")}};
    return successResponse(id, decorateModernResult(std::move(result), m_serverVersion, true));
}

// 목적: 각 domain tool adapter의 descriptor를 하나의 목록으로 조립
// 입력: request: tools/list identity와 negotiated protocol metadata
// 출력: 현재 제공되는 tool descriptor result
QJsonObject McpServer::handleToolsList(const QJsonObject& request) const
{
    QJsonArray tools{m_catalogTools->queryPhotosDescriptor(),
                     m_projectTools->listProjectsDescriptor(),
                     m_editorTools->getStateDescriptor(),
                     m_sourceTools->eventsDescriptor()};
    if (m_projectTools->writesEnabled())
    {
        tools.append(m_projectTools->addPhotoToProjectDescriptor());
    }
    if (m_editorTools->writesEnabled())
    {
        tools.append(m_editorTools->selectPhotoDescriptor());
        tools.append(m_editorTools->setExposureDescriptor());
    }
    if (m_sourceTools->writesEnabled())
    {
        tools.append(m_sourceTools->cancelRequestDescriptor());
    }
    QJsonObject result{{QStringLiteral("tools"), std::move(tools)}};
    if (isModernRequest(request))
    {
        result = decorateModernResult(std::move(result), m_serverVersion, true);
    }
    return successResponse(request.value(QStringLiteral("id")), std::move(result));
}

// 목적: tool 이름으로 domain adapter를 선택하고 transport result로 포장
// 입력: request: tools/call identity, tool name과 arguments
// 출력: 성공/실패 tool result 또는 protocol params 오류
QJsonObject McpServer::handleToolsCall(const QJsonObject& request) const
{
    const QJsonValue id = request.value(QStringLiteral("id"));
    if (!request.value(QStringLiteral("params")).isObject())
    {
        return errorResponse(id, JsonRpcInvalidParams, QStringLiteral("Invalid tools/call params"));
    }
    const QJsonObject params = request.value(QStringLiteral("params")).toObject();
    if (!params.value(QStringLiteral("name")).isString() || !params.value(QStringLiteral("arguments")).isObject())
    {
        return errorResponse(id, JsonRpcInvalidParams, QStringLiteral("Unknown tool or invalid arguments"));
    }

    const QString toolName = params.value(QStringLiteral("name")).toString();
    const QJsonObject arguments = params.value(QStringLiteral("arguments")).toObject();
    McpToolCallResult called;
    if (toolName == McpCatalogTools::queryPhotosToolName())
    {
        called = m_catalogTools->callQueryPhotos(arguments);
    }
    else if (toolName == McpProjectTools::listProjectsToolName())
    {
        called = m_projectTools->callListProjects(arguments);
    }
    else if (toolName == McpProjectTools::addPhotoToProjectToolName())
    {
        called = m_projectTools->callAddPhotoToProject(arguments);
    }
    else if (toolName == McpEditorTools::getStateToolName())
    {
        called = m_editorTools->callGetState(arguments);
    }
    else if (toolName == McpEditorTools::selectPhotoToolName())
    {
        called = m_editorTools->callSelectPhoto(arguments);
    }
    else if (toolName == McpEditorTools::setExposureToolName())
    {
        called = m_editorTools->callSetExposure(arguments);
    }
    else if (toolName == McpSourceResolutionTools::eventsToolName())
    {
        called = m_sourceTools->callEvents(arguments);
    }
    else if (toolName == McpSourceResolutionTools::cancelRequestToolName())
    {
        called = m_sourceTools->callCancelRequest(arguments);
    }
    else
    {
        return errorResponse(id, JsonRpcInvalidParams, QStringLiteral("Unknown tool or invalid arguments"));
    }
    if (!called.argumentsValid)
    {
        return errorResponse(id, JsonRpcInvalidParams, called.validationError);
    }
    if (!called.diagnostic.empty())
    {
        *m_diagnostics << toStdString(toolName) << " failed: " << called.diagnostic << '\n';
    }

    const QString text = QString::fromUtf8(QJsonDocument(called.structuredContent).toJson(QJsonDocument::Compact));
    QJsonObject result{
        {QStringLiteral("content"),
         QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("text")}, {QStringLiteral("text"), text}}}},
        {QStringLiteral("structuredContent"), called.structuredContent},
        {QStringLiteral("isError"), called.isError}};
    if (isModernRequest(request))
    {
        result = decorateModernResult(std::move(result), m_serverVersion, false);
    }
    return successResponse(id, std::move(result));
}

// 목적: MCP request가 2026-07-28 per-request metadata를 사용하는지 판정
// 입력: request: JSON-RPC request
// 출력: exact modern protocol metadata를 가지면 true
bool McpServer::isModernRequest(const QJsonObject& request) const
{
    const QJsonObject params = request.value(QStringLiteral("params")).toObject();
    const QJsonObject metadata = params.value(QStringLiteral("_meta")).toObject();
    return metadata.value(QStringLiteral("io.modelcontextprotocol/protocolVersion")).toString() ==
           QString::fromLatin1(ModernProtocolVersion);
}

// 목적: JSON-RPC 성공 envelope 생성
// 입력: id: 원 요청 identity, result: method별 result object
// 출력: jsonrpc, id와 result를 포함한 response
QJsonObject McpServer::successResponse(const QJsonValue& id, QJsonObject result)
{
    return QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                       {QStringLiteral("id"), id},
                       {QStringLiteral("result"), std::move(result)}};
}

// 목적: JSON-RPC protocol error envelope 생성
// 입력: id: 원 요청 identity 또는 null, code/message/data: 표준 오류 정보
// 출력: jsonrpc, id와 error를 포함한 response
QJsonObject McpServer::errorResponse(const QJsonValue& id, int code, const QString& message, QJsonObject data)
{
    QJsonObject error{{QStringLiteral("code"), code}, {QStringLiteral("message"), message}};
    if (!data.isEmpty())
    {
        error.insert(QStringLiteral("data"), std::move(data));
    }
    return QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                       {QStringLiteral("id"), id},
                       {QStringLiteral("error"), error}};
}

// 목적: response 한 개를 compact UTF-8 JSON line으로 stdout stream에 기록
// 입력: response: 유효한 MCP JSON-RPC response object
// 출력: stream 상태가 정상이면 true
bool McpServer::writeResponse(const QJsonObject& response)
{
    const QByteArray encoded = QJsonDocument(response).toJson(QJsonDocument::Compact);
    m_output->write(encoded.constData(), encoded.size());
    *m_output << '\n';
    m_output->flush();
    return m_output->good();
}

}  // namespace flexraw::mcp
