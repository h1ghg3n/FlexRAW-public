#include "qt_folder_import_event_adapter.h"

#include <algorithm>
#include <limits>
#include <mutex>
#include <utility>

#include <QMetaObject>
#include <QThread>

#include "catalog_orchestrator.h"

namespace flexraw::ui::facade
{

struct QtFolderImportEventAdapter::SubscriptionState
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
    core::client::FolderOperationCallback callback;
};

class QtFolderImportEventAdapter::Subscription final : public core::client::IFolderOperationSubscription
{
public:
    // 목적: public handle과 shared Folder operation delivery state 연결
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

    // 목적: queued event와 이후 Folder operation callback 전달을 idempotent하게 차단
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

// 목적: Catalog-owned Folder lifecycle을 현재 Qt thread의 serialized client event로 변환
// 입력: catalogOrchestrator: authoritative Folder operation owner, parent: Qt lifetime owner
// 출력: Qt delivery context에 bound된 event adapter
QtFolderImportEventAdapter::QtFolderImportEventAdapter(core::orchestration::CatalogOrchestrator& catalogOrchestrator,
                                                       QObject* parent)
    : QObject(parent),
      m_catalogOrchestrator(&catalogOrchestrator),
      m_activeOperation(catalogOrchestrator.activeFolderOperation())
{
    Q_ASSERT(m_catalogOrchestrator->thread() == thread());
    connect(m_catalogOrchestrator,
            &core::orchestration::CatalogOrchestrator::folderOperationStarted,
            this,
            &QtFolderImportEventAdapter::recordStarted,
            Qt::DirectConnection);
    connect(m_catalogOrchestrator,
            &core::orchestration::CatalogOrchestrator::folderOperationTerminal,
            this,
            &QtFolderImportEventAdapter::recordTerminal,
            Qt::DirectConnection);
}

// 목적: queued callback을 차단하고 outliving subscription handle을 inactive 상태로 전환
// 입력: 없음
// 출력: adapter destruction 이후 callback 없음
QtFolderImportEventAdapter::~QtFolderImportEventAdapter()
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

// 목적: initial active Folder operation과 이후 lifecycle event를 Qt delivery context에서 구독
// 입력: callback: immutable Folder operation event consumer
// 출력: RAII unsubscribe handle 또는 callback·thread 오류
core::client::FolderOperationSubscriptionResult QtFolderImportEventAdapter::subscribeToFolderOperations(
    core::client::FolderOperationCallback callback)
{
    if (!callback)
    {
        return core::client::FolderOperationSubscriptionResult::failure(
            {core::client::ClientErrorCode::InvalidArgument, "Folder operation callback must not be empty."});
    }
    if (QThread::currentThread() != thread())
    {
        return core::client::FolderOperationSubscriptionResult::failure(
            {core::client::ClientErrorCode::Conflict,
             "Folder operation subscription must be created on its adapter delivery context."});
    }
    if (m_shuttingDown)
    {
        return core::client::FolderOperationSubscriptionResult::failure(
            {core::client::ClientErrorCode::Conflict, "Folder operation event adapter is shutting down."});
    }

    const SubscriptionStatePtr state = std::make_shared<SubscriptionState>();
    state->callback = std::move(callback);
    core::client::FolderOperationSubscriptionHandle handle = std::make_shared<Subscription>(state);
    std::erase_if(m_subscriptions, [](const std::weak_ptr<SubscriptionState>& weakState) {
        const SubscriptionStatePtr existingState = weakState.lock();
        return existingState == nullptr || !existingState->isActive();
    });
    m_subscriptions.emplace_back(state);
    core::client::FolderOperationEvent initialEvent{
        nextEventSequence(),
        true,
        m_activeOperation,
        std::nullopt,
    };
    if (!enqueueEvent(state, std::move(initialEvent)))
    {
        state->unsubscribe();
        return core::client::FolderOperationSubscriptionResult::failure(
            {core::client::ClientErrorCode::Unknown, "Unable to queue the initial Folder operation event."});
    }
    return core::client::FolderOperationSubscriptionResult::success(std::move(handle));
}

// 목적: adapter lifetime에서 0을 사용하지 않는 event ordering sequence 발급
// 입력: 없음
// 출력: 다음 Folder operation event sequence
core::client::FolderOperationEventSequence QtFolderImportEventAdapter::nextEventSequence() noexcept
{
    const std::uint64_t value = m_nextEventSequence;
    m_nextEventSequence = value == std::numeric_limits<std::uint64_t>::max() ? 1 : value + 1;
    return {value};
}

// 목적: accepted Folder operation을 current active state로 기록하고 publish
// 입력: receipt: owner가 발급한 operation identity와 kind
// 출력: active snapshot event queue 등록
void QtFolderImportEventAdapter::recordStarted(const core::client::FolderOperationReceipt& receipt)
{
    if (m_shuttingDown || receipt.id.value == 0)
    {
        return;
    }
    m_activeOperation = receipt;
    publishEvent(std::nullopt);
}

// 목적: owner terminal을 기록하고 current active state에서 제거한 뒤 publish
// 입력: terminal: completed·failed·cancelled exact terminal
// 출력: terminal event queue 등록
void QtFolderImportEventAdapter::recordTerminal(const core::client::FolderOperationTerminal& terminal)
{
    if (m_shuttingDown || terminal.receipt.id.value == 0)
    {
        return;
    }
    if (m_activeOperation.has_value() && m_activeOperation->id == terminal.receipt.id)
    {
        m_activeOperation.reset();
    }
    publishEvent(terminal);
}

// 목적: 현재 active operation과 optional terminal을 모든 active subscription에 fan-out
// 입력: terminal: 이번 transition의 optional terminal
// 출력: subscription별 Qt queued callback 등록
void QtFolderImportEventAdapter::publishEvent(std::optional<core::client::FolderOperationTerminal> terminal)
{
    const core::client::FolderOperationEvent event{
        nextEventSequence(),
        false,
        m_activeOperation,
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

// 목적: 한 subscription에 immutable Folder operation event를 Qt queue로 등록
// 입력: state: subscription lifetime, event: 전달할 snapshot event
// 출력: 전달 불필요 또는 queue 성공이면 true
bool QtFolderImportEventAdapter::enqueueEvent(const SubscriptionStatePtr& state,
                                              core::client::FolderOperationEvent event)
{
    if (!state->isActive())
    {
        return true;
    }
    return QMetaObject::invokeMethod(
        this, [state, event = std::move(event)] { deliverEvent(state, event); }, Qt::QueuedConnection);
}

// 목적: queued immutable event를 active callback 하나에 예외 격리하여 전달
// 입력: state: subscription lifetime, event: 전달할 Folder operation event
// 출력: 없음
void QtFolderImportEventAdapter::deliverEvent(const SubscriptionStatePtr& state,
                                              const core::client::FolderOperationEvent& event) noexcept
{
    try
    {
        const std::scoped_lock lock(state->mutex);
        if (!state->active)
        {
            return;
        }
        const core::client::FolderOperationCallback callback = state->callback;
        callback(event);
    }
    catch (...)
    {
        // Consumer 예외는 Qt event loop와 다른 subscription delivery로 전파하지 않는다.
    }
}

}  // namespace flexraw::ui::facade
