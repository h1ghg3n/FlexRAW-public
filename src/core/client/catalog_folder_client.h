#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "client_error.h"
#include "client_result.h"

namespace flexraw::core::client
{

struct CatalogFolderSnapshot
{
    std::string path;
    std::int64_t photoCount{0};

    bool operator==(const CatalogFolderSnapshot&) const = default;
};

using CatalogFolderListResult = ClientResult<std::vector<CatalogFolderSnapshot>, ClientError>;

class ICatalogFolderClient
{
public:
    // 목적: implementation별 resource를 올바른 concrete destructor로 정리
    // 입력: 없음
    // 출력: 없음
    virtual ~ICatalogFolderClient() = default;

    // 목적: active Catalog에서 linked Photo가 존재하는 distinct Folder snapshot 조회
    // 입력: 없음
    // 출력: normalized UTF-8 path 순서와 양수 photo 수 또는 구조화된 client 오류
    [[nodiscard]] virtual CatalogFolderListResult listFolders() const = 0;
};

}  // namespace flexraw::core::client
