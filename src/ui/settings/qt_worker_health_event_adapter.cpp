#include "qt_worker_health_event_adapter.h"

#include <algorithm>
#include <limits>
#include <mutex>
#include <utility>

#include <QMetaObject>
#include <QThread>

#include "worker_health_orchestrator.h"

namespace flexraw::ui::settings
{

struct QtWorkerHealthEventAdapter::SubscriptionState
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
    core::client::WorkerHealthCallback callback;
};

class QtWorkerHealthEventAdapter::Subscription final : public core::client::IWorkerHealthSubscription
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

    // 목적: queued event와 이후 Worker health callback 전달을 idempotent하게 차단
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

// 목적: Worker health operation state를 현재 Qt thread의 serialized Product event로 변환
// 입력: orchestrator: authoritative probe/snapshot owner, parent: Qt lifetime owner
// 출력: Qt delivery context에 bound된 event adapter
QtWorkerHealthEventAdapter::QtWorkerHealthEventAdapter(core::orchestration::WorkerHealthOrchestrator& orchestrator,
                                                       QObject* const parent)
    : QObject(parent), m_orchestrator(&orchestrator)
{
    Q_ASSERT(m_orchestrator->thread() == thread());
    connect(
        m_orchestrator,
        &core::orchestration::WorkerHealthOrchestrator::workerHealthProbeAccepted,
        this,
        [this](const core::client::WorkerHealthProbeReceipt&) { publishEvent(std::nullopt); },
        Qt::DirectConnection);
    connect(
        m_orchestrator,
        &core::orchestration::WorkerHealthOrchestrator::workerHealthProbeCompleted,
        this,
        [this](const core::client::WorkerHealthProbeCompletion& completion) { publishEvent(completion); },
        Qt::DirectConnection);
}

// 목적: queued callback을 차단하고 outliving subscription handle을 inactive 상태로 전환
// 입력: 없음
// 출력: adapter destruction 이후 callback 없음
QtWorkerHealthEventAdapter::~QtWorkerHealthEventAdapter()
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

// 목적: initial active probe와 이후 accepted/terminal health event 구독
// 입력: callback: immutable Worker health event consumer
// 출력: RAII unsubscribe handle 또는 callback·thread 오류
core::client::WorkerHealthSubscriptionResult QtWorkerHealthEventAdapter::subscribeToWorkerHealth(
    core::client::WorkerHealthCallback callback)
{
    if (!callback)
    {
        return core::client::WorkerHealthSubscriptionResult::failure(
            {core::client::ClientErrorCode::InvalidArgument, "Worker health callback must not be empty."});
    }
    if (QThread::currentThread() != thread())
    {
        return core::client::WorkerHealthSubscriptionResult::failure(
            {core::client::ClientErrorCode::Conflict,
             "Worker health subscription must be created on its adapter delivery context."});
    }
    if (m_shuttingDown)
    {
        return core::client::WorkerHealthSubscriptionResult::failure(
            {core::client::ClientErrorCode::Conflict, "Worker health event adapter is shutting down."});
    }

    const SubscriptionStatePtr state = std::make_shared<SubscriptionState>();
    state->callback = std::move(callback);
    core::client::WorkerHealthSubscriptionHandle handle = std::make_shared<Subscription>(state);
    std::erase_if(m_subscriptions, [](const std::weak_ptr<SubscriptionState>& weakState) {
        const SubscriptionStatePtr existing = weakState.lock();
        return existing == nullptr || !existing->isActive();
    });
    m_subscriptions.emplace_back(state);
    core::client::WorkerHealthEvent initial{nextEventSequence(), true, m_orchestrator->activeProbe(), std::nullopt};
    if (!enqueueEvent(state, std::move(initial)))
    {
        state->unsubscribe();
        return core::client::WorkerHealthSubscriptionResult::failure(
            {core::client::ClientErrorCode::Unknown, "Unable to queue the initial Worker health event."});
    }
    return core::client::WorkerHealthSubscriptionResult::success(std::move(handle));
}

// 목적: adapter lifetime에서 0을 사용하지 않는 event ordering sequence 발급
// 입력: 없음
// 출력: 다음 Worker health event sequence
core::client::WorkerHealthEventSequence QtWorkerHealthEventAdapter::nextEventSequence() noexcept
{
    const std::uint64_t value = m_nextEventSequence;
    m_nextEventSequence = value == std::numeric_limits<std::uint64_t>::max() ? 1 : value + 1;
    return {value};
}

// 목적: 현재 active probe와 optional completion을 모든 active subscription에 fan-out
// 입력: completion: accepted probe의 optional terminal snapshot
// 출력: subscription별 Qt queued callback 등록
void QtWorkerHealthEventAdapter::publishEvent(std::optional<core::client::WorkerHealthProbeCompletion> completion)
{
    if (m_shuttingDown)
    {
        return;
    }
    core::client::WorkerHealthEvent event{
        nextEventSequence(), false, m_orchestrator->activeProbe(), std::move(completion)};
    std::erase_if(m_subscriptions, [](const std::weak_ptr<SubscriptionState>& weakState) {
        const SubscriptionStatePtr state = weakState.lock();
        return state == nullptr || !state->isActive();
    });
    for (const std::weak_ptr<SubscriptionState>& weakState : m_subscriptions)
    {
        if (const SubscriptionStatePtr state = weakState.lock(); state != nullptr)
        {
            static_cast<void>(enqueueEvent(state, event));
        }
    }
}

// 목적: 한 subscription에 immutable event를 Qt queued callback으로 등록
// 입력: state: subscription lifetime, event: 전달할 Worker health event
// 출력: 전달 불필요 또는 queue 성공이면 true
bool QtWorkerHealthEventAdapter::enqueueEvent(const SubscriptionStatePtr& state, core::client::WorkerHealthEvent event)
{
    if (!state->isActive())
    {
        return true;
    }
    return QMetaObject::invokeMethod(
        this, [state, event = std::move(event)]() { deliverEvent(state, event); }, Qt::QueuedConnection);
}

// 목적: queued immutable event를 active callback 하나에 예외 격리하여 전달
// 입력: state: subscription lifetime, event: 전달할 Worker health event
// 출력: 없음
void QtWorkerHealthEventAdapter::deliverEvent(const SubscriptionStatePtr& state,
                                              const core::client::WorkerHealthEvent& event) noexcept
{
    const std::scoped_lock lock(state->mutex);
    if (!state->active || !state->callback)
    {
        return;
    }
    try
    {
        state->callback(event);
    }
    catch (...)
    {}
}

}  // namespace flexraw::ui::settings
