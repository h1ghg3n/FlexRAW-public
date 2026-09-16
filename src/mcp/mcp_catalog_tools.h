#pragma once

#include <QJsonObject>
#include <QString>

#include "catalog_photo_client.h"
#include "mcp_tool_result.h"

namespace flexraw::mcp
{

class McpCatalogTools final
{
public:
    // 목적: MCP Catalog tool projection을 frontend-neutral Catalog client에 연결
    // 입력: catalogPhotoClient: bounded Catalog query consumer
    // 출력: client lifetime 동안 사용할 Catalog MCP adapter
    explicit McpCatalogTools(core::client::ICatalogPhotoClient& catalogPhotoClient) noexcept;

    // 목적: Catalog photo page tool의 stable wire name 반환
    // 입력: 없음
    // 출력: catalog_query_photos
    [[nodiscard]] static QString queryPhotosToolName();

    // 목적: Catalog photo page tool의 schema와 read-only hint 반환
    // 입력: 없음
    // 출력: tools/list에 포함할 descriptor
    [[nodiscard]] QJsonObject queryPhotosDescriptor() const;

    // 목적: MCP arguments를 검증하고 bounded Catalog photo page 조회
    // 입력: arguments: catalog_query_photos JSON arguments
    // 출력: protocol validation 또는 structured success/domain failure
    [[nodiscard]] McpToolCallResult callQueryPhotos(const QJsonObject& arguments) const;

private:
    // 목적: MCP arguments를 frontend-neutral Catalog page request로 복원
    // 입력: arguments: JSON object, request: 채울 client request, errorMessage: validation 설명
    // 출력: 모든 field와 scope 조합이 유효하면 true
    [[nodiscard]] bool parsePhotoPageRequest(const QJsonObject& arguments,
                                             core::client::CatalogPhotoPageRequest& request,
                                             QString& errorMessage) const;

    core::client::ICatalogPhotoClient* m_catalogPhotoClient{nullptr};
};

}  // namespace flexraw::mcp
