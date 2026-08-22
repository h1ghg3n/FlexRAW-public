#include "export_settings.h"

#include <limits>

#include <QUuid>

namespace flexraw::ui::settings
{
namespace
{

// 목적: 저장된 정수 enum 값이 범위 안에 있는지 확인
// 입력: value: settings에서 읽은 값, minimum/maximum: 허용 enum 범위
// 출력: 범위 안이면 value, 아니면 fallback
[[nodiscard]] int boundedEnumValue(const QVariant& value, const int minimum, const int maximum, const int fallback)
{
    bool converted = false;
    const int parsed = value.toInt(&converted);
    return converted && parsed >= minimum && parsed <= maximum ? parsed : fallback;
}

// 목적: 저장된 정수 설정값을 허용 범위로 제한
// 입력: value: settings에서 읽은 값, minimum/maximum: 허용 범위
// 출력: 범위 안이면 value, 아니면 fallback
[[nodiscard]] int boundedIntegerValue(const QVariant& value, const int minimum, const int maximum, const int fallback)
{
    bool converted = false;
    const int parsed = value.toInt(&converted);
    return converted && parsed >= minimum && parsed <= maximum ? parsed : fallback;
}

// 목적: 저장된 UUID text를 brace 없는 canonical form으로 정규화
// 입력: value: settings에서 읽은 storage UUID 후보
// 출력: valid non-null UUID 문자열 또는 빈 문자열
[[nodiscard]] QString normalizedUuidText(const QVariant& value)
{
    const QUuid uuid = QUuid::fromString(value.toString().trimmed());
    return uuid.isNull() ? QString{} : uuid.toString(QUuid::WithoutBraces);
}

}  // namespace

// 목적: 지정한 application settings에 접근하는 export 설정 저장소 초기화
// 입력: settings: 소유권을 유지하는 Qt settings 객체
// 출력: 초기화된 ExportSettings 객체
ExportSettings::ExportSettings(QSettings& settings) : m_settings(settings) {}

// 목적: 마지막으로 저장한 raster export 기본 option을 검증하여 복원
// 입력: 없음
// 출력: 유효한 저장값 또는 안전한 기본 RasterExportOptions
core::export_::RasterExportOptions ExportSettings::loadRasterDefaults() const
{
    core::export_::RasterExportOptions options;
    m_settings.beginGroup(QStringLiteral("export"));
    options.format = static_cast<core::export_::RasterExportFormat>(
        boundedEnumValue(m_settings.value(QStringLiteral("format")),
                         static_cast<int>(core::export_::RasterExportFormat::Jpeg),
                         static_cast<int>(core::export_::RasterExportFormat::Tiff),
                         static_cast<int>(options.format)));
    options.jpegQuality =
        boundedIntegerValue(m_settings.value(QStringLiteral("jpegQuality")), 1, 100, options.jpegQuality);
    options.pngCompression =
        boundedIntegerValue(m_settings.value(QStringLiteral("pngCompression")), 0, 9, options.pngCompression);
    options.tiffCompression = static_cast<core::export_::TiffCompression>(
        boundedEnumValue(m_settings.value(QStringLiteral("tiffCompression")),
                         static_cast<int>(core::export_::TiffCompression::None),
                         static_cast<int>(core::export_::TiffCompression::Lzw),
                         static_cast<int>(options.tiffCompression)));
    options.maximumDimension = boundedIntegerValue(m_settings.value(QStringLiteral("maximumDimension")),
                                                   0,
                                                   std::numeric_limits<int>::max(),
                                                   options.maximumDimension);
    options.outputColorSpace = static_cast<core::export_::RasterOutputColorSpace>(
        boundedEnumValue(m_settings.value(QStringLiteral("outputColorSpace")),
                         static_cast<int>(core::export_::RasterOutputColorSpace::Srgb),
                         static_cast<int>(core::export_::RasterOutputColorSpace::DisplayP3),
                         static_cast<int>(options.outputColorSpace)));
    options.includeMetadata = m_settings.value(QStringLiteral("includeMetadata"), options.includeMetadata).toBool();
    m_settings.endGroup();
    return options;
}

// 목적: 성공한 raster export의 option을 다음 export 기본값으로 저장
// 입력: options: 저장할 검증 완료 raster export option
// 출력: 없음
void ExportSettings::saveRasterDefaults(const core::export_::RasterExportOptions& options) const
{
    m_settings.beginGroup(QStringLiteral("export"));
    m_settings.setValue(QStringLiteral("format"), static_cast<int>(options.format));
    m_settings.setValue(QStringLiteral("jpegQuality"), options.jpegQuality);
    m_settings.setValue(QStringLiteral("pngCompression"), options.pngCompression);
    m_settings.setValue(QStringLiteral("tiffCompression"), static_cast<int>(options.tiffCompression));
    m_settings.setValue(QStringLiteral("maximumDimension"), options.maximumDimension);
    m_settings.setValue(QStringLiteral("outputColorSpace"), static_cast<int>(options.outputColorSpace));
    m_settings.setValue(QStringLiteral("includeMetadata"), options.includeMetadata);
    m_settings.endGroup();
    m_settings.sync();
}

// 목적: 마지막 export 실행 위치와 내부 Remote endpoint/storage 결합 기본값 복원
// 입력: 없음
// 출력: 유효한 저장값 또는 Local/loopback 안전 기본값
ExportExecutionDefaults ExportSettings::loadExecutionDefaults() const
{
    ExportExecutionDefaults defaults;
    m_settings.beginGroup(QStringLiteral("export"));
    defaults.mode = static_cast<ExportExecutionMode>(boundedEnumValue(m_settings.value(QStringLiteral("executionMode")),
                                                                      static_cast<int>(ExportExecutionMode::Local),
                                                                      static_cast<int>(ExportExecutionMode::Auto),
                                                                      static_cast<int>(defaults.mode)));
    defaults.remoteHost = m_settings.value(QStringLiteral("remoteHost"), defaults.remoteHost).toString().trimmed();
    if (defaults.remoteHost.isEmpty())
    {
        defaults.remoteHost = QStringLiteral("127.0.0.1");
    }
    defaults.remotePort =
        boundedIntegerValue(m_settings.value(QStringLiteral("remotePort")), 1, 65535, defaults.remotePort);
    defaults.expectedSourceStorageId = normalizedUuidText(m_settings.value(QStringLiteral("expectedSourceStorageId")));
    defaults.expectedOutputStorageId = normalizedUuidText(m_settings.value(QStringLiteral("expectedOutputStorageId")));
    const bool hadLegacyManualRoots = m_settings.contains(QStringLiteral("localSourceRoot")) ||
                                      m_settings.contains(QStringLiteral("localOutputRoot"));
    m_settings.remove(QStringLiteral("localSourceRoot"));
    m_settings.remove(QStringLiteral("localOutputRoot"));
    m_settings.endGroup();
    if (hadLegacyManualRoots)
    {
        m_settings.sync();
    }
    return defaults;
}

// 목적: accepted export 실행 위치와 숨겨진 endpoint/storage 결합을 다음 dialog 기본값으로 저장
// 입력: defaults: 검증 완료 실행 mode, endpoint와 자동 탐색된 expected storage UUID
// 출력: 없음
void ExportSettings::saveExecutionDefaults(const ExportExecutionDefaults& defaults) const
{
    m_settings.beginGroup(QStringLiteral("export"));
    m_settings.setValue(QStringLiteral("executionMode"), static_cast<int>(defaults.mode));
    m_settings.setValue(QStringLiteral("remoteHost"), defaults.remoteHost.trimmed());
    m_settings.setValue(QStringLiteral("remotePort"), defaults.remotePort);
    m_settings.setValue(QStringLiteral("expectedSourceStorageId"),
                        normalizedUuidText(defaults.expectedSourceStorageId));
    m_settings.setValue(QStringLiteral("expectedOutputStorageId"),
                        normalizedUuidText(defaults.expectedOutputStorageId));
    m_settings.endGroup();
    m_settings.sync();
}

}  // namespace flexraw::ui::settings
