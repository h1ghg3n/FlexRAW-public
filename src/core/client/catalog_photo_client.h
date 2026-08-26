#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "client_error.h"
#include "client_identity.h"
#include "client_result.h"

namespace flexraw::core::client
{

inline constexpr std::int32_t DefaultCatalogPhotoPageSize = 100;
inline constexpr std::int32_t MaximumCatalogPhotoPageSize = 200;

enum class CatalogPhotoPageDirection : std::uint8_t
{
    Forward,
    Backward,
};

enum class CatalogFileKind : std::uint8_t
{
    Unknown,
    Raw,
    RasterImage,
};

enum class CatalogScanStatus : std::uint8_t
{
    Pending,
    Ready,
    Unsupported,
    Failed,
};

enum class CatalogSourceState : std::uint8_t
{
    FingerprintPending,
    Available,
    Missing,
    VerificationRequired,
    IdentityUnverified,
    ReplacementDetected,
    Unreadable,
    Unlinked,
};

struct CatalogSourceFingerprint
{
    std::int64_t sizeBytes{0};
    std::int64_t modifiedAtMs{0};
    std::vector<std::uint8_t> sha256;

    bool operator==(const CatalogSourceFingerprint&) const = default;
};

struct CatalogPhotoSnapshot
{
    ClientPhotoId id;
    std::optional<std::string> sourcePath;
    std::string lastKnownPath;
    std::string extension;
    std::string displayName;
    CatalogFileKind kind{CatalogFileKind::Unknown};
    CatalogScanStatus scanStatus{CatalogScanStatus::Pending};
    CatalogSourceFingerprint fingerprint;
    CatalogSourceState sourceState{CatalogSourceState::FingerprintPending};

    bool operator==(const CatalogPhotoSnapshot&) const = default;
};

struct CatalogPhotoPageCursor
{
    std::string displayName;
    ClientPhotoId photoId;
    std::optional<std::string> exactFolderPath;
    std::optional<ClientProjectId> projectId;

    bool operator==(const CatalogPhotoPageCursor&) const = default;
};

struct CatalogPhotoPageRequest
{
    std::int32_t pageSize{DefaultCatalogPhotoPageSize};
    CatalogPhotoPageDirection direction{CatalogPhotoPageDirection::Forward};
    std::optional<CatalogPhotoPageCursor> cursor;
    std::optional<std::string> exactFolderPath;
    std::optional<ClientProjectId> projectId;
};

struct CatalogPhotoPage
{
    std::vector<CatalogPhotoSnapshot> photos;
    std::optional<CatalogPhotoPageCursor> previousCursor;
    std::optional<CatalogPhotoPageCursor> nextCursor;
};

using CatalogPhotoPageResult = ClientResult<CatalogPhotoPage, ClientError>;

class ICatalogPhotoClient
{
public:
    // 목적: implementation별 resource를 올바른 concrete destructor로 정리
    // 입력: 없음
    // 출력: 없음
    virtual ~ICatalogPhotoClient() = default;

    // 목적: active Catalog의 optional Folder/Project scope에서 bounded keyset photo page 조회
    // 입력: request: page 크기, 방향, optional exclusive cursor와 UTF-8 scope
    // 출력: stable order photo snapshot과 양방향 cursor 또는 구조화된 오류
    [[nodiscard]] virtual CatalogPhotoPageResult queryPhotoPage(const CatalogPhotoPageRequest& request) const = 0;
};

}  // namespace flexraw::core::client
