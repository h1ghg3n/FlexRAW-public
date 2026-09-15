#pragma once

#include <QDialog>

#include "adjustment_control_style.h"

class QComboBox;
class QSettings;

namespace flexraw::core::client
{
class ICatalogStartupSettingsClient;
class IExportDefaultsClient;
class IWorkerHealthClient;
class IWorkerHealthEventSource;
class IWorkerProfileClient;
}  // namespace flexraw::core::client

namespace flexraw::ui::settings
{

class ExportDefaultsPage;
class GeneralSettingsPage;

class SettingsDialog final : public QDialog
{
    Q_OBJECT

public:
    // 목적: 현재 UI preference를 편집하는 최소 Settings window 초기화
    // 입력: settings: application preference 저장소, workerProfileClient: Worker CRUD,
    //       exportDefaultsClient: application Export 기본값 계약, workerHealthClient/eventSource: optional health,
    //       catalogStartupSettingsClient: optional General settings capability, parent: Qt 부모 widget
    // 출력: available General capability, View, Workers와 Export tab을 포함한 dialog
    explicit SettingsDialog(QSettings& settings,
                            core::client::IWorkerProfileClient& workerProfileClient,
                            core::client::IExportDefaultsClient& exportDefaultsClient,
                            core::client::IWorkerHealthClient* workerHealthClient = nullptr,
                            core::client::IWorkerHealthEventSource* workerHealthEventSource = nullptr,
                            core::client::ICatalogStartupSettingsClient* catalogStartupSettingsClient = nullptr,
                            QWidget* parent = nullptr);

    // 목적: dialog에서 현재 선택된 adjustment presentation 반환
    // 입력: 없음
    // 출력: Classic 또는 Relative style
    [[nodiscard]] editor::AdjustmentControlStyle adjustmentControlStyle() const;

private:
    QSettings& m_settings;
    QComboBox* m_adjustmentControlStyleComboBox{nullptr};
    GeneralSettingsPage* m_generalSettingsPage{nullptr};
    ExportDefaultsPage* m_exportDefaultsPage{nullptr};
};

}  // namespace flexraw::ui::settings
