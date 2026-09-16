#include "qt_activity_adapter.h"

#include <algorithm>
#include <limits>
#include <mutex>
#include <utility>

#include <QMetaObject>
#include <QThread>

#include "log.h"

namespace flexraw::ui::mainwindow
{
namespace
{

// 목적: Folder operation 종류를 status presentation용 Activity 종류로 변환
// 입력: kind: scan 또는 Catalog import
// 출력: 같은 의미의 Activity kind
[[nodiscard]] core::client::ActivityKind toActivityKind(core::client::FolderOperationKind kind) noexcept
{
    return kind == core::client::FolderOperationKind::Import ? core::client::ActivityKind::FolderImport
                                                             : core::client::ActivityKind::FolderScan;
}

// 목적: Folder operation terminal 상태를 공통 Activity terminal 상태로 변환
// 입력: state: completed·failed·cancelled Folder terminal
// 출력: 같은 의미의 Activity terminal state
[[nodiscard]] core::client::ActivityTerminalState toActivityTerminalState(
    core::client::FolderOperationTerminalState state) noexcept
{
    switch (state)
    {
    case core::client::FolderOperationTerminalState::Completed:
        return core::client::ActivityTerminalState::Completed;
    case core::client::FolderOperationTerminalState::Failed:
        return core::client::ActivityTerminalState::Failed;
    case core::client::FolderOperationTerminalState::Cancelled:
        return core::client::ActivityTerminalState::Cancelled;
    }
    return core::client::ActivityTerminalState::Failed;
}

}  // namespace

struct QtActivityAdapter::SubscriptionState
{
    // 목적: queued event와 이후 callback 전달을 idempotent하게 차단
    // 입력: 없음
    // 출력: inactive state와 해제된 callback
    void unsubscribe() noexcept
    {
        const std::scoped_lock lock(mutex);
        active = false;
        callback = {};
    }

    // 목적: subscription callback 전달 가능 상태를 thread-safe하게 조회
    // 입력: 없음
    // 출력: active이면 true
    [[nodiscard]] bool isActive() const noexcept
    {
        const std::scoped_lock lock(mutex);
        return active;
    }

    mutable std::recursive_mutex mutex;
    bool active{true};
    core::client::ActivityCallback callback;
};

class QtActivityAdapter::Subscription final : public core::client::IActivitySubscription
{
public:
    // 목적: public handle과 shared Activity delivery state 연결
    // 입력: state: adapter queued callback과 공유할 lifetime state
    // 출력: active RAII subscription handle
    explicit Subscription(SubscriptionStatePtr state) : m_state(std::move(state)) {}

    // 목적: 마지막 handle 해제 시 queued event와 이후 callback 전달 차단
    // 입력: 없음
    // 출력: inactive subscription
    ~Subscription() override
    {
        unsubscribe();
    }

    // 목적: queued event와 이후 Activity callback 전달을 idempotent하게 차단
    // 입력: 없음
    // 출력: 없음
    void unsubscribe() noexcept override
    {
        m_state->unsubscribe();
    }

