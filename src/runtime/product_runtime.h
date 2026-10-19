#pragma once

#include <memory>

namespace flexraw::core::client
{
class ICatalogCullingClient;
class ICatalogInclusionClient;
class ISourceResolutionEventSource;
}  // namespace flexraw::core::client

namespace flexraw::core::orchestration
{
class CatalogOrchestrator;
class EditorOrchestrator;
class ExportOrchestrator;
class IPreviewPipeline;
class PreviewOrchestrator;
class QtSourceResolutionEventSource;
}  // namespace flexraw::core::orchestration

namespace flexraw::runtime
{

class ProductRuntime final
{
public:
    // 목적: 주입된 Preview와 Export capability로 공유 product operation owner graph 구성
    // 입력: previewPipeline: 동기 Preview 구현, exportOrchestrator: application-scoped Export authority
    // 출력: Catalog, Editor, Source Resolution과 Export lifecycle을 소유한 Product Runtime
    ProductRuntime(std::unique_ptr<core::orchestration::IPreviewPipeline> previewPipeline,
                   std::unique_ptr<core::orchestration::ExportOrchestrator> exportOrchestrator);

    // 목적: downstream consumer보다 늦게 Export, Editor, Source event, Catalog와 Preview owner를 역순 정리
    // 입력: 없음
    // 출력: active operation resource가 dependency보다 먼저 정리됨
    ~ProductRuntime();

    ProductRuntime(const ProductRuntime&) = delete;
    ProductRuntime& operator=(const ProductRuntime&) = delete;

    // 목적: Composition Root가 Catalog-backed client와 transitional Qt adapter를 같은 authority에 연결
    // 입력: 없음
    // 출력: Product Runtime lifetime 동안 유효한 Catalog Orchestrator 참조
    [[nodiscard]] core::orchestration::CatalogOrchestrator& catalogOrchestrator() noexcept;

    // 목적: Adapter가 공유 Catalog 선별 mutation authority를 Product Contract로 소비
    // 입력: 없음
    // 출력: Product Runtime lifetime 동안 유효한 Catalog culling client 참조
    [[nodiscard]] core::client::ICatalogCullingClient& catalogCullingClient() noexcept;

    // 목적: Adapter가 공유 Catalog 등록 포함 mutation authority를 Product Contract로 소비
    // 입력: 없음
    // 출력: Product Runtime lifetime 동안 유효한 Catalog inclusion client 참조
    [[nodiscard]] core::client::ICatalogInclusionClient& catalogInclusionClient() noexcept;

    // 목적: Composition Root가 Editor client와 transitional Qt adapter를 같은 session owner에 연결
    // 입력: 없음
    // 출력: Product Runtime lifetime 동안 유효한 Editor Orchestrator 참조
    [[nodiscard]] core::orchestration::EditorOrchestrator& editorOrchestrator() noexcept;

    // 목적: Adapter가 공유 Catalog source lifecycle의 frontend-neutral event를 구독
    // 입력: 없음
    // 출력: Product Runtime lifetime 동안 유효한 Source Resolution event source 참조
    [[nodiscard]] core::client::ISourceResolutionEventSource& sourceResolutionEventSource() noexcept;

    // 목적: Composition Root가 Export product adapter를 공유 scheduling authority에 연결
    // 입력: 없음
    // 출력: Product Runtime lifetime 동안 유효한 Export Orchestrator 참조
    [[nodiscard]] core::orchestration::ExportOrchestrator& exportOrchestrator() noexcept;

private:
    std::unique_ptr<core::orchestration::PreviewOrchestrator> m_previewOrchestrator;
    std::unique_ptr<core::orchestration::CatalogOrchestrator> m_catalogOrchestrator;
    std::unique_ptr<core::orchestration::QtSourceResolutionEventSource> m_sourceResolutionEventSource;
    std::unique_ptr<core::orchestration::EditorOrchestrator> m_editorOrchestrator;
    std::unique_ptr<core::orchestration::ExportOrchestrator> m_exportOrchestrator;
};

}  // namespace flexraw::runtime
