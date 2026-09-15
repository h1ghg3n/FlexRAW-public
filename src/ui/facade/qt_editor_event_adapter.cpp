#include "qt_editor_event_adapter.h"

#include <limits>
#include <mutex>
#include <optional>
#include <utility>

#include <QMetaObject>
#include <QThread>

#include "editor_orchestrator.h"

namespace flexraw::ui::facade
{

struct QtEditorEventAdapter::SubscriptionState
{
    // 목적: queued event와 이후 callback 전달을 idempotent하게 차단
    // 입력: 없음
    // 출력: inactive state와 해제된 callback/pending event
    void unsubscribe() noexcept
    {
        const std::scoped_lock lock(mutex);
        active = false;
        callback = {};
        pendingAdjustment.reset();
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
    core::client::EditorStateCallback callback;
    std::optional<core::client::EditorStateEvent> pendingAdjustment;
    bool adjustmentDeliveryQueued{false};
};

class QtEditorEventAdapter::Subscription final : public core::client::IEditorStateSubscription
{
public:
    // 목적: public handle과 shared subscription delivery state 연결
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

    // 목적: queued event와 이후 Editor state callback 전달을 idempotent하게 차단
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

// 목적: Editor snapshot invalidation을 현재 Qt thread의 serialized client event로 변환
// 입력: editorOrchestrator: authoritative Editor session, parent: Qt lifetime owner
// 출력: Qt delivery context에 bound된 event adapter
QtEditorEventAdapter::QtEditorEventAdapter(core::orchestration::EditorOrchestrator& editorOrchestrator, QObject* parent)
    : QObject(parent), m_editorOrchestrator(&editorOrchestrator)
{
    Q_ASSERT(m_editorOrchestrator->thread() == thread());
    connect(
        m_editorOrchestrator,
        &core::orchestration::EditorOrchestrator::editorSnapshotChanged,
        this,
        [this] { publishCurrentSnapshot(); },
        Qt::DirectConnection);
}

// 목적: queued callback을 차단하고 outliving subscription handle을 inactive 상태로 전환
// 입력: 없음
// 출력: adapter destruction 이후 callback 없음
QtEditorEventAdapter::~QtEditorEventAdapter()
{
    m_shuttingDown = true;
    for (const std::weak_ptr<SubscriptionState>& weakState : m_subscriptions)
    {
        if (const SubscriptionStatePtr state = weakState.lock(); state != nullptr)
        {
            state->unsubscribe();
        }
    }
    m_subscriptions.clear();
}

// 목적: 현재 Qt delivery context에서 initial snapshot과 이후 Editor state event 구독
// 입력: callback: immutable event consumer
// 출력: RAII unsubscribe handle 또는 callback·thread 오류
core::client::EditorStateSubscriptionResult QtEditorEventAdapter::subscribeToEditorState(
    core::client::EditorStateCallback callback)
{
    if (!callback)
    {
        return core::client::EditorStateSubscriptionResult::failure(
            {core::client::ClientErrorCode::InvalidArgument, "Editor state callback must not be empty."});
    }
    if (QThread::currentThread() != thread())
    {
        return core::client::EditorStateSubscriptionResult::failure(
            {core::client::ClientErrorCode::Conflict,
             "Editor state subscription must be created on its adapter delivery context."});
    }
    if (m_shuttingDown)
    {
        return core::client::EditorStateSubscriptionResult::failure(
            {core::client::ClientErrorCode::Conflict, "Editor state event adapter is shutting down."});
    }

    const SubscriptionStatePtr state = std::make_shared<SubscriptionState>();
    state->callback = std::move(callback);
    core::client::EditorStateSubscriptionHandle handle = std::make_shared<Subscription>(state);
    std::erase_if(m_subscriptions, [](const std::weak_ptr<SubscriptionState>& weakState) {
        const SubscriptionStatePtr existingState = weakState.lock();
        return existingState == nullptr || !existingState->isActive();
    });
    m_subscriptions.emplace_back(state);
    core::client::EditorStateEvent initialEvent{
        nextEventSequence(),
        true,
        m_editorOrchestrator->editorSnapshot(),
    };
    if (!enqueueEvent(state, std::move(initialEvent)))
    {
        state->unsubscribe();
        return core::client::EditorStateSubscriptionResult::failure(
            {core::client::ClientErrorCode::Unknown, "Unable to queue the initial Editor state event."});
    }

    return core::client::EditorStateSubscriptionResult::success(std::move(handle));
}

// 목적: adapter lifetime에서 0을 사용하지 않는 단조 event sequence 발급
// 입력: 없음
// 출력: 다음 Editor event sequence
core::client::EditorEventSequence QtEditorEventAdapter::nextEventSequence() noexcept
{
    const std::uint64_t value = m_nextEventSequence;
    m_nextEventSequence = value == std::numeric_limits<std::uint64_t>::max() ? 1 : value + 1;
    return {value};
}

// 목적: 현재 Orchestrator snapshot을 capture해 active subscription에 비동기 fan-out
// 입력: 없음
// 출력: Adjustment intermediate coalescing 또는 ordered queue 등록
void QtEditorEventAdapter::publishCurrentSnapshot()
{
    if (m_shuttingDown)
    {
        return;
    }

    const core::client::EditorStateEvent event{
        nextEventSequence(),
        false,
        m_editorOrchestrator->editorSnapshot(),
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

// 목적: 한 subscription에 event를 Qt queued callback으로 등록
// 입력: state: subscription lifetime, event: immutable Editor state event
// 출력: 전달이 불필요하거나 queue 등록에 성공하면 true
bool QtEditorEventAdapter::enqueueEvent(const SubscriptionStatePtr& state, core::client::EditorStateEvent event)
{
    const bool coalesceAdjustment = !event.initial && event.snapshot.adjustmentActive;
    if (!coalesceAdjustment)
    {
        if (!state->isActive())
        {
            return true;
        }
        return QMetaObject::invokeMethod(
            this, [state, event = std::move(event)] { deliverEvent(state, event); }, Qt::QueuedConnection);
    }

    bool queueDelivery = false;
    {
        const std::scoped_lock lock(state->mutex);
        if (!state->active)
        {
            return true;
        }
        state->pendingAdjustment = std::move(event);
        if (!state->adjustmentDeliveryQueued)
        {
            state->adjustmentDeliveryQueued = true;
            queueDelivery = true;
        }
    }
    if (!queueDelivery)
    {
        return true;
    }

    const bool queued =
        QMetaObject::invokeMethod(this, [state] { deliverPendingAdjustment(state); }, Qt::QueuedConnection);
    if (!queued)
    {
        const std::scoped_lock lock(state->mutex);
        state->adjustmentDeliveryQueued = false;
        state->pendingAdjustment.reset();
    }
    return queued;
}

// 목적: queued immutable event를 active callback 하나에 예외 격리하여 전달
// 입력: state: subscription lifetime, event: 전달할 snapshot event
// 출력: 없음
void QtEditorEventAdapter::deliverEvent(const SubscriptionStatePtr& state,
                                        const core::client::EditorStateEvent& event) noexcept
{
    try
    {
        const std::scoped_lock lock(state->mutex);
        if (!state->active)
        {
            return;
        }
        const core::client::EditorStateCallback callback = state->callback;
        callback(event);
    }
    catch (...)
    {
        // Consumer 예외는 Qt event loop와 다른 subscription delivery로 전파하지 않는다.
    }
}

// 목적: 같은 Adjustment burst에서 마지막으로 coalescing된 intermediate event 전달
// 입력: state: pending event와 callback을 소유한 subscription
// 출력: 없음
void QtEditorEventAdapter::deliverPendingAdjustment(const SubscriptionStatePtr& state) noexcept
{
    std::optional<core::client::EditorStateEvent> event;
    {
        const std::scoped_lock lock(state->mutex);
        state->adjustmentDeliveryQueued = false;
        if (!state->active)
        {
            state->pendingAdjustment.reset();
            return;
        }
        event = std::move(state->pendingAdjustment);
        state->pendingAdjustment.reset();
    }
    if (event.has_value())
    {
        deliverEvent(state, *event);
    }
}

}  // namespace flexraw::ui::facade
