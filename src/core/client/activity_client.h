#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "client_error.h"
#include "client_identity.h"
#include "client_result.h"

namespace flexraw::core::client
{

enum class ActivityKind : std::uint8_t
{
    Preview,
    FolderScan,
    SourceVerification,
};

struct ActivityId
{
    ActivityKind kind{ActivityKind::Preview};
    std::uint64_t value{0};

    bool operator==(const ActivityId&) const = default;
};

struct ActiveActivity
{
    ActivityId id;
    bool canCancel{false};
    std::optional<ClientPhotoId> photoId;

    bool operator==(const ActiveActivity&) const = default;
};

enum class ActivityTerminalState : std::uint8_t
{
    Completed,
    Failed,
    Cancelled,
};

struct ActivityTerminal
{
    ActivityId id;
    ActivityTerminalState state{ActivityTerminalState::Completed};
    std::optional<ClientError> error;

    bool operator==(const ActivityTerminal&) const = default;
};

struct ActivityEventSequence
{
    std::uint64_t value{0};

    bool operator==(const ActivityEventSequence&) const = default;
};

struct ActivityEvent
{
    ActivityEventSequence sequence;
    bool initial{false};
    std::vector<ActiveActivity> activeActivities;
    std::optional<ActivityTerminal> terminal;

    bool operator==(const ActivityEvent&) const = default;
};

using ActivityCallback = std::function<void(const ActivityEvent&)>;

class IActivitySubscription
{
public:
    // 목적: implementation별 Activity subscription resource를 올바른 concrete destructor로 정리
    // 입력: 없음
    // 출력: 없음
    virtual ~IActivitySubscription() = default;

    // 목적: queued event와 이후 Activity callback 전달을 차단
    // 입력: 없음
    // 출력: 없음; 여러 번 호출해도 같은 inactive 상태 유지
    virtual void unsubscribe() noexcept = 0;

    // 목적: subscription이 이후 callback을 받을 수 있는지 조회
    // 입력: 없음
    // 출력: callback 전달이 허용된 상태이면 true
    [[nodiscard]] virtual bool isActive() const noexcept = 0;
};

using ActivitySubscriptionHandle = std::shared_ptr<IActivitySubscription>;
using ActivitySubscriptionResult = ClientResult<ActivitySubscriptionHandle, ClientError>;
using ActivityCancelResult = ClientResult<ActivityId, ClientError>;

class IActivityClient
{
public:
    // 목적: implementation별 Activity client resource를 올바른 concrete destructor로 정리
    // 입력: 없음
    // 출력: 없음
    virtual ~IActivityClient() = default;

    // 목적: adapter delivery context에서 initial active 목록과 이후 lifecycle event 구독
    // 입력: callback: immutable Activity event consumer
    // 출력: unsubscribe lifetime handle 또는 callback·delivery context 오류
    [[nodiscard]] virtual ActivitySubscriptionResult subscribeToActivities(ActivityCallback callback) = 0;

    // 목적: cancellable Activity의 실제 owner에 cancellation command 전달
    // 입력: activityId: kind와 owner request identity
    // 출력: owner가 cancellation을 수락한 identity 또는 stale·unsupported 오류
    [[nodiscard]] virtual ActivityCancelResult cancelActivity(ActivityId activityId) = 0;
};

}  // namespace flexraw::core::client
