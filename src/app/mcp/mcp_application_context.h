#pragma once

#include <memory>
#include <string>

#include "catalog_session_client.h"

namespace flexraw::core::client
{
class ICatalogPhotoClient;
class ICatalogProjectClient;
class IEditorClient;
class ISourceResolutionClient;
class ISourceResolutionEventSource;
}  // namespace flexraw::core::client

namespace flexraw::runtime
{
class ProductRuntime;
}

namespace flexraw::app::mcp
{

class McpApplicationContext final
{
public:
    // 목적: UI 없는 local MCP용 Catalog resource owner 생성
    // 입력: 없음
    // 출력: 아직 Catalog가 열리지 않은 application context
    McpApplicationContext();

    // 목적: Catalog connection과 background resource를 MCP server보다 늦게 정리
    // 입력: 없음
    // 출력: 열린 Catalog와 owned operation resource가 정리됨
    ~McpApplicationContext();

    McpApplicationContext(const McpApplicationContext&) = delete;
    McpApplicationContext& operator=(const McpApplicationContext&) = delete;

    // 목적: 명시적으로 지정된 기존 Catalog를 MCP process lifetime에 맞춰 열기
    // 입력: catalogPath: UTF-8 absolute 또는 working-directory-relative existing file path
    // 출력: normalized open snapshot 또는 validation·not-found·database 오류
    [[nodiscard]] core::client::CatalogSessionResult openExistingCatalog(const std::string& catalogPath);

    // 목적: MCP Catalog read adapter가 사용할 frontend-neutral client 반환
    // 입력: 없음
    // 출력: context lifetime 동안 유효한 Catalog Photo client 참조
    [[nodiscard]] core::client::ICatalogPhotoClient& catalogPhotoClient() noexcept;

    // 목적: MCP Project adapter가 사용할 frontend-neutral client 반환
    // 입력: 없음
    // 출력: context lifetime 동안 유효한 Catalog Project client 참조
    [[nodiscard]] core::client::ICatalogProjectClient& catalogProjectClient() noexcept;

    // 목적: MCP Editor adapter가 사용할 frontend-neutral client 반환
    // 입력: 없음
    // 출력: context lifetime 동안 유효한 Editor client 참조
    [[nodiscard]] core::client::IEditorClient& editorClient() noexcept;

    // 목적: MCP Source Resolution command adapter가 사용할 frontend-neutral client 반환
    // 입력: 없음
    // 출력: context lifetime 동안 유효한 Source Resolution client 참조
    [[nodiscard]] core::client::ISourceResolutionClient& sourceResolutionClient() noexcept;

    // 목적: MCP async delivery adapter가 구독할 frontend-neutral Source Resolution event source 반환
    // 입력: 없음
    // 출력: context lifetime 동안 유효한 ordered event source 참조
    [[nodiscard]] core::client::ISourceResolutionEventSource& sourceResolutionEventSource() noexcept;

private:
    std::unique_ptr<runtime::ProductRuntime> m_productRuntime;
};

}  // namespace flexraw::app::mcp
