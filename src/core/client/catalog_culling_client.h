#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "client_error.h"
#include "client_identity.h"
#include "client_result.h"

namespace flexraw::core::client
{

inline constexpr std::size_t MaximumCatalogCullingBatchSize = 200;

enum class CatalogCullingState : std::uint8_t
{
    Neutral = 0,
    Pick = 1,
    Reject = 2,
};

enum class CatalogCullingFilter : std::uint8_t
{
    All = 0,
    Pick = 1,
    Reject = 2,
};

struct SetCatalogCullingCommand
{
    std::string expectedCatalogPath;
    CatalogCullingState state{CatalogCullingState::Neutral};
    std::vector<ClientPhotoId> photoIds;
};

struct CatalogCullingReceipt
{
    CatalogCullingState state{CatalogCullingState::Neutral};
    std::vector<ClientPhotoId> photoIds;

    bool operator==(const CatalogCullingReceipt&) const = default;
};

using CatalogCullingResult = ClientResult<CatalogCullingReceipt, ClientError>;

class ICatalogCullingClient
{
public:
    // 목적: implementation별 resource를 올바른 concrete destructor로 정리
    // 입력: 없음
    // 출력: 없음
    virtual ~ICatalogCullingClient() = default;

    // 목적: expected active Catalog의 Photo batch에 지속성 있는 선별 상태 적용
    // 입력: command: Catalog path, target state와 stable Photo identity 목록
    // 출력: deterministic하게 정규화된 identity receipt 또는 구조화된 오류
    [[nodiscard]] virtual CatalogCullingResult setCatalogCulling(const SetCatalogCullingCommand& command) = 0;
};

}  // namespace flexraw::core::client
