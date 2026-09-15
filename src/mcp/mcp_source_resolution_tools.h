#pragma once

#include <cstddef>
#include <deque>
#include <optional>
#include <string>

#include <QString>

#include "mcp_tool_result.h"
#include "source_resolution_client.h"

namespace flexraw::mcp
{

class McpSourceResolutionTools final
{
public:
    // 목적: Source Resolution command/event contract를 bounded MCP event window와 write policy에 연결
    // 입력: sourceClient: cancel owner, eventSource: ordered lifecycle source, writesEnabled: process write opt-in
    // 출력: event source lifetime 안에서 사용할 MCP adapter
    McpSourceResolutionTools(core::client::ISourceResolutionClient& sourceClient,
                             core::client::ISourceResolutionEventSource& eventSource,
                             bool writesEnabled);

    // 목적: queued callback보다 먼저 subscription을 해제하고 adapter lifetime 종료
    // 입력: 없음
    // 출력: destruction 이후 Source Resolution callback 없음
    ~McpSourceResolutionTools();

    McpSourceResolutionTools(const McpSourceResolutionTools&) = delete;
    McpSourceResolutionTools& operator=(const McpSourceResolutionTools&) = delete;

    // 목적: bounded Source Resolution event 조회 tool의 stable wire name 반환
    // 입력: 없음
    // 출력: source_resolution_events
    [[nodiscard]] static QString eventsToolName();

    // 목적: Source Resolution cancellation tool의 stable wire name 반환
    // 입력: 없음
    // 출력: source_cancel_request
    [[nodiscard]] static QString cancelRequestToolName();

    // 목적: bounded event window tool의 schema와 read-only hint 반환
    // 입력: 없음
    // 출력: tools/list에 포함할 descriptor
    [[nodiscard]] QJsonObject eventsDescriptor() const;

    // 목적: Source Resolution cancellation tool의 schema와 mutation hint 반환
    // 입력: 없음
    // 출력: write-enabled tools/list에 포함할 descriptor
    [[nodiscard]] QJsonObject cancelRequestDescriptor() const;

    // 목적: retained Source Resolution event를 sequence 이후 bounded window로 투영
    // 입력: arguments: optional after_sequence decimal string과 limit
    // 출력: ordered event window 또는 validation/subscription failure
    [[nodiscard]] McpToolCallResult callEvents(const QJsonObject& arguments) const;

    // 목적: write opt-in을 확인하고 accepted Source Resolution request 취소
    // 입력: arguments: positive decimal request_id
    // 출력: cancelled identity 또는 validation/domain failure
    [[nodiscard]] McpToolCallResult callCancelRequest(const QJsonObject& arguments) const;

    // 목적: process가 명시적 cancellation tool 광고를 허용했는지 확인
    // 입력: 없음
    // 출력: --allow-write가 지정됐으면 true
    [[nodiscard]] bool writesEnabled() const noexcept;

    // 목적: Source Resolution event subscription 구성 성공 여부 확인
    // 입력: 없음
    // 출력: ordered event를 받을 준비가 됐으면 true
    [[nodiscard]] bool ready() const noexcept;

    // 목적: startup subscription failure의 diagnostic-only message 조회
    // 입력: 없음
    // 출력: ready 상태면 빈 string, 실패하면 technical message
    [[nodiscard]] const std::string& startupDiagnostic() const noexcept;

    // 목적: EOF/process shutdown 전에 queued event와 이후 callback 전달 차단
    // 입력: 없음
    // 출력: idempotent inactive subscription 상태
    void shutdown() noexcept;

private:
    // 목적: delivery context에서 immutable event를 bounded retained window에 추가
    // 입력: event: ordered Source Resolution lifecycle event
    // 출력: capacity를 넘은 oldest event가 제거된 최신 window
    void recordEvent(const core::client::SourceResolutionEvent& event);

    static constexpr std::size_t RetainedEventCapacity = 128;
    static constexpr int DefaultEventLimit = 32;
    static constexpr int MaximumEventLimit = 64;

    core::client::ISourceResolutionClient* m_sourceClient{nullptr};
    core::client::SourceResolutionSubscriptionHandle m_subscription;
    std::deque<core::client::SourceResolutionEvent> m_events;
    std::optional<core::client::ClientError> m_subscriptionError;
    std::string m_startupDiagnostic;
    bool m_writesEnabled{false};
    bool m_shuttingDown{false};
};

}  // namespace flexraw::mcp
