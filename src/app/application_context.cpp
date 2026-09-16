#include "application_context.h"

#include <memory>

#include <QSettings>

#include "catalog_editor_facade.h"
#include "catalog_orchestrator.h"
#include "catalog_session_orchestrator.h"
#include "catalog_thumbnail_orchestrator.h"
#include "catalog_thumbnail_pipeline.h"
#include "current_path_identity_service.h"
#include "current_system_memory_probe.h"
#include "editor_orchestrator.h"
#include "export_orchestrator.h"
#include "export_pipeline.h"
#include "managed_catalog_session.h"
#include "preview_cache.h"
#include "preview_pipeline.h"
#include "product_runtime.h"
#include "qt_catalog_startup_settings_adapter.h"
#include "qt_export_client_adapter.h"
#include "qt_export_settings_adapter.h"
#include "qt_worker_health_event_adapter.h"
#include "qt_worker_profile_settings_adapter.h"
#include "remote_export_execution_adapter.h"
#include "remote_render_executor.h"
#include "worker_health_orchestrator.h"
#include "worker_health_probe_adapter.h"

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
    : m_productRuntime(std::make_unique<runtime::ProductRuntime>(
          std::make_unique<core::orchestration::FilePreviewPipeline>(core::preview::defaultPreviewCacheRoot()))),
      m_catalogThumbnailOrchestrator(std::make_unique<core::orchestration::CatalogThumbnailOrchestrator>(
          std::make_unique<core::orchestration::FileCatalogThumbnailPipeline>())),
      m_pathIdentityService(platform::createCurrentPathIdentityService()),
      m_systemMemoryProbe(platform::createCurrentSystemMemoryProbe()),
      m_exportOrchestrator(std::make_unique<core::orchestration::ExportOrchestrator>(
          std::make_unique<core::orchestration::FileExportPipeline>(*m_pathIdentityService),
          std::make_unique<worker::client::RemoteExportExecutionAdapter>(
              std::make_unique<worker::client::RemoteRenderExecutor>()),
          m_systemMemoryProbe.get(),
          productionExportSchedulingConfiguration())),
      m_applicationSettings(std::make_unique<QSettings>()),
      m_catalogStartupSettings(std::make_unique<ui::settings::QtCatalogStartupSettingsAdapter>(*m_applicationSettings)),
      m_managedCatalogSession(
          std::make_unique<ManagedCatalogSession>(m_productRuntime->catalogOrchestrator(), *m_catalogStartupSettings)),
      m_catalogSessionOrchestrator(std::make_unique<core::orchestration::CatalogSessionOrchestrator>(
          m_productRuntime->catalogOrchestrator(), m_productRuntime->editorOrchestrator())),
      m_catalogEditorFacade(
          std::make_unique<ui::facade::CatalogEditorFacade>(m_productRuntime->catalogOrchestrator(),
                                                            *m_catalogSessionOrchestrator,
                                                            *m_catalogThumbnailOrchestrator,
                                                            m_productRuntime->editorOrchestrator(),
                                                            m_productRuntime->sourceResolutionEventSource())),
      m_exportSettings(std::make_unique<ui::settings::QtExportSettingsAdapter>(*m_applicationSettings)),
      m_workerProfileSettings(std::make_unique<ui::settings::QtWorkerProfileSettingsAdapter>(*m_applicationSettings)),
      m_workerHealthOrchestrator(std::make_unique<core::orchestration::WorkerHealthOrchestrator>(
          *m_workerProfileSettings, std::make_unique<worker::client::WorkerHealthProbeAdapter>())),
      m_workerHealthEventAdapter(
          std::make_unique<ui::settings::QtWorkerHealthEventAdapter>(*m_workerHealthOrchestrator)),
      m_exportClientAdapter(
          std::make_unique<ui::export_::QtExportClientAdapter>(*m_exportOrchestrator, *m_workerProfileSettings)),
      m_mainWindow(std::make_unique<ui::mainwindow::MainWindow>(*m_catalogEditorFacade,
                                                                *m_exportClientAdapter,
                                                                *m_exportClientAdapter,
                                                                *m_exportSettings,
                                                                *m_applicationSettings,
                                                                *m_workerProfileSettings,
                                                                m_workerHealthOrchestrator.get(),
                                                                m_workerHealthEventAdapter.get(),
                                                                m_catalogStartupSettings.get()))
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
