#pragma once

#include <QJsonObject>
#include <QString>

#include "editor_client.h"
#include "mcp_tool_result.h"

namespace flexraw::mcp
{

class McpEditorTools final
{
public:
    // 목적: MCP Editor projection을 shared frontend-neutral client에 연결
    // 입력: editorClient: process-scoped Editor consumer, writesEnabled: Editor command opt-in
    // 출력: client lifetime 동안 사용할 Editor MCP adapter
    McpEditorTools(core::client::IEditorClient& editorClient, bool writesEnabled) noexcept;

    // 목적: Editor snapshot tool의 stable wire name 반환
    // 입력: 없음
    // 출력: editor_get_state
    [[nodiscard]] static QString getStateToolName();

    // 목적: Photo selection tool의 stable wire name 반환
    // 입력: 없음
    // 출력: editor_select_photo
    [[nodiscard]] static QString selectPhotoToolName();

    // 목적: absolute exposure update tool의 stable wire name 반환
    // 입력: 없음
    // 출력: editor_set_exposure
    [[nodiscard]] static QString setExposureToolName();

    // 목적: immutable Editor snapshot tool schema 반환
    // 입력: 없음
    // 출력: tools/list descriptor
    [[nodiscard]] QJsonObject getStateDescriptor() const;

    // 목적: stable Photo selection command schema 반환
    // 입력: 없음
    // 출력: tools/list descriptor
    [[nodiscard]] QJsonObject selectPhotoDescriptor() const;

    // 목적: session-only absolute exposure command schema 반환
    // 입력: 없음
    // 출력: tools/list descriptor
    [[nodiscard]] QJsonObject setExposureDescriptor() const;

    // 목적: 현재 Editor immutable snapshot을 JSON으로 투영
    // 입력: arguments: 비어 있어야 하는 tool arguments
    // 출력: protocol validation 또는 structured snapshot
    [[nodiscard]] McpToolCallResult callGetState(const QJsonObject& arguments) const;

    // 목적: stable PhotoId를 기존 EditorOrchestrator session에 선택
    // 입력: arguments: decimal string photo_id
    // 출력: protocol validation 또는 structured snapshot/domain failure
    [[nodiscard]] McpToolCallResult callSelectPhoto(const QJsonObject& arguments) const;

    // 목적: 현재 snapshot의 나머지 Develop 값을 보존하며 absolute exposure 갱신
    // 입력: arguments: finite JSON number exposure_ev
    // 출력: protocol validation 또는 structured snapshot/domain failure
    [[nodiscard]] McpToolCallResult callSetExposure(const QJsonObject& arguments) const;

    // 목적: process가 source observation 가능한 Editor command 광고를 허용했는지 확인
    // 입력: 없음
    // 출력: --allow-write가 지정됐으면 true
    [[nodiscard]] bool writesEnabled() const noexcept;

private:
    core::client::IEditorClient* m_editorClient{nullptr};
    bool m_writesEnabled{false};
};

}  // namespace flexraw::mcp
