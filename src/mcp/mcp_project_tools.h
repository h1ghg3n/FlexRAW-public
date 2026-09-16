#pragma once

#include <QString>

#include "catalog_project_client.h"
#include "mcp_tool_result.h"

namespace flexraw::mcp
{

class McpProjectTools final
{
public:
    // 목적: MCP Project projection을 frontend-neutral Project client와 write policy에 연결
    // 입력: projectClient: Project command consumer, writesEnabled: explicit process write opt-in
    // 출력: client lifetime 동안 사용할 Project MCP adapter
    McpProjectTools(core::client::ICatalogProjectClient& projectClient, bool writesEnabled) noexcept;

    // 목적: Project list tool의 stable wire name 반환
    // 입력: 없음
    // 출력: catalog_list_projects
    [[nodiscard]] static QString listProjectsToolName();

    // 목적: idempotent membership add tool의 stable wire name 반환
    // 입력: 없음
    // 출력: catalog_add_photo_to_project
    [[nodiscard]] static QString addPhotoToProjectToolName();

    // 목적: Project list tool의 schema와 read-only hint 반환
    // 입력: 없음
    // 출력: tools/list에 포함할 descriptor
    [[nodiscard]] QJsonObject listProjectsDescriptor() const;

    // 목적: Project membership add tool의 schema와 mutation hint 반환
    // 입력: 없음
    // 출력: write-enabled tools/list에 포함할 descriptor
    [[nodiscard]] QJsonObject addPhotoToProjectDescriptor() const;

    // 목적: Project 목록을 frontend-neutral client에서 조회해 JSON으로 투영
    // 입력: arguments: 비어 있어야 하는 tool arguments
    // 출력: protocol validation 또는 structured success/domain failure
    [[nodiscard]] McpToolCallResult callListProjects(const QJsonObject& arguments) const;

    // 목적: write opt-in을 확인하고 idempotent Project membership 추가 수행
    // 입력: arguments: decimal string project_id와 photo_id
    // 출력: protocol validation 또는 structured success/domain failure
    [[nodiscard]] McpToolCallResult callAddPhotoToProject(const QJsonObject& arguments) const;

    // 목적: process가 명시적 write tool 광고를 허용했는지 확인
    // 입력: 없음
    // 출력: --allow-write가 지정됐으면 true
    [[nodiscard]] bool writesEnabled() const noexcept;

private:
    core::client::ICatalogProjectClient* m_projectClient{nullptr};
    bool m_writesEnabled{false};
};

}  // namespace flexraw::mcp
