#include "managed_catalog_session.h"

#include <stdexcept>
#include <utility>

#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>

#include "catalog_orchestrator.h"

namespace flexraw::app
{
namespace
{

constexpr auto CatalogSettingsGroup = "catalog";
constexpr auto LastActiveCatalogKey = "lastActivePath";
constexpr auto RecoveryPathKey = "recovery/unavailablePath";
constexpr auto RecoveryErrorCodeKey = "recovery/errorCode";
constexpr auto RecoveryErrorMessageKey = "recovery/errorMessage";
constexpr auto ManagedCatalogFileName = "Flexraw.flexraw-catalog";

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

// 목적: 비어 있지 않은 catalog path를 비교 가능한 절대 경로로 정규화
// 입력: catalogPath: settings 또는 caller가 전달한 catalog 경로
// 출력: 정규화된 절대 경로, 입력이 비어 있으면 빈 문자열
[[nodiscard]] QString normalizedCatalogPath(const QString& catalogPath)
{
    const QString trimmedPath = catalogPath.trimmed();
    return trimmedPath.isEmpty() ? QString{} : QFileInfo(trimmedPath).absoluteFilePath();
}

// 목적: 두 catalog locator가 같은 정규화 경로인지 확인
// 입력: left/right: 비교할 catalog 경로
// 출력: 정규화된 경로 문자열이 같으면 true
[[nodiscard]] bool isSameCatalogPath(const QString& left, const QString& right)
{
    return normalizedCatalogPath(left) == normalizedCatalogPath(right);
}

// 목적: Managed Catalog startup path 오류를 구조화된 CoreError로 생성
// 입력: code: 오류 분류, message: technical 설명
// 출력: Catalog startup state에 저장할 CoreError
[[nodiscard]] core::types::CoreError makeStartupError(core::types::ErrorCode code, QString message)
{
    return {code, std::move(message)};
}

// 목적: OS별 per-user local application data 아래 기본 Managed Catalog 경로 resolve
// 입력: 없음
// 출력: 기본 catalog 절대 경로, 표준 위치를 얻지 못하면 빈 문자열
[[nodiscard]] QString defaultManagedCatalogPath()
{
    const QString applicationDataPath = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    return applicationDataPath.isEmpty() ? QString{} : QDir(applicationDataPath).filePath(ManagedCatalogFileName);
}

}  // namespace

// 목적: OS 표준 application data와 기본 QSettings를 사용하는 Managed Catalog session 시작
// 입력: 없음
// 출력: startup recovery가 완료된 production catalog session
ManagedCatalogSession::ManagedCatalogSession()
    : ManagedCatalogSession(std::make_unique<QSettings>(), defaultManagedCatalogPath())
{}

// 목적: 주입된 settings와 기본 catalog 경로로 test 가능한 Managed Catalog session 시작
// 입력: settings: session이 소유할 설정 저장소, defaultCatalogPath: fallback catalog 파일 경로
// 출력: startup recovery가 완료된 catalog session
ManagedCatalogSession::ManagedCatalogSession(std::unique_ptr<QSettings> settings, QString defaultCatalogPath)
    : m_settings(requireSettings(std::move(settings))),
      m_defaultCatalogPath(normalizedCatalogPath(defaultCatalogPath)),
      m_orchestrator(std::make_unique<core::orchestration::CatalogOrchestrator>())
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

// 목적: session이 소유한 CatalogOrchestrator를 downstream consumer에 주입
// 입력: 없음
// 출력: ManagedCatalogSession lifetime 동안 유효한 Orchestrator 참조
core::orchestration::CatalogOrchestrator& ManagedCatalogSession::orchestrator() noexcept
{
    return *m_orchestrator;
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

    m_settings->beginGroup(QString::fromLatin1(CatalogSettingsGroup));
    const QString configuredCatalogPath =
        normalizedCatalogPath(m_settings->value(QString::fromLatin1(LastActiveCatalogKey)).toString());
    m_settings->endGroup();

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
        const core::orchestration::CatalogSessionResult opened = m_orchestrator->openCatalog(configuredCatalogPath);
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
        isSameCatalogPath(configuredCatalogPath, m_defaultCatalogPath) && !QFileInfo::exists(configuredCatalogPath);
    if (isSameCatalogPath(configuredCatalogPath, m_defaultCatalogPath) && !missingDefaultCatalog)
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

    return m_orchestrator->openCatalog(m_defaultCatalogPath);
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
    if (isSameCatalogPath(unavailablePath, catalogPath))
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
    const core::orchestration::CatalogSessionState session = m_orchestrator->state();
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
