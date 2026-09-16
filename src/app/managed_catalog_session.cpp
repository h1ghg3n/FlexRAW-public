#include "managed_catalog_session.h"

#include <stdexcept>
#include <utility>

#include <QDir>
#include <QFileInfo>
#include <QSettings>

#include "catalog_orchestrator.h"
#include "catalog_path.h"
#include "client_error_projection.h"

namespace flexraw::app
{
namespace
{

constexpr auto CatalogSettingsGroup = "catalog";
constexpr auto LastActiveCatalogKey = "lastActivePath";
constexpr auto RecoveryPathKey = "recovery/unavailablePath";
constexpr auto RecoveryErrorCodeKey = "recovery/errorCode";
constexpr auto RecoveryErrorMessageKey = "recovery/errorMessage";

// 목적: settings dependency가 null이 아닌 소유 객체인지 검증
// 입력: settings: caller가 전달한 optional ownership
// 출력: 유효한 settings ownership 또는 invalid_argument 예외
[[nodiscard]] std::unique_ptr<QSettings> requireSettings(std::unique_ptr<QSettings> settings)
{
    if (settings == nullptr)
    {
        throw std::invalid_argument("ManagedCatalogSession requires QSettings.");
    }
    return settings;
}

// 목적: Managed Catalog startup path 오류를 구조화된 CoreError로 생성
// 입력: code: 오류 분류, message: technical 설명
// 출력: Catalog startup state에 저장할 CoreError
[[nodiscard]] core::types::CoreError makeStartupError(core::types::ErrorCode code, QString message)
{
    return {code, std::move(message)};
}

}  // namespace

// 목적: 주입된 Catalog owner와 application startup settings를 사용하는 Managed Catalog session 시작
// 입력: orchestrator: 이 session보다 오래 살아야 하는 Catalog resource owner,
//       startupSettingsClient: startup 시점 snapshot을 제공하는 settings contract
// 출력: startup recovery가 완료된 production catalog session
ManagedCatalogSession::ManagedCatalogSession(core::orchestration::CatalogOrchestrator& orchestrator,
                                             core::client::ICatalogStartupSettingsClient& startupSettingsClient)
    : m_orchestrator(orchestrator), m_settings(std::make_unique<QSettings>())
{
    const core::client::CatalogStartupSettingsResult settings = startupSettingsClient.catalogStartupSettings();
    if (settings.hasError())
    {
        m_startupConfigurationError = core::orchestration::fromClientError(settings.error());
    }
    else
    {
        m_defaultCatalogPath =
            core::catalog::normalizeCatalogPath(QString::fromUtf8(settings.value().managedCatalogPath.c_str()));
        m_startupBehavior = settings.value().behavior;
    }
    m_startupState = startCatalog();
}

// 목적: 주입된 Catalog owner, settings와 resolved startup 값으로 test 가능한 Managed Catalog session 시작
// 입력: orchestrator: session보다 오래 살아야 하는 owner, settings: 소유할 설정,
//       defaultCatalogPath: fallback 경로, startupBehavior: startup Catalog 선택 정책
// 출력: startup recovery가 완료된 catalog session
ManagedCatalogSession::ManagedCatalogSession(core::orchestration::CatalogOrchestrator& orchestrator,
                                             std::unique_ptr<QSettings> settings,
                                             QString defaultCatalogPath,
                                             const core::client::CatalogStartupBehavior startupBehavior)
    : m_orchestrator(orchestrator),
      m_settings(requireSettings(std::move(settings))),
      m_defaultCatalogPath(core::catalog::normalizeCatalogPath(defaultCatalogPath)),
      m_startupBehavior(startupBehavior)
{
    m_startupState = startCatalog();
}

// 목적: 현재 active Catalog를 다음 startup 대상으로 기록한 뒤 session resource 정리
// 입력: 없음
// 출력: clean shutdown 시 마지막 active Catalog 경로가 settings에 저장됨
ManagedCatalogSession::~ManagedCatalogSession()
{
    rememberActiveCatalog();
}

// 목적: startup open, fallback과 fatal 상태를 immutable 값으로 조회
// 입력: 없음
// 출력: 현재 application startup의 Managed Catalog 결과
const ManagedCatalogStartupState& ManagedCatalogSession::startupState() const noexcept
{
    return m_startupState;
}

// 목적: 마지막 active Catalog 또는 기본 Managed Catalog를 안전하게 open/recover
// 입력: 없음
// 출력: active session, recovery issue와 fatal error를 포함한 startup state
ManagedCatalogStartupState ManagedCatalogSession::startCatalog()
{
    ManagedCatalogStartupState startupState;
    startupState.defaultCatalogPath = m_defaultCatalogPath;

    if (m_startupConfigurationError.has_value())
    {
        startupState.fatalError = m_startupConfigurationError;
        return startupState;
    }

    QString configuredCatalogPath;
    switch (m_startupBehavior)
    {
    case core::client::CatalogStartupBehavior::ReopenLastActive:
        m_settings->beginGroup(QString::fromLatin1(CatalogSettingsGroup));
        configuredCatalogPath = core::catalog::normalizeCatalogPath(
            m_settings->value(QString::fromLatin1(LastActiveCatalogKey)).toString());
        m_settings->endGroup();
        break;
    case core::client::CatalogStartupBehavior::OpenManagedCatalog:
        break;
    default:
        startupState.fatalError = makeStartupError(core::types::ErrorCode::InvalidArgument,
                                                   QStringLiteral("Catalog startup behavior is invalid."));
        return startupState;
    }

    startupState.requestedCatalogPath = configuredCatalogPath.isEmpty() ? m_defaultCatalogPath : configuredCatalogPath;

    if (configuredCatalogPath.isEmpty())
    {
        const core::orchestration::CatalogSessionResult opened = openDefaultCatalog();
        if (opened.hasError())
        {
            startupState.fatalError = opened.error();
        }
        else
        {
            startupState.session = opened.value();
        }
        return startupState;
    }

    std::optional<core::types::CoreError> requestedError;
    if (!QFileInfo::exists(configuredCatalogPath))
    {
        requestedError = makeStartupError(core::types::ErrorCode::NotFound,
                                          QStringLiteral("The last active catalog does not exist."));
    }
    else
    {
        const core::orchestration::CatalogSessionResult opened = m_orchestrator.openCatalog(configuredCatalogPath);
        if (!opened.hasError())
        {
            startupState.session = opened.value();
            clearResolvedRecoveryIssue(configuredCatalogPath);
            return startupState;
        }
        requestedError = opened.error();
    }

    startupState.recoveryIssue = ManagedCatalogRecoveryIssue{configuredCatalogPath, *requestedError};
    recordRecoveryIssue(*startupState.recoveryIssue);

    const bool missingDefaultCatalog =
        core::catalog::catalogPathsReferToSameFile(configuredCatalogPath, m_defaultCatalogPath) &&
        !QFileInfo::exists(configuredCatalogPath);
    if (core::catalog::catalogPathsReferToSameFile(configuredCatalogPath, m_defaultCatalogPath) &&
        !missingDefaultCatalog)
    {
        startupState.fatalError = *requestedError;
        return startupState;
    }

    startupState.usedFallback = true;
    const core::orchestration::CatalogSessionResult fallback = openDefaultCatalog();
    if (fallback.hasError())
    {
        startupState.fatalError = fallback.error();
    }
    else
    {
        startupState.session = fallback.value();
    }
    return startupState;
}

// 목적: 기본 Managed Catalog 부모 directory를 생성하고 catalog open
// 입력: 없음
// 출력: 열린 기본 session 또는 path/database 오류
core::orchestration::CatalogSessionResult ManagedCatalogSession::openDefaultCatalog()
{
    if (m_defaultCatalogPath.isEmpty())
    {
        return core::orchestration::CatalogSessionResult::failure(makeStartupError(
            core::types::ErrorCode::InvalidArgument, QStringLiteral("Managed catalog path is unavailable.")));
    }

    const QFileInfo catalogInfo(m_defaultCatalogPath);
    const QString parentPath = catalogInfo.dir().absolutePath();
    if (!QDir(parentPath).exists() && !QDir().mkpath(parentPath))
    {
        return core::orchestration::CatalogSessionResult::failure(makeStartupError(
            core::types::ErrorCode::PermissionDenied, QStringLiteral("Unable to create managed catalog directory.")));
    }

    return m_orchestrator.openCatalog(m_defaultCatalogPath);
}

// 목적: unavailable recent Catalog 정보를 settings에 보존
// 입력: issue: 실패한 catalog path와 구조화된 오류
// 출력: 다음 startup과 future recent UI가 읽을 recovery record 저장
void ManagedCatalogSession::recordRecoveryIssue(const ManagedCatalogRecoveryIssue& issue)
{
    m_settings->beginGroup(QString::fromLatin1(CatalogSettingsGroup));
    m_settings->setValue(QString::fromLatin1(RecoveryPathKey), issue.catalogPath);
    m_settings->setValue(QString::fromLatin1(RecoveryErrorCodeKey), static_cast<int>(issue.error.code));
    m_settings->setValue(QString::fromLatin1(RecoveryErrorMessageKey), issue.error.message);
    m_settings->endGroup();
    m_settings->sync();
}

// 목적: 이전 unavailable Catalog가 다시 정상 open됐을 때 recovery marker 제거
// 입력: catalogPath: 정상 open된 catalog 절대 경로
// 출력: 동일 path의 stale recovery record가 제거됨
void ManagedCatalogSession::clearResolvedRecoveryIssue(const QString& catalogPath)
{
    m_settings->beginGroup(QString::fromLatin1(CatalogSettingsGroup));
    const QString unavailablePath = m_settings->value(QString::fromLatin1(RecoveryPathKey)).toString();
    if (core::catalog::catalogPathsReferToSameFile(unavailablePath, catalogPath))
    {
        m_settings->remove(QStringLiteral("recovery"));
    }
    m_settings->endGroup();
    m_settings->sync();
}

// 목적: clean shutdown 시 현재 active Catalog를 다음 startup 대상으로 기록
// 입력: 없음
// 출력: 열린 session이 있으면 last-active path가 settings에 저장됨
void ManagedCatalogSession::rememberActiveCatalog()
{
    const core::orchestration::CatalogSessionState session = m_orchestrator.state();
    if (!session.isOpen || session.catalogPath.isEmpty())
    {
        return;
    }

    m_settings->beginGroup(QString::fromLatin1(CatalogSettingsGroup));
    m_settings->setValue(QString::fromLatin1(LastActiveCatalogKey), session.catalogPath);
    m_settings->endGroup();
    m_settings->sync();
}

}  // namespace flexraw::app
