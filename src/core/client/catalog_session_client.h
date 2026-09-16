#pragma once

#include <cstdint>
#include <string>

#include "client_error.h"
#include "client_result.h"

namespace flexraw::core::client
{

enum class CatalogOpenMode : std::uint8_t
{
    OpenExisting,
    CreateNew,
};

enum class CatalogReplacementPolicy : std::uint8_t
{
    Reject,
    ReplaceCurrent,
};

struct CatalogSessionSnapshot
{
    bool isOpen{false};
    std::string catalogPath;

    bool operator==(const CatalogSessionSnapshot&) const = default;
};

struct OpenCatalogCommand
{
    std::string catalogPath;
    CatalogOpenMode openMode{CatalogOpenMode::OpenExisting};
    CatalogReplacementPolicy replacementPolicy{CatalogReplacementPolicy::Reject};
};

using CatalogSessionResult = ClientResult<CatalogSessionSnapshot, ClientError>;

class ICatalogSessionClient
{
public:
    // 목적: implementation별 resource를 올바른 concrete destructor로 정리
    // 입력: 없음
    // 출력: 없음
    virtual ~ICatalogSessionClient() = default;

    // 목적: 현재 active Catalog session을 immutable Qt-free snapshot으로 조회
    // 입력: 없음
    // 출력: open 여부와 open 상태에서만 채워지는 normalized absolute UTF-8 catalog path
    [[nodiscard]] virtual CatalogSessionSnapshot catalogSnapshot() const = 0;

    // 목적: 명시적 create/open 및 optional clean-session 교체 정책으로 Catalog 활성화
    // 입력: command: UTF-8 path, create/open mode와 active session replacement 정책
    // 출력: 열린 normalized absolute snapshot 또는 validation·permission·database·conflict 오류
    [[nodiscard]] virtual CatalogSessionResult openCatalog(const OpenCatalogCommand& command) = 0;

    // 목적: dirty Editor state를 보존하면서 clean Catalog session을 idempotent하게 종료
    // 입력: 없음
    // 출력: 닫힌 snapshot 또는 unsaved Editor conflict 오류
    [[nodiscard]] virtual CatalogSessionResult closeCatalog() = 0;
};

}  // namespace flexraw::core::client
