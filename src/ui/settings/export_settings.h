#pragma once

#include <cstdint>

#include <QSettings>
#include <QString>

#include "export_options.h"

namespace flexraw::ui::settings
{

enum class ExportExecutionMode : std::uint8_t
{
    Local,
    Remote,
    Auto,
};

struct ExportExecutionDefaults
{
    ExportExecutionMode mode{ExportExecutionMode::Local};
    QString remoteHost{QStringLiteral("127.0.0.1")};
    int remotePort{47331};
    QString expectedSourceStorageId;
    QString expectedOutputStorageId;
};

class ExportSettings final
{
public:
    // 목적: 지정한 application settings에 접근하는 export 설정 저장소 초기화
    // 입력: settings: 소유권을 유지하는 Qt settings 객체
    // 출력: 초기화된 ExportSettings 객체
    explicit ExportSettings(QSettings& settings);

    // 목적: 마지막으로 저장한 raster export 기본 option을 검증하여 복원
    // 입력: 없음
    // 출력: 유효한 저장값 또는 안전한 기본 RasterExportOptions
    [[nodiscard]] core::export_::RasterExportOptions loadRasterDefaults() const;

    // 목적: 성공한 raster export의 option을 다음 export 기본값으로 저장
    // 입력: options: 저장할 검증 완료 raster export option
    // 출력: 없음
    void saveRasterDefaults(const core::export_::RasterExportOptions& options) const;

    // 목적: 마지막 export 실행 위치와 내부 Remote endpoint/storage 결합 기본값 복원
    // 입력: 없음
    // 출력: 유효한 저장값 또는 Local/loopback 안전 기본값
    [[nodiscard]] ExportExecutionDefaults loadExecutionDefaults() const;

    // 목적: accepted export 실행 위치와 숨겨진 endpoint/storage 결합을 다음 dialog 기본값으로 저장
    // 입력: defaults: 검증 완료 실행 mode, endpoint와 자동 탐색된 expected storage UUID
    // 출력: 없음
    void saveExecutionDefaults(const ExportExecutionDefaults& defaults) const;

private:
    QSettings& m_settings;
};

}  // namespace flexraw::ui::settings
