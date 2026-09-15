#pragma once

#include "catalog_session_client.h"

namespace flexraw::core::client
{
class IEditorClient;
}

namespace flexraw::core::orchestration
{

class CatalogOrchestrator;

class CatalogSessionOrchestrator final : public client::ICatalogSessionClient
{
public:
    // 목적: Catalog resource owner와 Editor state owner를 하나의 session transition use case로 조립
    // 입력: catalogOrchestrator: database session owner, editorClient: dirty/selection authority
    // 출력: 두 dependency lifetime 안에서 동작하는 CatalogSessionOrchestrator
    CatalogSessionOrchestrator(CatalogOrchestrator& catalogOrchestrator, client::IEditorClient& editorClient) noexcept;

    // 목적: 현재 active Catalog session을 immutable Qt-free snapshot으로 조회
    // 입력: 없음
    // 출력: open 여부와 open 상태에서만 채워지는 normalized absolute UTF-8 catalog path
    [[nodiscard]] client::CatalogSessionSnapshot catalogSnapshot() const override;

    // 목적: 명시적 create/open 및 optional clean-session 교체 정책으로 Catalog 활성화
    // 입력: command: UTF-8 path, create/open mode와 active session replacement 정책
    // 출력: 열린 normalized absolute snapshot 또는 validation·permission·database·conflict 오류
    [[nodiscard]] client::CatalogSessionResult openCatalog(const client::OpenCatalogCommand& command) override;

    // 목적: dirty Editor state를 보존하면서 clean Catalog session을 idempotent하게 종료
    // 입력: 없음
    // 출력: 닫힌 snapshot 또는 unsaved Editor conflict 오류
    [[nodiscard]] client::CatalogSessionResult closeCatalog() override;

private:
    CatalogOrchestrator* m_catalogOrchestrator{nullptr};
    client::IEditorClient* m_editorClient{nullptr};
};

}  // namespace flexraw::core::orchestration
