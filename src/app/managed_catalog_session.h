#pragma once

#include <memory>
#include <optional>

#include <QString>

#include "catalog_contracts.h"
#include "error.h"

class QSettings;

namespace flexraw::core::orchestration
{
class CatalogOrchestrator;
}

namespace flexraw::app
{

struct ManagedCatalogRecoveryIssue
{
    QString catalogPath;
    core::types::CoreError error;
};

struct ManagedCatalogStartupState
{
    core::orchestration::CatalogSessionState session;
    QString requestedCatalogPath;
    QString defaultCatalogPath;
    std::optional<ManagedCatalogRecoveryIssue> recoveryIssue;
    std::optional<core::types::CoreError> fatalError;
    bool usedFallback{false};
};

class ManagedCatalogSession final
{
public:
    // 목적: OS 표준 application data와 기본 QSettings를 사용하는 Managed Catalog session 시작
    // 입력: 없음
    // 출력: startup recovery가 완료된 production catalog session
    ManagedCatalogSession();

    // 목적: 주입된 settings와 기본 catalog 경로로 test 가능한 Managed Catalog session 시작
    // 입력: settings: session이 소유할 설정 저장소, defaultCatalogPath: fallback catalog 파일 경로
    // 출력: startup recovery가 완료된 catalog session
    ManagedCatalogSession(std::unique_ptr<QSettings> settings, QString defaultCatalogPath);

    // 목적: 현재 active Catalog를 다음 startup 대상으로 기록한 뒤 session resource 정리
    // 입력: 없음
    // 출력: clean shutdown 시 마지막 active Catalog 경로가 settings에 저장됨
    ~ManagedCatalogSession();

    ManagedCatalogSession(const ManagedCatalogSession&) = delete;
    ManagedCatalogSession& operator=(const ManagedCatalogSession&) = delete;

    // 목적: session이 소유한 CatalogOrchestrator를 downstream consumer에 주입
    // 입력: 없음
    // 출력: ManagedCatalogSession lifetime 동안 유효한 Orchestrator 참조
    [[nodiscard]] core::orchestration::CatalogOrchestrator& orchestrator() noexcept;

    // 목적: startup open, fallback과 fatal 상태를 immutable 값으로 조회
    // 입력: 없음
    // 출력: 현재 application startup의 Managed Catalog 결과
    [[nodiscard]] const ManagedCatalogStartupState& startupState() const noexcept;

private:
    // 목적: 마지막 active Catalog 또는 기본 Managed Catalog를 안전하게 open/recover
    // 입력: 없음
    // 출력: active session, recovery issue와 fatal error를 포함한 startup state
    [[nodiscard]] ManagedCatalogStartupState startCatalog();

    // 목적: 기본 Managed Catalog 부모 directory를 생성하고 catalog open
    // 입력: 없음
    // 출력: 열린 기본 session 또는 path/database 오류
    [[nodiscard]] core::orchestration::CatalogSessionResult openDefaultCatalog();

    // 목적: unavailable recent Catalog 정보를 settings에 보존
    // 입력: issue: 실패한 catalog path와 구조화된 오류
    // 출력: 다음 startup과 future recent UI가 읽을 recovery record 저장
    void recordRecoveryIssue(const ManagedCatalogRecoveryIssue& issue);

    // 목적: 이전 unavailable Catalog가 다시 정상 open됐을 때 recovery marker 제거
    // 입력: catalogPath: 정상 open된 catalog 절대 경로
    // 출력: 동일 path의 stale recovery record가 제거됨
    void clearResolvedRecoveryIssue(const QString& catalogPath);

    // 목적: clean shutdown 시 현재 active Catalog를 다음 startup 대상으로 기록
    // 입력: 없음
    // 출력: 열린 session이 있으면 last-active path가 settings에 저장됨
    void rememberActiveCatalog();

    std::unique_ptr<QSettings> m_settings;
    QString m_defaultCatalogPath;
    std::unique_ptr<core::orchestration::CatalogOrchestrator> m_orchestrator;
    ManagedCatalogStartupState m_startupState;
};

}  // namespace flexraw::app
