#pragma once

#include <optional>

#include <QWidget>

#include "export_defaults_client.h"
#include "worker_profile_client.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QSpinBox;

namespace flexraw::ui::settings
{

class ExportDefaultsPage final : public QWidget
{
public:
    // 목적: frontend-neutral Export 기본값을 편집하는 Settings page 구성
    // 입력: exportDefaultsClient: application 기본값 계약, workerProfileClient: preferred Worker 조회 계약, parent: Qt
    // 부모 출력: persisted snapshot을 표시하는 Export 기본값 page
    explicit ExportDefaultsPage(core::client::IExportDefaultsClient& exportDefaultsClient,
                                core::client::IWorkerProfileClient& workerProfileClient,
                                QWidget* parent = nullptr);

    // 목적: 현재 widget 값을 application-wide Export 기본값으로 저장
    // 입력: 없음
    // 출력: raster와 execution 기본값이 모두 저장되면 true
    [[nodiscard]] bool saveDefaults();

    // 목적: application Worker profile 변경을 preferred Worker selector에 다시 반영
    // 입력: 없음
    // 출력: 현재 선택 identity를 보존한 profile 목록과 상태 안내 갱신
    void refreshWorkerProfiles();

private:
    // 목적: 저장된 Export 기본값 snapshot을 widget에 복원
    // 입력: 없음
    // 출력: 성공 시 저장 가능한 controls와 preferred identity 초기화
    void loadDefaults();

    // 목적: 현재 widget 값을 Qt-free raster 기본값으로 조립
    // 입력: 없음
    // 출력: format, encoding, dimension, color와 metadata option
    [[nodiscard]] core::client::ExportRasterOptions collectRasterOptions() const;

    // 목적: 현재 widget 값을 Qt-free placement 기본값으로 조립
    // 입력: 없음
    // 출력: Local/Remote/Auto와 optional preferred Worker identity
    [[nodiscard]] core::client::ExportExecutionDefaults collectExecutionDefaults() const;

    core::client::IExportDefaultsClient* m_exportDefaultsClient{nullptr};
    core::client::IWorkerProfileClient* m_workerProfileClient{nullptr};
    QComboBox* m_formatComboBox{nullptr};
    QSpinBox* m_jpegQualitySpinBox{nullptr};
    QSpinBox* m_pngCompressionSpinBox{nullptr};
    QComboBox* m_tiffCompressionComboBox{nullptr};
    QSpinBox* m_maximumDimensionSpinBox{nullptr};
    QComboBox* m_outputColorSpaceComboBox{nullptr};
    QCheckBox* m_includeMetadataCheckBox{nullptr};
    QComboBox* m_placementComboBox{nullptr};
    QComboBox* m_workerProfileComboBox{nullptr};
    QLabel* m_statusLabel{nullptr};
    bool m_defaultsLoaded{false};
    bool m_workerProfilesInitialized{false};
    std::optional<core::client::WorkerProfileId> m_preferredWorkerProfileId;
};

}  // namespace flexraw::ui::settings
