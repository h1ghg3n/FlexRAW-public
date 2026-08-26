#pragma once

#include <memory>

#include "mainwindow.h"

namespace flexraw::core::orchestration
{
class CatalogThumbnailOrchestrator;
class EditorOrchestrator;
class ExportOrchestrator;
class PreviewOrchestrator;
}  // namespace flexraw::core::orchestration

namespace flexraw::platform
{
class ISystemMemoryProbe;
}

namespace flexraw::ui::facade
{
class CatalogEditorFacade;
}

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
    std::unique_ptr<core::orchestration::PreviewOrchestrator> m_previewOrchestrator;
    std::unique_ptr<core::orchestration::CatalogThumbnailOrchestrator> m_catalogThumbnailOrchestrator;
    std::unique_ptr<platform::ISystemMemoryProbe> m_systemMemoryProbe;
    std::unique_ptr<core::orchestration::ExportOrchestrator> m_exportOrchestrator;
    std::unique_ptr<ManagedCatalogSession> m_managedCatalogSession;
    std::unique_ptr<core::orchestration::EditorOrchestrator> m_editorOrchestrator;
    std::unique_ptr<ui::facade::CatalogEditorFacade> m_catalogEditorFacade;
    std::unique_ptr<ui::mainwindow::MainWindow> m_mainWindow;
};

}  // namespace flexraw::app
