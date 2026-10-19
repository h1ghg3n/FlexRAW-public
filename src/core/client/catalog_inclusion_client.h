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

inline constexpr std::size_t MaximumCatalogInclusionBatchSize = 200;

enum class CatalogInclusionState : std::uint8_t
{
    Included = 0,
    Excluded = 1,
};

enum class CatalogInclusionFilter : std::uint8_t
{
    IncludedOnly = 0,
    ExcludedOnly = 1,
};

struct SetCatalogInclusionCommand
{
    std::string expectedCatalogPath;
    CatalogInclusionState state{CatalogInclusionState::Included};
    std::vector<ClientPhotoId> photoIds;
};

struct CatalogInclusionReceipt
{
    CatalogInclusionState state{CatalogInclusionState::Included};
    std::vector<ClientPhotoId> photoIds;

    bool operator==(const CatalogInclusionReceipt&) const = default;
};

using CatalogInclusionResult = ClientResult<CatalogInclusionReceipt, ClientError>;

class ICatalogInclusionClient
{
public:
    // 목적: implementation별 resource를 올바른 concrete destructor로 정리
    // 입력: 없음
    // 출력: 없음
    virtual ~ICatalogInclusionClient() = default;

    // 목적: expected active Catalog의 Photo batch에 지속성 있는 등록 포함 상태 적용
    // 입력: command: Catalog path, target state와 stable Photo identity 목록
    // 출력: deterministic하게 정규화된 identity receipt 또는 구조화된 오류
    [[nodiscard]] virtual CatalogInclusionResult setCatalogInclusion(const SetCatalogInclusionCommand& command) = 0;
};

}  // namespace flexraw::core::client
