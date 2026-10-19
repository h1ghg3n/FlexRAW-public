#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>

#include <QJsonObject>
#include <QString>

#include "export_client.h"
#include "mcp_tool_result.h"

namespace flexraw::core::client
{
class IEditorClient;
}

namespace flexraw::mcp
{

class McpExportTools final
{
public:
    // 목적: current Editor selection과 Export lifecycle을 bounded Local JPEG MCP surface에 연결
    // 입력: editorClient/exportClient/eventSource: 공유 Product Contract, writesEnabled: process write opt-in
    // 출력: event source lifetime 안에서 사용할 MCP Export adapter
    McpExportTools(core::client::IEditorClient& editorClient,
                   core::client::IExportClient& exportClient,
                   core::client::IExportEventSource& eventSource,
                   bool writesEnabled);

    // 목적: queued callback보다 먼저 subscription을 해제하고 adapter lifetime 종료
    // 입력: 없음
    // 출력: destruction 이후 Export callback 없음
    ~McpExportTools();

    McpExportTools(const McpExportTools&) = delete;
    McpExportTools& operator=(const McpExportTools&) = delete;

    // 목적: selected-photo Local JPEG submit tool의 stable wire name 반환
    // 입력: 없음
    // 출력: export_selected_photo_local
    [[nodiscard]] static QString submitSelectedPhotoToolName();

    // 목적: bounded Export event poll tool의 stable wire name 반환
    // 입력: 없음
    // 출력: export_events
    [[nodiscard]] static QString eventsToolName();

    // 목적: Export cancellation tool의 stable wire name 반환
    // 입력: 없음
    // 출력: export_cancel_request
    [[nodiscard]] static QString cancelRequestToolName();

    // 목적: Local JPEG artifact write tool의 schema와 mutation hint 반환
    // 입력: 없음
    // 출력: write-enabled tools/list에 포함할 descriptor
    [[nodiscard]] QJsonObject submitSelectedPhotoDescriptor() const;

    // 목적: bounded Export event window tool의 schema와 read-only hint 반환
    // 입력: 없음
    // 출력: tools/list에 포함할 descriptor
    [[nodiscard]] QJsonObject eventsDescriptor() const;

    // 목적: Export cancellation tool의 schema와 mutation hint 반환
    // 입력: 없음
    // 출력: write-enabled tools/list에 포함할 descriptor
    [[nodiscard]] QJsonObject cancelRequestDescriptor() const;

    // 목적: current Editor selection과 in-memory Develop state를 Local JPEG Export로 제출
    // 입력: arguments: output_locator와 optional jpeg_quality
    // 출력: accepted request identity와 requested artifact path 또는 validation/domain failure
    [[nodiscard]] McpToolCallResult callSubmitSelectedPhoto(const QJsonObject& arguments) const;

    // 목적: retained Export event를 sequence 이후 bounded window로 투영
    // 입력: arguments: optional after_sequence decimal string과 limit
    // 출력: ordered event window 또는 validation/subscription failure
    [[nodiscard]] McpToolCallResult callEvents(const QJsonObject& arguments) const;

    // 목적: write opt-in을 확인하고 accepted Export request 취소
    // 입력: arguments: positive decimal request_id
    // 출력: cancelled identity 또는 validation/domain failure
    [[nodiscard]] McpToolCallResult callCancelRequest(const QJsonObject& arguments) const;

    // 목적: process가 Export artifact write와 cancellation tool 광고를 허용했는지 확인
    // 입력: 없음
    // 출력: --allow-write가 지정됐으면 true
    [[nodiscard]] bool writesEnabled() const noexcept;

    // 목적: Export event subscription 구성 성공 여부 확인
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
    // 입력: event: ordered Export lifecycle event
    // 출력: capacity를 넘은 oldest event가 제거된 최신 window
    void recordEvent(const core::client::ExportEvent& event);

    static constexpr std::size_t RetainedEventCapacity = 128;
    static constexpr int DefaultEventLimit = 32;
    static constexpr int MaximumEventLimit = 64;

    core::client::IEditorClient* m_editorClient{nullptr};
    core::client::IExportClient* m_exportClient{nullptr};
    core::client::ExportSubscriptionHandle m_subscription;
    std::deque<core::client::ExportEvent> m_events;
    std::optional<core::client::ClientError> m_subscriptionError;
    std::string m_startupDiagnostic;
    bool m_writesEnabled{false};
    bool m_shuttingDown{false};
};

}  // namespace flexraw::mcp