    // 목적: subscription이 이후 callback을 받을 수 있는지 조회
    // 입력: 없음
    // 출력: callback 전달이 허용된 상태이면 true
    [[nodiscard]] bool isActive() const noexcept override
    {
        return m_state->isActive();
    }

private:
    SubscriptionStatePtr m_state;
};

// 목적: 기존 Qt owner lifecycle을 하나의 Qt-free Activity client로 집계
// 입력: previewClient: Preview cancel command, preview/folder/source event source와 source client,
//       parent: Qt owner
// 출력: 현재 Qt thread에 bound된 Activity adapter
QtActivityAdapter::QtActivityAdapter(core::client::IPreviewPresentationClient& previewClient,
                                     core::client::IPreviewPresentationEventSource& previewEventSource,
                                     core::client::IFolderImportEventSource& folderEventSource,
                                     core::client::ISourceResolutionClient& sourceResolutionClient,
                                     core::client::ISourceResolutionEventSource& sourceResolutionEventSource,
                                     QObject* parent)
    : QObject(parent), m_previewClient(&previewClient), m_sourceResolutionClient(&sourceResolutionClient)
{
    const core::client::PreviewPresentationSubscriptionResult previewSubscribed =
        previewEventSource.subscribeToPreviewPresentation(
            [this](const core::client::PreviewPresentationEvent& event) { handlePreviewPresentationEvent(event); });
    if (previewSubscribed.hasError())
    {
        LOG_ERROR(
            "activity", "Unable to subscribe to Preview presentation: {}", previewSubscribed.error().technicalMessage);
    }
    else
    {
        m_previewPresentationSubscription = previewSubscribed.value();
    }
    const core::client::SourceResolutionSubscriptionResult sourceSubscribed =
        sourceResolutionEventSource.subscribeToSourceResolution(
            [this](const core::client::SourceResolutionEvent& event) { handleSourceResolutionEvent(event); });
    if (sourceSubscribed.hasError())
    {
        LOG_ERROR(
            "activity", "Unable to subscribe to Source Resolution: {}", sourceSubscribed.error().technicalMessage);
    }
    else
    {
        m_sourceResolutionSubscription = sourceSubscribed.value();
    }
    const core::client::FolderOperationSubscriptionResult subscribed = folderEventSource.subscribeToFolderOperations(
        [this](const core::client::FolderOperationEvent& event) { handleFolderOperationEvent(event); });
    if (subscribed.hasError())
    {
        LOG_ERROR("activity", "Unable to subscribe to Folder operations: {}", subscribed.error().technicalMessage);
    }
    else
    {
        m_folderOperationSubscription = subscribed.value();
    }
}

// 목적: queued callback을 차단하고 outliving Activity subscription을 inactive로 전환
// 입력: 없음
// 출력: adapter destruction 이후 callback 없음
QtActivityAdapter::~QtActivityAdapter()
{
    m_shuttingDown = true;
    if (m_previewPresentationSubscription != nullptr)
    {
        m_previewPresentationSubscription->unsubscribe();
        m_previewPresentationSubscription.reset();
    }
    if (m_folderOperationSubscription != nullptr)
    {
        m_folderOperationSubscription->unsubscribe();
        m_folderOperationSubscription.reset();
    }
    if (m_sourceResolutionSubscription != nullptr)
    {
        m_sourceResolutionSubscription->unsubscribe();
        m_sourceResolutionSubscription.reset();
    }
    for (const std::weak_ptr<SubscriptionState>& weakState : m_subscriptions)
    {
        if (const SubscriptionStatePtr state = weakState.lock(); state != nullptr)
        {
            state->unsubscribe();
        }
    }
    m_subscriptions.clear();
}

// 목적: Preview presentation lifecycle event를 공통 Activity lifecycle로 투영
// 입력: event: initial active state, accepted transition 또는 exact terminal
// 출력: Activity active/terminal event queue 등록 가능
void QtActivityAdapter::handlePreviewPresentationEvent(const core::client::PreviewPresentationEvent& event)
{
    if (m_shuttingDown)
    {
        return;
    }
    if ((event.initial || event.requestStarted) && event.snapshot.activeRequestId.has_value())
    {
        recordStarted({{core::client::ActivityKind::Preview, event.snapshot.activeRequestId->value},
                       true,
                       event.snapshot.selectedPhotoId});
    }
    if (!event.terminal.has_value() || !event.terminal->requestId.has_value())
    {
        return;
    }

    core::client::ActivityTerminalState state = core::client::ActivityTerminalState::Completed;
    switch (event.terminal->state)
    {
    case core::client::PreviewTerminalState::Completed:
        break;
    case core::client::PreviewTerminalState::Failed:
        state = core::client::ActivityTerminalState::Failed;
        break;
    case core::client::PreviewTerminalState::Cancelled:
        state = core::client::ActivityTerminalState::Cancelled;
        break;
    }
    recordTerminal(
        {{core::client::ActivityKind::Preview, event.terminal->requestId->value}, state, event.terminal->error});
}

// 목적: Source Resolution lifecycle event를 공통 Activity lifecycle로 투영
// 입력: event: initial active requests, accepted transition 또는 exact terminal
// 출력: Activity active/terminal event queue 등록 가능
void QtActivityAdapter::handleSourceResolutionEvent(const core::client::SourceResolutionEvent& event)
{
    if (m_shuttingDown)
    {
        return;
    }
    if (event.initial)
    {
        for (const core::client::SourceRequestReceipt& request : event.snapshot.activeRequests)
        {
            recordStarted({{core::client::ActivityKind::SourceVerification, request.id.value}, true, request.photoId});
        }
    }
    else if (event.accepted.has_value())
    {
        recordStarted({{core::client::ActivityKind::SourceVerification, event.accepted->id.value},
                       true,
                       event.accepted->photoId});
    }
    if (!event.terminal.has_value())
    {
        return;
    }

    core::client::ActivityTerminalState state = core::client::ActivityTerminalState::Completed;
    switch (event.terminal->state)
    {
    case core::client::SourceResolutionTerminalState::Completed:
        break;
    case core::client::SourceResolutionTerminalState::Failed:
        state = core::client::ActivityTerminalState::Failed;
        break;
    case core::client::SourceResolutionTerminalState::Cancelled:
        state = core::client::ActivityTerminalState::Cancelled;
        break;
    }
    recordTerminal({{core::client::ActivityKind::SourceVerification, event.terminal->receipt.id.value},
                    state,
                    event.terminal->error});
}

// 목적: initial active 목록과 이후 lifecycle event를 Qt delivery context에서 구독
// 입력: callback: immutable Activity event consumer
// 출력: RAII unsubscribe handle 또는 callback·thread 오류
core::client::ActivitySubscriptionResult QtActivityAdapter::subscribeToActivities(
    core::client::ActivityCallback callback)
{
    if (!callback)
    {
        return core::client::ActivitySubscriptionResult::failure(
            {core::client::ClientErrorCode::InvalidArgument, "Activity callback must not be empty."});
    }
    if (QThread::currentThread() != thread())
    {
        return core::client::ActivitySubscriptionResult::failure(
            {core::client::ClientErrorCode::Conflict,
             "Activity subscription must be created on its adapter delivery context."});
    }
    if (m_shuttingDown)
    {
        return core::client::ActivitySubscriptionResult::failure(
            {core::client::ClientErrorCode::Conflict, "Activity adapter is shutting down."});
    }

    const SubscriptionStatePtr state = std::make_shared<SubscriptionState>();
    state->callback = std::move(callback);
    core::client::ActivitySubscriptionHandle handle = std::make_shared<Subscription>(state);
    std::erase_if(m_subscriptions, [](const std::weak_ptr<SubscriptionState>& weakState) {
        const SubscriptionStatePtr existingState = weakState.lock();
        return existingState == nullptr || !existingState->isActive();
    });
    m_subscriptions.emplace_back(state);
    core::client::ActivityEvent initialEvent{
        nextEventSequence(),
        true,
        m_activeActivities,
        std::nullopt,
    };
    if (!enqueueEvent(state, std::move(initialEvent)))
    {
        state->unsubscribe();
        return core::client::ActivitySubscriptionResult::failure(
            {core::client::ClientErrorCode::Unknown, "Unable to queue the initial Activity event."});
    }
    return core::client::ActivitySubscriptionResult::success(std::move(handle));
}

// 목적: cancellable Activity identity를 실제 Preview 또는 Source owner에 전달
// 입력: activityId: kind와 owner request identity
// 출력: owner가 cancellation을 수락한 identity 또는 stale·unsupported 오류
core::client::ActivityCancelResult QtActivityAdapter::cancelActivity(core::client::ActivityId activityId)
{
    if (QThread::currentThread() != thread())
    {
        return core::client::ActivityCancelResult::failure(
            {core::client::ClientErrorCode::Conflict, "Activity cancellation must run on its adapter context."});
    }
    const auto activity =
        std::find_if(m_activeActivities.cbegin(),
                     m_activeActivities.cend(),
                     [activityId](const core::client::ActiveActivity& active) { return active.id == activityId; });
    if (activityId.value == 0 || activity == m_activeActivities.cend())
    {
        return core::client::ActivityCancelResult::failure(
            {core::client::ClientErrorCode::NotFound, "Activity is no longer active."});
    }
    if (!activity->canCancel)
    {
        return core::client::ActivityCancelResult::failure(
            {core::client::ClientErrorCode::Conflict, "Activity owner does not support cancellation."});
    }

    bool cancelled = false;
    switch (activityId.kind)
    {
    case core::client::ActivityKind::Preview:
        cancelled = m_previewClient->cancelPreviewRequest({activityId.value}).hasValue();
        break;
    case core::client::ActivityKind::SourceVerification:
        cancelled = m_sourceResolutionClient->cancelSourceRequest({activityId.value}).hasValue();
        break;
    case core::client::ActivityKind::FolderScan:
    case core::client::ActivityKind::FolderImport:
        break;
    }
    return cancelled ? core::client::ActivityCancelResult::success(activityId)
                     : core::client::ActivityCancelResult::failure(
                           {core::client::ClientErrorCode::NotFound, "Activity owner rejected stale cancellation."});
}

// 목적: 0을 사용하지 않는 adapter-local event ordering sequence 발급
// 입력: 없음
// 출력: 다음 Activity event sequence
core::client::ActivityEventSequence QtActivityAdapter::nextEventSequence() noexcept
{
    const std::uint64_t value = m_nextEventSequence;
    m_nextEventSequence = value == std::numeric_limits<std::uint64_t>::max() ? 1 : value + 1;
    return {value};
}

// 목적: Folder operation lifecycle event를 공통 Activity lifecycle로 투영
// 입력: event: initial active state 또는 exact terminal
// 출력: Activity active/terminal event queue 등록 가능
void QtActivityAdapter::handleFolderOperationEvent(const core::client::FolderOperationEvent& event)
{
    if (event.activeOperation.has_value())
    {
        recordStarted(
            {{toActivityKind(event.activeOperation->kind), event.activeOperation->id.value}, false, std::nullopt});
    }
    if (!event.terminal.has_value())
    {
        return;
    }

    recordTerminal({{toActivityKind(event.terminal->receipt.kind), event.terminal->receipt.id.value},
                    toActivityTerminalState(event.terminal->state),
                    event.terminal->error});
}

// 목적: accepted owner request를 active Activity 목록에 추가하고 snapshot publish
// 입력: activity: kind, owner identity, cancellation capability와 optional Photo identity
// 출력: active snapshot event queue 등록
void QtActivityAdapter::recordStarted(core::client::ActiveActivity activity)
{
    if (m_shuttingDown || activity.id.value == 0)
    {
        return;
    }
    const auto existing = std::find_if(m_activeActivities.begin(),
                                       m_activeActivities.end(),
                                       [&activity](const auto& active) { return active.id == activity.id; });
    if (existing == m_activeActivities.end())
    {
        m_activeActivities.push_back(std::move(activity));
    }
    else
    {
        *existing = std::move(activity);
    }
    publishEvent(std::nullopt);
}

// 목적: owner terminal 결과를 active 목록에서 제거하고 terminal snapshot publish
// 입력: terminal: completed·failed·cancelled 결과와 optional error
// 출력: terminal event를 생략하지 않는 snapshot queue 등록
void QtActivityAdapter::recordTerminal(core::client::ActivityTerminal terminal)
{
    if (m_shuttingDown || terminal.id.value == 0)
    {
        return;
    }
    std::erase_if(m_activeActivities,
                  [&terminal](const core::client::ActiveActivity& activity) { return activity.id == terminal.id; });
    publishEvent(std::move(terminal));
}

// 목적: 현재 active 목록과 optional terminal 결과를 모든 active subscription에 fan-out
// 입력: terminal: 이번 transition의 optional terminal 결과
// 출력: subscription별 Qt queued callback 등록
void QtActivityAdapter::publishEvent(std::optional<core::client::ActivityTerminal> terminal)
{
    const core::client::ActivityEvent event{
        nextEventSequence(),
        false,
        m_activeActivities,
        std::move(terminal),
    };
    auto subscription = m_subscriptions.begin();
    while (subscription != m_subscriptions.end())
    {
        const SubscriptionStatePtr state = subscription->lock();
        if (state == nullptr || !state->isActive())
        {
            subscription = m_subscriptions.erase(subscription);
            continue;
        }
        if (!enqueueEvent(state, event))
        {
            state->unsubscribe();
        }
        ++subscription;
    }
}

// 목적: 한 subscription에 immutable Activity event를 Qt queue로 등록
// 입력: state: subscription lifetime, event: 전달할 snapshot event
// 출력: 전달 불필요 또는 queue 성공이면 true
bool QtActivityAdapter::enqueueEvent(const SubscriptionStatePtr& state, core::client::ActivityEvent event)
{
    if (!state->isActive())
    {
        return true;
    }
    return QMetaObject::invokeMethod(
        this, [state, event = std::move(event)] { deliverEvent(state, event); }, Qt::QueuedConnection);
}

// 목적: queued immutable event를 active callback 하나에 예외 격리하여 전달
// 입력: state: subscription lifetime, event: 전달할 Activity event
// 출력: 없음
void QtActivityAdapter::deliverEvent(const SubscriptionStatePtr& state,
                                     const core::client::ActivityEvent& event) noexcept
{
    try
    {
        const std::scoped_lock lock(state->mutex);
        if (!state->active)
        {
            return;
        }
        const core::client::ActivityCallback callback = state->callback;
        callback(event);
    }
    catch (...)
    {
        // Consumer 예외는 Qt event loop와 다른 subscription delivery로 전파하지 않는다.
    }
}

}  // namespace flexraw::ui::mainwindow
