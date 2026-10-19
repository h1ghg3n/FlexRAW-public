#include "product_runtime.h"

#include <stdexcept>
#include <utility>

#include "catalog_orchestrator.h"
#include "editor_orchestrator.h"
#include "export_orchestrator.h"
#include "preview_orchestrator.h"
#include "preview_pipeline.h"
#include "qt_source_resolution_event_source.h"

namespace flexraw::runtime
{

// 목적: 주입된 Preview와 Export capability로 공유 product operation owner graph 구성
// 입력: previewPipeline: synchronous Preview implementation, exportOrchestrator: application-scoped Export authority
// 출력: Catalog, Editor, Source Resolution과 Export lifecycle을 소유한 Product Runtime
ProductRuntime::ProductRuntime(std::unique_ptr<core::orchestration::IPreviewPipeline> previewPipeline,
                               std::unique_ptr<core::orchestration::ExportOrchestrator> exportOrchestrator)
    : m_previewOrchestrator(std::make_unique<core::orchestration::PreviewOrchestrator>(std::move(previewPipeline))),
      m_catalogOrchestrator(std::make_unique<core::orchestration::CatalogOrchestrator>()),
      m_sourceResolutionEventSource(
          std::make_unique<core::orchestration::QtSourceResolutionEventSource>(*m_catalogOrchestrator)),
      m_editorOrchestrator(
          std::make_unique<core::orchestration::EditorOrchestrator>(*m_previewOrchestrator, *m_catalogOrchestrator)),
      m_exportOrchestrator(std::move(exportOrchestrator))
{
    if (m_exportOrchestrator == nullptr)
    {
        throw std::invalid_argument("Product Runtime Export authority must not be null.");
    }
}

// 목적: downstream consumer보다 늦게 Export, Editor, Source event, Catalog와 Preview owner를 역순 정리
// 입력: 없음
// 출력: active operation resource가 dependency보다 먼저 정리됨
ProductRuntime::~ProductRuntime() = default;

// 목적: Composition Root가 Catalog-backed client와 transitional Qt adapter를 같은 authority에 연결
// 입력: 없음
// 출력: Product Runtime lifetime 동안 유효한 Catalog Orchestrator 참조
core::orchestration::CatalogOrchestrator& ProductRuntime::catalogOrchestrator() noexcept
{
    return *m_catalogOrchestrator;
}

// 목적: Adapter가 공유 Catalog 선별 mutation authority를 Product Contract로 소비
// 입력: 없음
// 출력: Product Runtime lifetime 동안 유효한 Catalog culling client 참조
core::client::ICatalogCullingClient& ProductRuntime::catalogCullingClient() noexcept
{
    return *m_catalogOrchestrator;
}

// 목적: Adapter가 공유 Catalog 등록 포함 mutation authority를 Product Contract로 소비
// 입력: 없음
// 출력: Product Runtime lifetime 동안 유효한 Catalog inclusion client 참조
core::client::ICatalogInclusionClient& ProductRuntime::catalogInclusionClient() noexcept
{
    return *m_catalogOrchestrator;
}

// 목적: Composition Root가 Editor client와 transitional Qt adapter를 같은 session owner에 연결
// 입력: 없음
// 출력: Product Runtime lifetime 동안 유효한 Editor Orchestrator 참조
core::orchestration::EditorOrchestrator& ProductRuntime::editorOrchestrator() noexcept
{
    return *m_editorOrchestrator;
}

// 목적: Adapter가 공유 Catalog source lifecycle의 frontend-neutral event를 구독
// 입력: 없음
// 출력: Product Runtime lifetime 동안 유효한 Source Resolution event source 참조
core::client::ISourceResolutionEventSource& ProductRuntime::sourceResolutionEventSource() noexcept
{
    return *m_sourceResolutionEventSource;
}

// 목적: Composition Root가 Export product adapter를 공유 scheduling authority에 연결
// 입력: 없음
// 출력: Product Runtime lifetime 동안 유효한 Export Orchestrator 참조
core::orchestration::ExportOrchestrator& ProductRuntime::exportOrchestrator() noexcept
{
    return *m_exportOrchestrator;
}

}  // namespace flexraw::runtime
