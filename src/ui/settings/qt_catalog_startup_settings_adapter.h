#pragma once

#include "catalog_startup_settings_client.h"

class QSettings;

namespace flexraw::ui::settings
{

class QtCatalogStartupSettingsAdapter final : public core::client::ICatalogStartupSettingsClient
{
public:
    // 목적: application QSettings와 OS Managed Catalog 위치를 frontend-neutral startup settings 계약에 연결
    // 입력: settings: adapter보다 오래 유지되는 application settings
    // 출력: 기존 Catalog runtime state와 분리된 application-scoped settings adapter
    explicit QtCatalogStartupSettingsAdapter(QSettings& settings);

    // 목적: 저장된 startup 정책을 검증하고 현재 OS Managed Catalog 위치와 함께 조회
    // 입력: 없음
    // 출력: 유효한 startup settings snapshot 또는 저장소·경로 오류
    [[nodiscard]] core::client::CatalogStartupSettingsResult catalogStartupSettings() const override;

    // 목적: 다음 application startup에 사용할 Catalog 선택 정책 저장
    // 입력: behavior: 마지막 active Catalog 재개 또는 Managed Catalog 강제 선택
    // 출력: 저장된 정책과 현재 Managed Catalog 위치 또는 validation·저장소 오류
    [[nodiscard]] core::client::CatalogStartupSettingsResult saveCatalogStartupBehavior(
        core::client::CatalogStartupBehavior behavior) override;

private:
    QSettings& m_settings;
};

}  // namespace flexraw::ui::settings
