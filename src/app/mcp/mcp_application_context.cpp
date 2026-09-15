#include "mcp_application_context.h"

#include <cstddef>

#include <QByteArray>
#include <QFileInfo>
#include <QString>

#include "catalog_orchestrator.h"
#include "client_error_projection.h"
#include "editor_orchestrator.h"
#include "preview_pipeline.h"
#include "product_runtime.h"

namespace flexraw::app::mcp
{
namespace
{

class McpUnavailablePreviewPipeline final : public core::orchestration::IPreviewPipeline
{
public:
    // 목적: viewport를 제공하지 않는 MCP Editor에서 예상 밖 preview 실행을 명시적으로 거부
    // 입력: request/tier/cancellationToken: 현재 MCP surface에서 도달하지 않아 사용하지 않음
    // 출력: preview payload가 M1.6 범위 밖임을 나타내는 UnsupportedFormat 오류
    [[nodiscard]] core::orchestration::PreviewPipelineResult render(const core::orchestration::PreviewRequest&,
                                                                    core::orchestration::PreviewTier,
                                                                    const core::types::CancellationToken&) override
    {
        return core::orchestration::PreviewPipelineResult::failure(
            {core::types::ErrorCode::UnsupportedFormat,
             QStringLiteral("The MCP Editor surface does not provide a preview viewport.")});
    }
};

// 목적: QString path를 byte 길이가 보존된 UTF-8 client string으로 변환
// 입력: value: normalized Qt path
// 출력: 같은 path bytes를 가진 UTF-8 string
[[nodiscard]] std::string toClientString(const QString& value)
{
    const QByteArray utf8 = value.toUtf8();
    return {utf8.constData(), static_cast<std::size_t>(utf8.size())};
}

}  // namespace

// 목적: UI 없는 local MCP용 Catalog resource owner 생성
// 입력: 없음
// 출력: 아직 Catalog가 열리지 않은 application context
McpApplicationContext::McpApplicationContext()
    : m_productRuntime(std::make_unique<runtime::ProductRuntime>(std::make_unique<McpUnavailablePreviewPipeline>()))
{}

// 목적: Catalog connection과 background resource를 MCP server보다 늦게 정리
// 입력: 없음
// 출력: 열린 Catalog와 owned operation resource가 정리됨
McpApplicationContext::~McpApplicationContext() = default;

// 목적: 명시적으로 지정된 기존 Catalog를 MCP process lifetime에 맞춰 열기
// 입력: catalogPath: UTF-8 absolute 또는 working-directory-relative existing file path
// 출력: normalized open snapshot 또는 validation·not-found·database 오류
core::client::CatalogSessionResult McpApplicationContext::openExistingCatalog(const std::string& catalogPath)
{
    if (catalogPath.empty())
    {
        return core::client::CatalogSessionResult::failure(
            {core::client::ClientErrorCode::InvalidArgument, "Catalog path is empty."});
    }

    const QString requestedPath = QString::fromUtf8(catalogPath.data(), static_cast<qsizetype>(catalogPath.size()));
    const QFileInfo catalogInfo(requestedPath);
    if (!catalogInfo.exists() || !catalogInfo.isFile())
    {
        return core::client::CatalogSessionResult::failure(
            {core::client::ClientErrorCode::NotFound, "Catalog file does not exist."});
    }
    if (!catalogInfo.isReadable())
    {
        return core::client::CatalogSessionResult::failure(
            {core::client::ClientErrorCode::PermissionDenied, "Catalog file is not readable."});
    }

    const core::orchestration::CatalogSessionResult opened =
        m_productRuntime->catalogOrchestrator().openCatalog(catalogInfo.absoluteFilePath());
    if (opened.hasError())
    {
        return core::client::CatalogSessionResult::failure(core::orchestration::toClientError(opened.error()));
    }

    return core::client::CatalogSessionResult::success(
        {opened.value().isOpen, toClientString(opened.value().catalogPath)});
}

// 목적: MCP Catalog read adapter가 사용할 frontend-neutral client 반환
// 입력: 없음
// 출력: context lifetime 동안 유효한 Catalog Photo client 참조
core::client::ICatalogPhotoClient& McpApplicationContext::catalogPhotoClient() noexcept
{
    return m_productRuntime->catalogOrchestrator();
}

// 목적: MCP Project adapter가 사용할 frontend-neutral client 반환
// 입력: 없음
// 출력: context lifetime 동안 유효한 Catalog Project client 참조
core::client::ICatalogProjectClient& McpApplicationContext::catalogProjectClient() noexcept
{
    return m_productRuntime->catalogOrchestrator();
}

// 목적: MCP Editor adapter가 사용할 frontend-neutral client 반환
// 입력: 없음
// 출력: context lifetime 동안 유효한 Editor client 참조
core::client::IEditorClient& McpApplicationContext::editorClient() noexcept
{
    return m_productRuntime->editorOrchestrator();
}

// 목적: MCP Source Resolution command adapter가 사용할 frontend-neutral client 반환
// 입력: 없음
// 출력: context lifetime 동안 유효한 Source Resolution client 참조
core::client::ISourceResolutionClient& McpApplicationContext::sourceResolutionClient() noexcept
{
    return m_productRuntime->catalogOrchestrator();
}

// 목적: MCP async delivery adapter가 구독할 frontend-neutral Source Resolution event source 반환
// 입력: 없음
// 출력: context lifetime 동안 유효한 ordered event source 참조
core::client::ISourceResolutionEventSource& McpApplicationContext::sourceResolutionEventSource() noexcept
{
    return m_productRuntime->sourceResolutionEventSource();
}

}  // namespace flexraw::app::mcp
