#include "qt_catalog_startup_settings_adapter.h"

#include <string>
#include <utility>

#include <QDir>
#include <QSettings>
#include <QStandardPaths>
#include <QString>

namespace flexraw::ui::settings
{
namespace
{

constexpr auto CatalogSettingsGroup = "catalog";
constexpr auto StartupBehaviorKey = "startupBehavior";
constexpr auto ReopenLastActiveValue = "reopen-last-active";
constexpr auto OpenManagedCatalogValue = "open-managed-catalog";
constexpr auto ManagedCatalogFileName = "Flexraw.flexraw-catalog";

// 목적: Catalog startup settings adapter의 diagnostic-only common error 생성
// 입력: code: machine-readable category, message: log/test detail
// 출력: frontend-neutral ClientError
[[nodiscard]] core::client::ClientError makeError(core::client::ClientErrorCode code, std::string message)
{
    return {code, std::move(message)};
}

// 목적: OS별 per-user local application data 아래 Managed Catalog 경로 resolve
// 입력: 없음
// 출력: 현재 application identity에 대응하는 기본 Catalog 절대 경로
[[nodiscard]] QString defaultManagedCatalogPath()
{
    const QString applicationDataPath = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    return applicationDataPath.isEmpty() ? QString{} : QDir(applicationDataPath).filePath(ManagedCatalogFileName);
}

// 목적: 저장된 문자열을 지원하는 Catalog startup 정책으로 변환
// 입력: storedValue: QSettings의 persistence 문자열
// 출력: 지원하는 정책, 알 수 없거나 비어 있으면 기존 기본 동작
[[nodiscard]] core::client::CatalogStartupBehavior startupBehaviorFromStoredValue(const QString& storedValue)
{
    if (storedValue == QString::fromLatin1(OpenManagedCatalogValue))
    {
        return core::client::CatalogStartupBehavior::OpenManagedCatalog;
    }
    return core::client::CatalogStartupBehavior::ReopenLastActive;
}

// 목적: Catalog startup 정책을 안정적인 persistence 문자열로 변환
// 입력: behavior: frontend-neutral startup 정책
// 출력: 지원하는 정책이면 저장 문자열, 아니면 빈 문자열
[[nodiscard]] QString storedValueFromStartupBehavior(const core::client::CatalogStartupBehavior behavior)
{
    switch (behavior)
    {
    case core::client::CatalogStartupBehavior::ReopenLastActive:
        return QString::fromLatin1(ReopenLastActiveValue);
    case core::client::CatalogStartupBehavior::OpenManagedCatalog:
        return QString::fromLatin1(OpenManagedCatalogValue);
    }
    return {};
}

}  // namespace

// 목적: application QSettings와 OS Managed Catalog 위치를 frontend-neutral startup settings 계약에 연결
// 입력: settings: adapter보다 오래 유지되는 application settings
// 출력: 기존 Catalog runtime state와 분리된 application-scoped settings adapter
QtCatalogStartupSettingsAdapter::QtCatalogStartupSettingsAdapter(QSettings& settings) : m_settings(settings) {}

// 목적: 저장된 startup 정책을 검증하고 현재 OS Managed Catalog 위치와 함께 조회
// 입력: 없음
// 출력: 유효한 startup settings snapshot 또는 저장소·경로 오류
core::client::CatalogStartupSettingsResult QtCatalogStartupSettingsAdapter::catalogStartupSettings() const
{
    m_settings.beginGroup(QString::fromLatin1(CatalogSettingsGroup));
    const QString storedBehavior = m_settings.value(QString::fromLatin1(StartupBehaviorKey)).toString();
    m_settings.endGroup();
    if (m_settings.status() != QSettings::NoError)
    {
        return core::client::CatalogStartupSettingsResult::failure(
            makeError(core::client::ClientErrorCode::DatabaseError, "Unable to load Catalog startup settings."));
    }

    const QString managedCatalogPath = defaultManagedCatalogPath();
    if (managedCatalogPath.isEmpty())
    {
        return core::client::CatalogStartupSettingsResult::failure(
            makeError(core::client::ClientErrorCode::NotFound, "Managed Catalog location is unavailable."));
    }

    return core::client::CatalogStartupSettingsResult::success(
        {startupBehaviorFromStoredValue(storedBehavior), managedCatalogPath.toUtf8().toStdString()});
}

// 목적: 다음 application startup에 사용할 Catalog 선택 정책 저장
// 입력: behavior: 마지막 active Catalog 재개 또는 Managed Catalog 강제 선택
// 출력: 저장된 정책과 현재 Managed Catalog 위치 또는 validation·저장소 오류
core::client::CatalogStartupSettingsResult QtCatalogStartupSettingsAdapter::saveCatalogStartupBehavior(
    const core::client::CatalogStartupBehavior behavior)
{
    const QString storedBehavior = storedValueFromStartupBehavior(behavior);
    if (storedBehavior.isEmpty())
    {
        return core::client::CatalogStartupSettingsResult::failure(
            makeError(core::client::ClientErrorCode::InvalidArgument, "Catalog startup behavior is invalid."));
    }

    m_settings.beginGroup(QString::fromLatin1(CatalogSettingsGroup));
    m_settings.setValue(QString::fromLatin1(StartupBehaviorKey), storedBehavior);
    m_settings.endGroup();
    m_settings.sync();
    if (m_settings.status() != QSettings::NoError)
    {
        return core::client::CatalogStartupSettingsResult::failure(
            makeError(core::client::ClientErrorCode::DatabaseError, "Unable to save Catalog startup settings."));
    }
    return catalogStartupSettings();
}

}  // namespace flexraw::ui::settings
