#pragma once

#include <iosfwd>
#include <optional>
#include <string>

#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QString>

namespace flexraw::mcp
{

class McpCatalogTools;
class McpEditorTools;
class McpProjectTools;
class McpSourceResolutionTools;

class McpServer final : public QObject
{
public:
    // 목적: local STDIO MCP transport를 독립된 tool adapter와 stream에 연결
    // 입력: 네 domain tool adapter, serverVersion과 STDIO streams
    // 출력: 주어진 stream lifetime 안에서 동작하는 MCP server
    McpServer(McpCatalogTools& catalogTools,
              McpProjectTools& projectTools,
              McpEditorTools& editorTools,
              McpSourceResolutionTools& sourceTools,
              std::string serverVersion,
              std::istream& input,
              std::ostream& output,
              std::ostream& diagnostics) noexcept;

    // 목적: stdin reader와 Qt event loop를 분리해 MCP request/event/stdout을 한 delivery context에서 직렬화
    // 입력: 없음
    // 출력: 정상 EOF는 0, stream 또는 delivery 실패는 non-zero process code
    [[nodiscard]] int run();

private:
    // 목적: delivery context에서 newline 하나를 parse하고 protocol response 기록
    // 입력: line: newline이 제거된 UTF-8 JSON-RPC bytes
    // 출력: write failure는 server state에 기록되고 이후 command 실행 차단
    void processLine(const std::string& line);

    enum class ConnectionEra
    {
        Unset,
        Legacy,
        Modern,
    };

    // 목적: 단일 JSON-RPC object를 lifecycle 또는 tool handler로 routing
    // 입력: request: parse가 완료된 JSON object
    // 출력: request response 또는 notification이면 빈 값
    [[nodiscard]] std::optional<QJsonObject> handleMessage(const QJsonObject& request);

    // 목적: legacy initialize request에 지원 protocol과 tool capability 응답
    // 입력: request: initialize params와 request identity
    // 출력: initialize result 또는 version/params 오류
    [[nodiscard]] QJsonObject handleInitialize(const QJsonObject& request);

    // 목적: modern MCP client에 지원 version과 tool capability 광고
    // 입력: request: server/discover request와 modern metadata
    // 출력: cache되지 않는 private discovery result 또는 version 오류
    [[nodiscard]] QJsonObject handleDiscover(const QJsonObject& request) const;

    // 목적: 각 domain tool adapter의 descriptor를 하나의 목록으로 조립
    // 입력: request: tools/list identity와 negotiated protocol metadata
    // 출력: 현재 제공되는 tool descriptor result
    [[nodiscard]] QJsonObject handleToolsList(const QJsonObject& request) const;

    // 목적: tool 이름으로 domain adapter를 선택하고 transport result로 포장
    // 입력: request: tools/call identity, tool name과 arguments
    // 출력: 성공/실패 tool result 또는 protocol params 오류
    [[nodiscard]] QJsonObject handleToolsCall(const QJsonObject& request) const;

    // 목적: MCP request가 2026-07-28 per-request metadata를 사용하는지 판정
    // 입력: request: JSON-RPC request
    // 출력: exact modern protocol metadata를 가지면 true
    [[nodiscard]] bool isModernRequest(const QJsonObject& request) const;

    // 목적: JSON-RPC 성공 envelope 생성
    // 입력: id: 원 요청 identity, result: method별 result object
    // 출력: jsonrpc, id와 result를 포함한 response
    [[nodiscard]] static QJsonObject successResponse(const QJsonValue& id, QJsonObject result);

    // 목적: JSON-RPC protocol error envelope 생성
    // 입력: id: 원 요청 identity 또는 null, code/message/data: 표준 오류 정보
    // 출력: jsonrpc, id와 error를 포함한 response
    [[nodiscard]] static QJsonObject errorResponse(const QJsonValue& id,
                                                   int code,
                                                   const QString& message,
                                                   QJsonObject data = {});

    // 목적: response 한 개를 compact UTF-8 JSON line으로 stdout stream에 기록
    // 입력: response: 유효한 MCP JSON-RPC response object
    // 출력: stream 상태가 정상이면 true
    [[nodiscard]] bool writeResponse(const QJsonObject& response);

    McpCatalogTools* m_catalogTools{nullptr};
    McpProjectTools* m_projectTools{nullptr};
    McpEditorTools* m_editorTools{nullptr};
    McpSourceResolutionTools* m_sourceTools{nullptr};
    std::string m_serverVersion;
    std::istream* m_input{nullptr};
    std::ostream* m_output{nullptr};
    std::ostream* m_diagnostics{nullptr};
    std::string m_legacyProtocolVersion;
    ConnectionEra m_connectionEra{ConnectionEra::Unset};
    bool m_legacyInitialized{false};
    bool m_writeFailed{false};
};

}  // namespace flexraw::mcp
