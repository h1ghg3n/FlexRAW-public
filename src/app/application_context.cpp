#include "application_context.h"

#include <memory>

#include "catalog_editor_facade.h"
#include "catalog_orchestrator.h"
#include "catalog_thumbnail_orchestrator.h"
#include "catalog_thumbnail_pipeline.h"
#include "current_system_memory_probe.h"
#include "editor_orchestrator.h"
#include "export_orchestrator.h"
#include "export_pipeline.h"
#include "managed_catalog_session.h"
#include "preview_cache.h"
#include "preview_orchestrator.h"
#include "preview_pipeline.h"
#include "remote_export_execution_adapter.h"
#include "remote_render_executor.h"

namespace flexraw::app
{
namespace
{

// 목적: Desktop Local/Remote/Auto export의 측정 전 production scheduling 상한 구성
// 입력: 없음
// 출력: Local c4, Remote c2와 CPU/RAM 안전 여유를 적용한 설정
[[nodiscard]] core::orchestration::ExportSchedulingConfiguration productionExportSchedulingConfiguration()
{
    core::orchestration::ExportSchedulingConfiguration configuration;
    configuration.localSlotLimit = 4;
    configuration.remoteSlotLimit = 2;
    configuration.maximumRemoteDispatchAttempts = 3;
    configuration.enforceLocalResourceReserve = true;
    configuration.reservedLogicalProcessors = 4;
    configuration.memoryReserveBytes = 10ULL * 1024ULL * 1024ULL * 1024ULL;
    configuration.memoryClaimPerJobBytes = 1024ULL * 1024ULL * 1024ULL;
    return configuration;
}

}  // namespace

// 목적: production preview/export/Catalog thumbnail, editor session과 MainWindow 객체 그래프 조립
// 입력: 없음
// 출력: 실행 가능한 application context
ApplicationContext::ApplicationContext()
    : m_previewOrchestrator(std::make_unique<core::orchestration::PreviewOrchestrator>(
          std::make_unique<core::orchestration::FilePreviewPipeline>(core::preview::defaultPreviewCacheRoot()))),
      m_catalogThumbnailOrchestrator(std::make_unique<core::orchestration::CatalogThumbnailOrchestrator>(
          std::make_unique<core::orchestration::FileCatalogThumbnailPipeline>())),
      m_systemMemoryProbe(platform::createCurrentSystemMemoryProbe()),
      m_exportOrchestrator(std::make_unique<core::orchestration::ExportOrchestrator>(
          std::make_unique<core::orchestration::FileExportPipeline>(),
          std::make_unique<worker::client::RemoteExportExecutionAdapter>(
              std::make_unique<worker::client::RemoteRenderExecutor>()),
          m_systemMemoryProbe.get(),
          productionExportSchedulingConfiguration())),
      m_managedCatalogSession(std::make_unique<ManagedCatalogSession>()),
      m_editorOrchestrator(std::make_unique<core::orchestration::EditorOrchestrator>(
          *m_previewOrchestrator, m_managedCatalogSession->orchestrator())),
      m_catalogEditorFacade(std::make_unique<ui::facade::CatalogEditorFacade>(
          m_managedCatalogSession->orchestrator(), *m_catalogThumbnailOrchestrator, *m_editorOrchestrator)),
      m_mainWindow(std::make_unique<ui::mainwindow::MainWindow>(
          *m_catalogEditorFacade, m_managedCatalogSession->orchestrator(), *m_exportOrchestrator))
{}

// 목적: UI보다 늦게 Orchestrator가 종료되도록 owned 객체를 역순 정리
// 입력: 없음
// 출력: 없음
ApplicationContext::~ApplicationContext() = default;

// 목적: context가 소유한 primary MainWindow 반환
// 입력: 없음
// 출력: application lifetime 동안 유효한 MainWindow 참조
ui::mainwindow::MainWindow& ApplicationContext::mainWindow()
{
    return *m_mainWindow;
}

}  // namespace flexraw::app
