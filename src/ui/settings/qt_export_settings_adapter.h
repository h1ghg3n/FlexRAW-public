#pragma once

#include "export_defaults_client.h"

class QSettings;

namespace flexraw::ui::settings
{

class QtExportSettingsAdapter final : public core::client::IExportDefaultsClient
{
public:
    // 목적: application QSettings를 frontend-neutral Export default contract에 연결
    // 입력: settings: adapter보다 오래 유지되는 application settings
    // 출력: 기존 export key를 사용하는 application-scoped adapter
    explicit QtExportSettingsAdapter(QSettings& settings);

    // 목적: 저장된 raster와 placement 값을 검증해 application-wide 기본값 snapshot 복원
    // 입력: 없음
    // 출력: 유효한 저장값과 field별 안전 fallback 또는 저장소 오류
    [[nodiscard]] core::client::ExportDefaultsResult exportDefaults() const override;

    // 목적: 성공한 Export의 Qt-free raster option을 기존 application key에 저장
    // 입력: options: 저장할 format, encoding, dimension, color와 metadata option
    // 출력: 저장된 option 또는 validation·저장소 오류
    [[nodiscard]] core::client::ExportRasterDefaultsResult saveRasterExportDefaults(
        const core::client::ExportRasterOptions& options) override;

    // 목적: accepted Export의 placement와 preferred Worker identity를 기존 application key에 저장
    // 입력: defaults: Local/Remote/Auto policy와 optional stable Worker identity
    // 출력: 저장된 normalized 기본값 또는 validation·저장소 오류
    [[nodiscard]] core::client::ExportExecutionDefaultsResult saveExportExecutionDefaults(
        const core::client::ExportExecutionDefaults& defaults) override;

private:
    QSettings& m_settings;
};

}  // namespace flexraw::ui::settings
