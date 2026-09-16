#include "qt_export_settings_adapter.h"

#include <limits>
#include <optional>
#include <string>
#include <utility>

#include <QSettings>
#include <QString>
#include <QUuid>
#include <QVariant>

namespace flexraw::ui::settings
{
namespace
{

// 목적: Export default adapter의 diagnostic-only common error 생성
// 입력: code: machine-readable category, message: log/test detail
// 출력: frontend-neutral ClientError
[[nodiscard]] core::client::ClientError makeError(core::client::ClientErrorCode code, std::string message)
{
    return {code, std::move(message)};
}

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

// 목적: optional Worker identity를 canonical UUID contract로 정규화
// 입력: id: 생략 가능 stable Worker identity
// 출력: 생략 상태, canonical identity 또는 invalid 상태
[[nodiscard]] core::client::ClientResult<std::optional<core::client::WorkerProfileId>, core::client::ClientError>
normalizedWorkerProfileId(const std::optional<core::client::WorkerProfileId>& id)
{
    if (!id.has_value())
    {
        return core::client::ClientResult<std::optional<core::client::WorkerProfileId>,
                                          core::client::ClientError>::success(std::nullopt);
    }
    const QUuid parsed = QUuid::fromString(QString::fromStdString(id->value));
    if (parsed.isNull())
    {
        return core::client::ClientResult<std::optional<core::client::WorkerProfileId>, core::client::ClientError>::
            failure(makeError(core::client::ClientErrorCode::InvalidArgument,
                              "Preferred Worker profile identity is invalid."));
    }
    return core::client::ClientResult<std::optional<core::client::WorkerProfileId>, core::client::ClientError>::success(
        core::client::WorkerProfileId{parsed.toString(QUuid::WithoutBraces).toStdString()});
}

// 목적: public raster default command의 모든 persisted field 범위 검증
// 입력: options: 저장할 Qt-free Export option
// 출력: 유효하면 nullopt, 아니면 structured validation error
[[nodiscard]] std::optional<core::client::ClientError> validateRasterOptions(
    const core::client::ExportRasterOptions& options)
{
    if (options.jpegQuality < 1 || options.jpegQuality > 100 || options.pngCompression < 0 ||
        options.pngCompression > 9 || options.maximumDimension < 0)
    {
        return makeError(core::client::ClientErrorCode::InvalidArgument,
                         "Raster Export defaults contain an out-of-range numeric option.");
    }
    switch (options.format)
    {
    case core::client::ExportRasterFormat::Jpeg:
    case core::client::ExportRasterFormat::Png:
    case core::client::ExportRasterFormat::Tiff:
        break;
    default:
        return makeError(core::client::ClientErrorCode::UnsupportedFormat,
                         "Raster Export default format is unsupported.");
    }
    switch (options.tiffCompression)
    {
    case core::client::ExportTiffCompression::None:
    case core::client::ExportTiffCompression::Lzw:
        break;
    default:
        return makeError(core::client::ClientErrorCode::InvalidArgument,
                         "Raster Export default TIFF compression is invalid.");
    }
    switch (options.outputColorSpace)
    {
    case core::client::ExportOutputColorSpace::Srgb:
    case core::client::ExportOutputColorSpace::AdobeRgb:
    case core::client::ExportOutputColorSpace::DisplayP3:
        return std::nullopt;
    default:
        return makeError(core::client::ClientErrorCode::UnsupportedFormat,
                         "Raster Export default color space is unsupported.");
    }
}

// 목적: public placement default command의 policy enum 검증
// 입력: policy: 저장할 Local/Remote/Auto policy
// 출력: 유효하면 true
[[nodiscard]] bool isSupportedPlacementPolicy(core::client::ExportPlacementPolicy policy) noexcept
{
    switch (policy)
    {
    case core::client::ExportPlacementPolicy::LocalOnly:
    case core::client::ExportPlacementPolicy::RemoteOnly:
    case core::client::ExportPlacementPolicy::Auto:
        return true;
    }
    return false;
}

}  // namespace

// 목적: application QSettings를 frontend-neutral Export default contract에 연결
// 입력: settings: adapter보다 오래 유지되는 application settings
// 출력: 기존 export key를 사용하는 application-scoped adapter
QtExportSettingsAdapter::QtExportSettingsAdapter(QSettings& settings) : m_settings(settings) {}

// 목적: 저장된 raster와 placement 값을 검증해 application-wide 기본값 snapshot 복원
// 입력: 없음
// 출력: 유효한 저장값과 field별 안전 fallback 또는 저장소 오류
core::client::ExportDefaultsResult QtExportSettingsAdapter::exportDefaults() const
{
    core::client::ExportDefaultsSnapshot defaults;
    m_settings.beginGroup(QStringLiteral("export"));
    defaults.rasterOptions.format = static_cast<core::client::ExportRasterFormat>(
        boundedEnumValue(m_settings.value(QStringLiteral("format")),
                         static_cast<int>(core::client::ExportRasterFormat::Jpeg),
                         static_cast<int>(core::client::ExportRasterFormat::Tiff),
                         static_cast<int>(defaults.rasterOptions.format)));
    defaults.rasterOptions.jpegQuality = boundedIntegerValue(
        m_settings.value(QStringLiteral("jpegQuality")), 1, 100, defaults.rasterOptions.jpegQuality);
    defaults.rasterOptions.pngCompression = boundedIntegerValue(
        m_settings.value(QStringLiteral("pngCompression")), 0, 9, defaults.rasterOptions.pngCompression);
    defaults.rasterOptions.tiffCompression = static_cast<core::client::ExportTiffCompression>(
        boundedEnumValue(m_settings.value(QStringLiteral("tiffCompression")),
                         static_cast<int>(core::client::ExportTiffCompression::None),
                         static_cast<int>(core::client::ExportTiffCompression::Lzw),
                         static_cast<int>(defaults.rasterOptions.tiffCompression)));
    defaults.rasterOptions.maximumDimension = boundedIntegerValue(m_settings.value(QStringLiteral("maximumDimension")),
                                                                  0,
                                                                  std::numeric_limits<std::int32_t>::max(),
                                                                  defaults.rasterOptions.maximumDimension);
    defaults.rasterOptions.outputColorSpace = static_cast<core::client::ExportOutputColorSpace>(
        boundedEnumValue(m_settings.value(QStringLiteral("outputColorSpace")),
                         static_cast<int>(core::client::ExportOutputColorSpace::Srgb),
                         static_cast<int>(core::client::ExportOutputColorSpace::DisplayP3),
                         static_cast<int>(defaults.rasterOptions.outputColorSpace)));
    defaults.rasterOptions.includeMetadata =
        m_settings.value(QStringLiteral("includeMetadata"), defaults.rasterOptions.includeMetadata).toBool();
    defaults.execution.placementPolicy = static_cast<core::client::ExportPlacementPolicy>(
        boundedEnumValue(m_settings.value(QStringLiteral("executionMode")),
                         static_cast<int>(core::client::ExportPlacementPolicy::LocalOnly),
                         static_cast<int>(core::client::ExportPlacementPolicy::Auto),
                         static_cast<int>(defaults.execution.placementPolicy)));
    const QUuid preferredId =
        QUuid::fromString(m_settings.value(QStringLiteral("preferredWorkerProfileId")).toString());
    if (!preferredId.isNull())
    {
        defaults.execution.preferredWorkerProfileId =
            core::client::WorkerProfileId{preferredId.toString(QUuid::WithoutBraces).toStdString()};
    }
    m_settings.endGroup();
    if (m_settings.status() != QSettings::NoError)
    {
        return core::client::ExportDefaultsResult::failure(
            makeError(core::client::ClientErrorCode::DatabaseError, "Unable to load Export defaults."));
    }
    return core::client::ExportDefaultsResult::success(std::move(defaults));
}

// 목적: 성공한 Export의 Qt-free raster option을 기존 application key에 저장
// 입력: options: 저장할 format, encoding, dimension, color와 metadata option
// 출력: 저장된 option 또는 validation·저장소 오류
core::client::ExportRasterDefaultsResult QtExportSettingsAdapter::saveRasterExportDefaults(
    const core::client::ExportRasterOptions& options)
{
    if (const std::optional<core::client::ClientError> error = validateRasterOptions(options); error.has_value())
    {
        return core::client::ExportRasterDefaultsResult::failure(*error);
    }
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
    return m_settings.status() == QSettings::NoError
               ? core::client::ExportRasterDefaultsResult::success(options)
               : core::client::ExportRasterDefaultsResult::failure(
                     makeError(core::client::ClientErrorCode::DatabaseError, "Unable to save raster Export defaults."));
}

// 목적: accepted Export의 placement와 preferred Worker identity를 기존 application key에 저장
// 입력: defaults: Local/Remote/Auto policy와 optional stable Worker identity
// 출력: 저장된 normalized 기본값 또는 validation·저장소 오류
core::client::ExportExecutionDefaultsResult QtExportSettingsAdapter::saveExportExecutionDefaults(
    const core::client::ExportExecutionDefaults& defaults)
{
    if (!isSupportedPlacementPolicy(defaults.placementPolicy))
    {
        return core::client::ExportExecutionDefaultsResult::failure(
            makeError(core::client::ClientErrorCode::InvalidArgument, "Export default placement policy is invalid."));
    }
    const auto normalizedId = normalizedWorkerProfileId(defaults.preferredWorkerProfileId);
    if (normalizedId.hasError())
    {
        return core::client::ExportExecutionDefaultsResult::failure(normalizedId.error());
    }
    core::client::ExportExecutionDefaults normalized{defaults.placementPolicy, normalizedId.value()};
    m_settings.beginGroup(QStringLiteral("export"));
    m_settings.setValue(QStringLiteral("executionMode"), static_cast<int>(normalized.placementPolicy));
    if (normalized.preferredWorkerProfileId.has_value())
    {
        m_settings.setValue(QStringLiteral("preferredWorkerProfileId"),
                            QString::fromStdString(normalized.preferredWorkerProfileId->value));
    }
    else
    {
        m_settings.remove(QStringLiteral("preferredWorkerProfileId"));
    }
    m_settings.endGroup();
    m_settings.sync();
    return m_settings.status() == QSettings::NoError
               ? core::client::ExportExecutionDefaultsResult::success(std::move(normalized))
               : core::client::ExportExecutionDefaultsResult::failure(makeError(
                     core::client::ClientErrorCode::DatabaseError, "Unable to save Export execution defaults."));
}

}  // namespace flexraw::ui::settings
