#pragma once

#include <QWidget>

#include "catalog_startup_settings_client.h"

class QComboBox;
class QLabel;
class QLineEdit;

namespace flexraw::ui::settings
{

class GeneralSettingsPage final : public QWidget
{
public:
    // 목적: Catalog startup product setting과 현재 application 기본 정보를 표시하는 General page 구성
    // 입력: startupSettingsClient: application startup settings 계약, parent: Qt 부모 widget
    // 출력: persisted startup 정책과 Managed Catalog 위치를 표시하는 Settings page
    explicit GeneralSettingsPage(core::client::ICatalogStartupSettingsClient& startupSettingsClient,
                                 QWidget* parent = nullptr);

    // 목적: 현재 선택한 Catalog startup 정책을 다음 application 실행용으로 저장
    // 입력: 없음
    // 출력: 저장 성공 시 true, load 또는 persistence 실패 시 false
    [[nodiscard]] bool saveSettings();

private:
    // 목적: 저장된 Catalog startup settings snapshot을 controls에 복원
    // 입력: 없음
    // 출력: 성공 시 저장 가능한 controls와 Managed Catalog 위치 갱신
    void loadSettings();

    core::client::ICatalogStartupSettingsClient* m_startupSettingsClient{nullptr};
    QComboBox* m_startupBehaviorComboBox{nullptr};
    QLineEdit* m_managedCatalogPathEdit{nullptr};
    QLabel* m_statusLabel{nullptr};
    bool m_settingsLoaded{false};
};

}  // namespace flexraw::ui::settings
