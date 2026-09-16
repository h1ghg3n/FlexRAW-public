#pragma once

#include <string>

#include "client_error.h"
#include "client_result.h"

namespace flexraw::core::client
{

enum class CatalogStartupBehavior
{
    ReopenLastActive,
    OpenManagedCatalog,
};

struct CatalogStartupSettingsSnapshot
{
    CatalogStartupBehavior behavior{CatalogStartupBehavior::ReopenLastActive};
    std::string managedCatalogPath;

    bool operator==(const CatalogStartupSettingsSnapshot&) const = default;
};

using CatalogStartupSettingsResult = ClientResult<CatalogStartupSettingsSnapshot, ClientError>;

class ICatalogStartupSettingsClient
{
public:
    // 목적: implementation별 Catalog startup settings resource를 concrete destructor로 정리
    // 입력: 없음
    // 출력: 없음
    virtual ~ICatalogStartupSettingsClient() = default;

    // 목적: 다음 application startup에 적용할 Catalog 동작과 Managed Catalog 위치 조회
    // 입력: 없음
    // 출력: immutable startup settings snapshot 또는 저장소 오류
    [[nodiscard]] virtual CatalogStartupSettingsResult catalogStartupSettings() const = 0;

    // 목적: 다음 application startup에 적용할 Catalog 선택 정책 저장
    // 입력: behavior: 마지막 active Catalog 재개 또는 Managed Catalog 강제 선택
    // 출력: 저장된 정책과 현재 Managed Catalog 위치 또는 validation·저장소 오류
    [[nodiscard]] virtual CatalogStartupSettingsResult saveCatalogStartupBehavior(CatalogStartupBehavior behavior) = 0;
};

}  // namespace flexraw::core::client
