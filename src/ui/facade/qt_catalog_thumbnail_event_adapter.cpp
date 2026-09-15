#include "qt_catalog_thumbnail_event_adapter.h"

#include <algorithm>
#include <limits>
#include <mutex>
#include <utility>

#include <QMetaObject>
#include <QThread>

#include "catalog_thumbnail_contracts.h"
#include "catalog_thumbnail_orchestrator.h"
#include "client_error_projection.h"
#include "display_frame_qt_adapter.h"

namespace flexraw::ui::facade
{

struct QtCatalogThumbnailEventAdapter::SubscriptionState
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
    core::client::CatalogThumbnailCallback callback;
};

class QtCatalogThumbnailEventAdapter::Subscription final : public core::client::ICatalogThumbnailSubscription
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

    // 목적: queued event와 이후 Catalog thumbnail callback 전달을 idempotent하게 차단
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

// 목적: Catalog thumbnail owner lifecycle을 Qt-free snapshot/event로 변환
// 입력: orchestrator: authoritative window/generation owner, parent: Qt lifetime owner
// 출력: 현재 Qt delivery context에 bound된 adapter
QtCatalogThumbnailEventAdapter::QtCatalogThumbnailEventAdapter(
    core::orchestration::CatalogThumbnailOrchestrator& orchestrator, QObject* parent)
    : QObject(parent), m_orchestrator(&orchestrator), m_snapshot(orchestrator.thumbnailWindowSnapshot())
{
    Q_ASSERT(m_orchestrator->thread() == thread());
    connect(m_orchestrator,
            &core::orchestration::CatalogThumbnailOrchestrator::thumbnailWindowStarted,
            this,
            &QtCatalogThumbnailEventAdapter::recordStarted,
            Qt::DirectConnection);
    connect(m_orchestrator,
            &core::orchestration::CatalogThumbnailOrchestrator::thumbnailReady,
            this,
            &QtCatalogThumbnailEventAdapter::recordFrame,
            Qt::DirectConnection);
    connect(m_orchestrator,
            &core::orchestration::CatalogThumbnailOrchestrator::thumbnailFailed,
            this,
            &QtCatalogThumbnailEventAdapter::recordIssue,
            Qt::DirectConnection);
    connect(m_orchestrator,
            &core::orchestration::CatalogThumbnailOrchestrator::thumbnailWindowTerminal,
            this,
            &QtCatalogThumbnailEventAdapter::recordTerminal,
            Qt::DirectConnection);
}

// 목적: queued callback을 차단하고 outliving subscription handle을 inactive 상태로 전환
// 입력: 없음
// 출력: adapter destruction 이후 callback 없음
QtCatalogThumbnailEventAdapter::~QtCatalogThumbnailEventAdapter()
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

// 목적: Qt GUI delivery context에서 initial thumbnail snapshot과 이후 lifecycle 구독
// 입력: callback: immutable frame·issue·terminal consumer
// 출력: RAII unsubscribe handle 또는 callback·thread 오류
core::client::CatalogThumbnailSubscriptionResult QtCatalogThumbnailEventAdapter::subscribeToCatalogThumbnails(
    core::client::CatalogThumbnailCallback callback)
{
    if (!callback)
    {
        return core::client::CatalogThumbnailSubscriptionResult::failure(
            {core::client::ClientErrorCode::InvalidArgument, "Catalog thumbnail callback must not be empty."});
    }
    if (QThread::currentThread() != thread())
    {
        return core::client::CatalogThumbnailSubscriptionResult::failure(
            {core::client::ClientErrorCode::Conflict,
             "Catalog thumbnail subscription must be created on its adapter delivery context."});
    }
    if (m_shuttingDown)
    {
        return core::client::CatalogThumbnailSubscriptionResult::failure(
            {core::client::ClientErrorCode::Conflict, "Catalog thumbnail event adapter is shutting down."});
    }

    const SubscriptionStatePtr state = std::make_shared<SubscriptionState>();
    state->callback = std::move(callback);
    core::client::CatalogThumbnailSubscriptionHandle handle = std::make_shared<Subscription>(state);
    std::erase_if(m_subscriptions, [](const std::weak_ptr<SubscriptionState>& weakState) {
        const SubscriptionStatePtr existing = weakState.lock();
        return existing == nullptr || !existing->isActive();
    });
    m_subscriptions.emplace_back(state);
    const core::client::CatalogThumbnailEvent initialEvent{
        nextEventSequence(), true, m_snapshot, false, std::nullopt, std::nullopt, std::nullopt};
    if (!enqueueEvent(state, initialEvent))
    {
        state->unsubscribe();
        return core::client::CatalogThumbnailSubscriptionResult::failure(
            {core::client::ClientErrorCode::Unknown, "Unable to queue the initial Catalog thumbnail event."});
    }
    return core::client::CatalogThumbnailSubscriptionResult::success(std::move(handle));
}

// 목적: adapter lifetime에서 0을 사용하지 않는 단조 event sequence 발급
// 입력: 없음
// 출력: 다음 Catalog thumbnail event sequence
core::client::CatalogThumbnailEventSequence QtCatalogThumbnailEventAdapter::nextEventSequence() noexcept
{
    const std::uint64_t value = m_nextEventSequence;
    m_nextEventSequence = value == std::numeric_limits<std::uint64_t>::max() ? 1 : value + 1;
    return {value};
}

