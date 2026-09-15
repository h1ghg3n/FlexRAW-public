#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "catalog_photo_client.h"
#include "client_error.h"
#include "client_identity.h"
#include "client_result.h"
#include "display_frame.h"

namespace flexraw::core::client
{

inline constexpr std::size_t MaximumCatalogThumbnailWindowSize = 200;

enum class CatalogThumbnailIdentityKind : std::uint8_t
{
    CatalogPhoto,
    TransientSource,
};

struct CatalogThumbnailItemIdentity
{
    CatalogThumbnailIdentityKind kind{CatalogThumbnailIdentityKind::TransientSource};
    ClientPhotoId photoId;
    std::string transientSourceLocator;

    bool operator==(const CatalogThumbnailItemIdentity&) const = default;
};

struct CatalogThumbnailTargetExtent
{
    std::uint32_t width{0};
    std::uint32_t height{0};

    bool operator==(const CatalogThumbnailTargetExtent&) const = default;
};

struct CatalogThumbnailItem
{
    CatalogThumbnailItemIdentity identity;
    std::string sourceLocator;
    std::string extension;
    std::string displayName;
    CatalogFileKind kind{CatalogFileKind::Unknown};

    bool operator==(const CatalogThumbnailItem&) const = default;
};

struct ReplaceCatalogThumbnailWindowCommand
{
    std::vector<CatalogThumbnailItem> items;
    CatalogThumbnailTargetExtent targetExtent;
};

struct CatalogThumbnailWindowGeneration
{
    std::uint64_t value{0};

    bool operator==(const CatalogThumbnailWindowGeneration&) const = default;
};

struct CatalogThumbnailWindowReceipt
{
    CatalogThumbnailWindowGeneration generation;
    std::uint32_t acceptedItemCount{0};

    bool operator==(const CatalogThumbnailWindowReceipt&) const = default;
};

struct CatalogThumbnailWindowSnapshot
{
    std::optional<CatalogThumbnailWindowGeneration> activeGeneration;
    std::optional<CatalogThumbnailTargetExtent> targetExtent;
    std::uint32_t requestedItemCount{0};
    std::uint32_t settledItemCount{0};

    bool operator==(const CatalogThumbnailWindowSnapshot&) const = default;
};

struct CatalogThumbnailFrameSnapshot
{
    CatalogThumbnailWindowGeneration generation;
    CatalogThumbnailItemIdentity identity;
    DisplayFrame frame;
};

struct CatalogThumbnailIssue
{
    CatalogThumbnailWindowGeneration generation;
    CatalogThumbnailItemIdentity identity;
    ClientError error;

    bool operator==(const CatalogThumbnailIssue&) const = default;
};

enum class CatalogThumbnailTerminalState : std::uint8_t
{
    Completed,
    Failed,
    Cancelled,
};

struct CatalogThumbnailTerminal
{
    CatalogThumbnailWindowGeneration generation;
    CatalogThumbnailTerminalState state{CatalogThumbnailTerminalState::Completed};
    std::optional<ClientError> error;

    bool operator==(const CatalogThumbnailTerminal&) const = default;
};

struct CatalogThumbnailEventSequence
{
    std::uint64_t value{0};

    bool operator==(const CatalogThumbnailEventSequence&) const = default;
};

struct CatalogThumbnailEvent
{
    CatalogThumbnailEventSequence sequence;
    bool initial{false};
    CatalogThumbnailWindowSnapshot snapshot;
    bool windowStarted{false};
    std::optional<CatalogThumbnailFrameSnapshot> frame;
    std::optional<CatalogThumbnailIssue> issue;
    std::optional<CatalogThumbnailTerminal> terminal;
};

using CatalogThumbnailWindowResult = ClientResult<CatalogThumbnailWindowReceipt, ClientError>;
using CatalogThumbnailClearResult = ClientResult<std::monostate, ClientError>;
using CatalogThumbnailCallback = std::function<void(const CatalogThumbnailEvent&)>;

class ICatalogThumbnailSubscription
{
public:
    // 목적: implementation별 Catalog thumbnail subscription resource를 concrete destructor로 정리
    // 입력: 없음
    // 출력: 없음
    virtual ~ICatalogThumbnailSubscription() = default;

    // 목적: queued event와 이후 Catalog thumbnail callback 전달 차단
    // 입력: 없음
    // 출력: 없음; 여러 번 호출해도 같은 inactive 상태 유지
    virtual void unsubscribe() noexcept = 0;

    // 목적: subscription이 이후 callback을 받을 수 있는지 조회
    // 입력: 없음
    // 출력: callback 전달이 허용된 상태이면 true
    [[nodiscard]] virtual bool isActive() const noexcept = 0;
};

using CatalogThumbnailSubscriptionHandle = std::shared_ptr<ICatalogThumbnailSubscription>;
using CatalogThumbnailSubscriptionResult = ClientResult<CatalogThumbnailSubscriptionHandle, ClientError>;

class ICatalogThumbnailClient
{
public:
    // 목적: implementation별 Catalog thumbnail command resource를 concrete destructor로 정리
    // 입력: 없음
    // 출력: 없음
    virtual ~ICatalogThumbnailClient() = default;

    // 목적: 현재 visible/adjacent thumbnail window를 새 bounded item 집합으로 교체
    // 입력: command: tagged identity, normalized source locator와 양수 target pixel 크기
    // 출력: owner가 발급한 generation과 중복 제거 후 item 수 또는 validation·shutdown 오류
    [[nodiscard]] virtual CatalogThumbnailWindowResult replaceThumbnailWindow(
        const ReplaceCatalogThumbnailWindowCommand& command) = 0;

    // 목적: active thumbnail window와 pending decode를 idempotent하게 정리
    // 입력: 없음
    // 출력: active generation이 있으면 Cancelled terminal을 발행한 성공 또는 shutdown 오류
    [[nodiscard]] virtual CatalogThumbnailClearResult clearThumbnailWindow() = 0;
};

class ICatalogThumbnailEventSource
{
public:
    // 목적: implementation별 Catalog thumbnail event resource를 concrete destructor로 정리
    // 입력: 없음
    // 출력: 없음
    virtual ~ICatalogThumbnailEventSource() = default;

    // 목적: adapter delivery context에서 initial snapshot과 이후 frame·issue·terminal 구독
    // 입력: callback: immutable Catalog thumbnail event consumer
    // 출력: unsubscribe lifetime handle 또는 callback·delivery context 오류
    [[nodiscard]] virtual CatalogThumbnailSubscriptionResult subscribeToCatalogThumbnails(
        CatalogThumbnailCallback callback) = 0;
};

}  // namespace flexraw::core::client
