#pragma once

#include <memory>

namespace flexraw::core::client
{
class ISourceResolutionEventSource;
}

namespace flexraw::core::orchestration
{
class CatalogOrchestrator;
class EditorOrchestrator;
class IPreviewPipeline;
class PreviewOrchestrator;
class QtSourceResolutionEventSource;
}  // namespace flexraw::core::orchestration

namespace flexraw::runtime
{

class ProductRuntime final
{
public:
    // 목적: 주입된 Preview capability로 공유 Catalog/Editor operation owner graph 구성
    // 입력: previewPipeline: consumer composition이 선택한 synchronous Preview implementation
    // 출력: Catalog, Editor와 Source Resolution lifecycle을 소유한 Product Runtime
    explicit ProductRuntime(std::unique_ptr<core::orchestration::IPreviewPipeline> previewPipeline);

    // 목적: downstream consumer보다 늦게 Editor, Source event, Catalog와 Preview owner를 역순 정리
    // 입력: 없음
    // 출력: active operation resource가 dependency보다 먼저 정리됨
    ~ProductRuntime();

    ProductRuntime(const ProductRuntime&) = delete;
    ProductRuntime& operator=(const ProductRuntime&) = delete;

    // 목적: Composition Root가 Catalog-backed client와 transitional Qt adapter를 같은 authority에 연결
    // 입력: 없음
    // 출력: Product Runtime lifetime 동안 유효한 Catalog Orchestrator 참조
    [[nodiscard]] core::orchestration::CatalogOrchestrator& catalogOrchestrator() noexcept;

    // 목적: Composition Root가 Editor client와 transitional Qt adapter를 같은 session owner에 연결
    // 입력: 없음
    // 출력: Product Runtime lifetime 동안 유효한 Editor Orchestrator 참조
    [[nodiscard]] core::orchestration::EditorOrchestrator& editorOrchestrator() noexcept;

    // 목적: Adapter가 공유 Catalog source lifecycle의 frontend-neutral event를 구독
    // 입력: 없음
    // 출력: Product Runtime lifetime 동안 유효한 Source Resolution event source 참조
    [[nodiscard]] core::client::ISourceResolutionEventSource& sourceResolutionEventSource() noexcept;

private:
    std::unique_ptr<core::orchestration::PreviewOrchestrator> m_previewOrchestrator;
    std::unique_ptr<core::orchestration::CatalogOrchestrator> m_catalogOrchestrator;
    std::unique_ptr<core::orchestration::QtSourceResolutionEventSource> m_sourceResolutionEventSource;
    std::unique_ptr<core::orchestration::EditorOrchestrator> m_editorOrchestrator;
};

}  // namespace flexraw::runtime