// 목적: owner가 accepted한 thumbnail window를 current snapshot과 started event로 투영
// 입력: started: generation, accepted item 수와 target extent
// 출력: 최신 snapshot을 가진 queued event
void QtCatalogThumbnailEventAdapter::recordStarted(const core::orchestration::CatalogThumbnailWindowStarted& started)
{
    m_snapshot = {started.receipt.generation, started.targetExtent, started.receipt.acceptedItemCount, 0};
    publishEvent(true, std::nullopt, std::nullopt, std::nullopt);
}

// 목적: owner가 stale filtering한 QImage frame을 immutable DisplayFrame event로 투영
// 입력: frame: generation, tagged identity와 decoded image
// 출력: frame 또는 projection issue event
void QtCatalogThumbnailEventAdapter::recordFrame(const core::orchestration::CatalogThumbnailFrame& frame)
{
    m_snapshot = m_orchestrator->thumbnailWindowSnapshot();
    std::optional<core::client::DisplayFrame> displayFrame = toDisplayFrame(frame.image, frame.generation.value);
    if (!displayFrame.has_value())
    {
        publishEvent(
            false,
            std::nullopt,
            core::client::CatalogThumbnailIssue{frame.generation,
                                                frame.identity,
                                                {core::client::ClientErrorCode::DecodeFailed,
                                                 "Unable to project the Catalog thumbnail image into DisplayFrame."}},
            std::nullopt);
        return;
    }
    publishEvent(
        false,
        core::client::CatalogThumbnailFrameSnapshot{frame.generation, frame.identity, std::move(*displayFrame)},
        std::nullopt,
        std::nullopt);
}

// 목적: item decode 실패를 공통 ClientError issue로 투영
// 입력: issue: generation, tagged identity와 CoreError
// 출력: immutable issue event
void QtCatalogThumbnailEventAdapter::recordIssue(const core::orchestration::CatalogThumbnailIssue& issue)
{
    m_snapshot = m_orchestrator->thumbnailWindowSnapshot();
    publishEvent(false,
                 std::nullopt,
                 core::client::CatalogThumbnailIssue{
                     issue.generation, issue.identity, core::orchestration::toClientError(issue.error)},
                 std::nullopt);
}

// 목적: owner exact terminal을 Qt-free terminal state로 투영
// 입력: terminal: generation과 optional technical error
// 출력: active generation이 제거된 terminal event
void QtCatalogThumbnailEventAdapter::recordTerminal(const core::orchestration::CatalogThumbnailWindowTerminal& terminal)
{
    m_snapshot = m_orchestrator->thumbnailWindowSnapshot();
    std::optional<core::client::ClientError> error;
    if (terminal.error.has_value())
    {
        error = core::orchestration::toClientError(*terminal.error);
    }
    publishEvent(false,
                 std::nullopt,
                 std::nullopt,
                 core::client::CatalogThumbnailTerminal{terminal.generation, terminal.state, std::move(error)});
}

// 목적: current snapshot과 transition payload를 모든 active subscription에 fan-out
// 입력: windowStarted/frame/issue/terminal: 이번 transition 종류와 payload
// 출력: subscription별 Qt queued callback 등록
void QtCatalogThumbnailEventAdapter::publishEvent(bool windowStarted,
                                                  std::optional<core::client::CatalogThumbnailFrameSnapshot> frame,
                                                  std::optional<core::client::CatalogThumbnailIssue> issue,
                                                  std::optional<core::client::CatalogThumbnailTerminal> terminal)
{
    if (m_shuttingDown)
    {
        return;
    }
    const core::client::CatalogThumbnailEvent event{
        nextEventSequence(), false, m_snapshot, windowStarted, std::move(frame), std::move(issue), std::move(terminal)};
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

// 목적: 한 subscription에 immutable event를 Qt queued callback으로 등록
// 입력: state: subscription lifetime, event: 전달할 Catalog thumbnail event
// 출력: 전달 불필요 또는 queue 성공이면 true
bool QtCatalogThumbnailEventAdapter::enqueueEvent(const SubscriptionStatePtr& state,
                                                  core::client::CatalogThumbnailEvent event)
{
    if (!state->isActive())
    {
        return true;
    }
    return QMetaObject::invokeMethod(
        this, [state, event = std::move(event)] { deliverEvent(state, event); }, Qt::QueuedConnection);
}

// 목적: queued immutable event를 active callback 하나에 예외 격리하여 전달
// 입력: state: subscription lifetime, event: 전달할 Catalog thumbnail event
// 출력: 없음
void QtCatalogThumbnailEventAdapter::deliverEvent(const SubscriptionStatePtr& state,
                                                  const core::client::CatalogThumbnailEvent& event) noexcept
{
    try
    {
        const std::scoped_lock lock(state->mutex);
        if (!state->active)
        {
            return;
        }
        const core::client::CatalogThumbnailCallback callback = state->callback;
        callback(event);
    }
    catch (...)
    {
        // Consumer 예외는 Qt event loop와 다른 subscription delivery로 전파하지 않는다.
    }
}

}  // namespace flexraw::ui::facade
