#pragma once

#include <memory>

#include "mainwindow.h"

class QSettings;

namespace flexraw::core::orchestration
{
class CatalogThumbnailOrchestrator;
class CatalogSessionOrchestrator;
class ExportOrchestrator;
class WorkerHealthOrchestrator;
}  // namespace flexraw::core::orchestration

namespace flexraw::runtime
{
class ProductRuntime;
}

namespace flexraw::platform
{
class IPathIdentityService;
class ISystemMemoryProbe;
}  // namespace flexraw::platform

namespace flexraw::ui::facade
{
class CatalogEditorFacade;
}

namespace flexraw::ui::export_
{
class QtExportClientAdapter;
}

namespace flexraw::ui::settings
{
class QtCatalogStartupSettingsAdapter;
class QtExportSettingsAdapter;
class QtWorkerHealthEventAdapter;
class QtWorkerProfileSettingsAdapter;
}  // namespace flexraw::ui::settings

namespace flexraw::app
{

class ManagedCatalogSession;

class ApplicationContext final
{
public:
    // 목적: production preview/export/Catalog thumbnail, editor session과 MainWindow 객체 그래프 조립
    // 입력: 없음
    // 출력: 실행 가능한 application context
    ApplicationContext();

    // 목적: UI보다 늦게 Orchestrator가 종료되도록 owned 객체를 역순 정리
    // 입력: 없음
    // 출력: 없음
    ~ApplicationContext();

    // 목적: context가 소유한 primary MainWindow 반환
    // 입력: 없음
    // 출력: application lifetime 동안 유효한 MainWindow 참조
    [[nodiscard]] ui::mainwindow::MainWindow& mainWindow();

private:
    std::unique_ptr<runtime::ProductRuntime> m_productRuntime;
    std::unique_ptr<core::orchestration::CatalogThumbnailOrchestrator> m_catalogThumbnailOrchestrator;
    std::unique_ptr<platform::IPathIdentityService> m_pathIdentityService;
    std::unique_ptr<platform::ISystemMemoryProbe> m_systemMemoryProbe;
    std::unique_ptr<core::orchestration::ExportOrchestrator> m_exportOrchestrator;
    std::unique_ptr<QSettings> m_applicationSettings;
    std::unique_ptr<ui::settings::QtCatalogStartupSettingsAdapter> m_catalogStartupSettings;
    std::unique_ptr<ManagedCatalogSession> m_managedCatalogSession;
    std::unique_ptr<core::orchestration::CatalogSessionOrchestrator> m_catalogSessionOrchestrator;
    std::unique_ptr<ui::facade::CatalogEditorFacade> m_catalogEditorFacade;
    std::unique_ptr<ui::settings::QtExportSettingsAdapter> m_exportSettings;
    std::unique_ptr<ui::settings::QtWorkerProfileSettingsAdapter> m_workerProfileSettings;
    std::unique_ptr<core::orchestration::WorkerHealthOrchestrator> m_workerHealthOrchestrator;
    std::unique_ptr<ui::settings::QtWorkerHealthEventAdapter> m_workerHealthEventAdapter;
    std::unique_ptr<ui::export_::QtExportClientAdapter> m_exportClientAdapter;
    std::unique_ptr<ui::mainwindow::MainWindow> m_mainWindow;
};

}  // namespace flexraw::app
