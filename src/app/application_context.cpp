#include "application_context.h"

#include <memory>

#include <QSettings>

#include "catalog_editor_facade.h"
#include "catalog_orchestrator.h"
#include "catalog_session_orchestrator.h"
#include "catalog_thumbnail_orchestrator.h"
#include "catalog_thumbnail_pipeline.h"
#include "compute_permit_pool.h"
#include "current_path_identity_service.h"
#include "current_system_memory_probe.h"
#include "display_profile_service.h"
#include "editor_orchestrator.h"
#include "export_orchestrator.h"
#include "export_pipeline.h"
#include "managed_catalog_session.h"
#include "preview_cache.h"
#include "preview_pipeline.h"
#include "product_export_client_adapter.h"
#include "product_runtime.h"
#include "qt_catalog_startup_settings_adapter.h"
#include "qt_export_settings_adapter.h"
#include "qt_worker_health_event_adapter.h"
#include "qt_worker_profile_settings_adapter.h"
#include "remote_export_execution_adapter.h"
#include "remote_render_executor.h"
#include "worker_health_orchestrator.h"
#include "worker_health_probe_adapter.h"

#if defined(_WIN32)
#include "windows_display_profile_service.h"
#include "windows_display_target_resolver.h"
#endif

namespace flexraw::app
{
namespace
{

// 현재 Preview는 dedicated worker 하나로 직렬 실행되므로 accounting seam만 추가하고 concurrency 정책은 보존합니다.
constexpr int TransitionalComputePermitCapacity = 1;

// 목적: Desktop host에서 지원되는 current display profile service 생성
// 입력: 없음
// 출력: Windows concrete service 또는 아직 Adapter가 없는 host의 빈 pointer
[[nodiscard]] std::unique_ptr<platform::IDisplayProfileService> createDisplayProfileService()
{
#if defined(_WIN32)
    return std::make_unique<platform::WindowsDisplayProfileService>();
#else
    return {};
#endif
}

// 목적: current Desktop host의 QScreen을 Platform display target에 대응할 resolver 선택
// 입력: 없음
// 출력: Windows Desktop resolver 또는 아직 integration이 없는 host의 빈 callable
[[nodiscard]] ui::facade::DisplayTargetResolver createDisplayTargetResolver()
{
#if defined(_WIN32)
    return platform::resolveWindowsDisplayTarget;
#else
    return {};
#endif
}

// 목적: Managed Catalog startup 결과를 기술 진단 없이 GUI 표시 모델로 변환
// 입력: state: startup open, fallback과 fatal 상태
// 출력: 정상 open·fallback·실패를 구분하는 GUI startup 안내 값
[[nodiscard]] ui::mainwindow::CatalogStartupNotice makeCatalogStartupNotice(const ManagedCatalogStartupState& state)
{
    if (state.usedFallback)
    {
        return {ui::mainwindow::CatalogStartupNoticeKind::RecoveredWithManagedCatalog, state.requestedCatalogPath};
    }
    if (state.fatalError.has_value())
    {
        return {ui::mainwindow::CatalogStartupNoticeKind::StartupCatalogUnavailable, state.requestedCatalogPath};
    }
    if (state.session.isOpen)
    {
        return {ui::mainwindow::CatalogStartupNoticeKind::CatalogOpened, state.session.catalogPath};
    }
    return {};
}

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
    : m_pathIdentityService(platform::createCurrentPathIdentityService()),
      m_displayProfileService(createDisplayProfileService()),
      m_systemMemoryProbe(platform::createCurrentSystemMemoryProbe()),
      m_computePermitPool(std::make_unique<core::orchestration::ComputePermitPool>(TransitionalComputePermitCapacity)),
      m_productRuntime(std::make_unique<runtime::ProductRuntime>(
          std::make_unique<core::orchestration::FilePreviewPipeline>(core::preview::defaultPreviewCacheRoot(),
                                                                     *m_computePermitPool),
          std::make_unique<core::orchestration::ExportOrchestrator>(
              std::make_unique<core::orchestration::FileExportPipeline>(*m_pathIdentityService),
              std::make_unique<worker::client::RemoteExportExecutionAdapter>(
                  std::make_unique<worker::client::RemoteRenderExecutor>()),
              m_systemMemoryProbe.get(),
              productionExportSchedulingConfiguration()))),
      m_catalogThumbnailOrchestrator(std::make_unique<core::orchestration::CatalogThumbnailOrchestrator>(
          std::make_unique<core::orchestration::FileCatalogThumbnailPipeline>())),
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
      m_exportClientAdapter(std::make_unique<runtime::ProductExportClientAdapter>(
          m_productRuntime->exportOrchestrator(), *m_workerProfileSettings)),
      m_mainWindow(std::make_unique<ui::mainwindow::MainWindow>(*m_catalogEditorFacade,
                                                                *m_exportClientAdapter,
                                                                *m_exportClientAdapter,
                                                                *m_exportSettings,
                                                                *m_applicationSettings,
                                                                *m_workerProfileSettings,
                                                                makeCatalogStartupNotice(
                                                                    m_managedCatalogSession->startupState()),
                                                                m_workerHealthOrchestrator.get(),
                                                                m_workerHealthEventAdapter.get(),
                                                                m_catalogStartupSettings.get(),
                                                                m_displayProfileService.get(),
                                                                createDisplayTargetResolver()))
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
