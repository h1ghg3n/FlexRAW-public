#include "qt_source_resolution_event_source.h"

#include <algorithm>
#include <limits>
#include <mutex>
#include <utility>

#include <QByteArray>
#include <QMetaObject>
#include <QThread>

#include "catalog_entry.h"
#include "catalog_orchestrator.h"
#include "client_error_projection.h"

namespace flexraw::core::orchestration
{
namespace
{

// 목적: Qt string을 byte length가 보존된 UTF-8 client string으로 변환
// 입력: value: Catalog record text
// 출력: Qt-free UTF-8 string
[[nodiscard]] std::string toClientString(const QString& value)
{
    const QByteArray utf8 = value.toUtf8();
    return {utf8.constData(), static_cast<std::size_t>(utf8.size())};
}

// 목적: domain file kind를 Qt-free Catalog photo kind로 투영
// 입력: kind: persisted photo file 분류
// 출력: 같은 의미의 client enum
[[nodiscard]] core::client::CatalogFileKind toClientFileKind(core::types::SupportedFileKind kind) noexcept
{
    switch (kind)
    {
    case core::types::SupportedFileKind::Raw:
        return core::client::CatalogFileKind::Raw;
    case core::types::SupportedFileKind::RasterImage:
        return core::client::CatalogFileKind::RasterImage;
    case core::types::SupportedFileKind::Unknown:
        return core::client::CatalogFileKind::Unknown;
    }
    return core::client::CatalogFileKind::Unknown;
}

// 목적: domain scan status를 Qt-free Catalog photo status로 투영
// 입력: status: persisted photo scan 상태
// 출력: 같은 의미의 client enum
[[nodiscard]] core::client::CatalogScanStatus toClientScanStatus(core::types::FileScanStatus status) noexcept
{
    switch (status)
    {
    case core::types::FileScanStatus::Pending:
        return core::client::CatalogScanStatus::Pending;
    case core::types::FileScanStatus::Ready:
        return core::client::CatalogScanStatus::Ready;
    case core::types::FileScanStatus::Unsupported:
        return core::client::CatalogScanStatus::Unsupported;
    case core::types::FileScanStatus::Failed:
        return core::client::CatalogScanStatus::Failed;
    }
    return core::client::CatalogScanStatus::Pending;
}

// 목적: source binding state를 Qt-free Catalog source state로 투영
// 입력: state: persisted source identity 상태
// 출력: 같은 의미의 client enum
[[nodiscard]] core::client::CatalogSourceState toClientSourceState(core::catalog::SourceBindingState state) noexcept
{
    switch (state)
    {
    case core::catalog::SourceBindingState::FingerprintPending:
        return core::client::CatalogSourceState::FingerprintPending;
    case core::catalog::SourceBindingState::Available:
        return core::client::CatalogSourceState::Available;
    case core::catalog::SourceBindingState::Missing:
        return core::client::CatalogSourceState::Missing;
    case core::catalog::SourceBindingState::VerificationRequired:
        return core::client::CatalogSourceState::VerificationRequired;
    case core::catalog::SourceBindingState::IdentityUnverified:
        return core::client::CatalogSourceState::IdentityUnverified;
    case core::catalog::SourceBindingState::ReplacementDetected:
        return core::client::CatalogSourceState::ReplacementDetected;
    case core::catalog::SourceBindingState::Unreadable:
        return core::client::CatalogSourceState::Unreadable;
    case core::catalog::SourceBindingState::Unlinked:
        return core::client::CatalogSourceState::Unlinked;
    }
    return core::client::CatalogSourceState::FingerprintPending;
}

// 목적: persisted source fingerprint를 Qt-free byte snapshot으로 투영
// 입력: fingerprint: metadata와 optional SHA-256 baseline
// 출력: fixed-width metadata와 independent byte vector
[[nodiscard]] core::client::CatalogSourceFingerprint toClientFingerprint(
    const core::types::SourceFingerprint& fingerprint)
{
    core::client::CatalogSourceFingerprint snapshot;
    snapshot.sizeBytes = fingerprint.sizeBytes;
    snapshot.modifiedAtMs = fingerprint.modifiedAtMs;
    snapshot.sha256.reserve(static_cast<std::size_t>(fingerprint.sha256.size()));
    for (const char byte : fingerprint.sha256)
    {
        snapshot.sha256.push_back(static_cast<std::uint8_t>(byte));
    }
    return snapshot;
}

// 목적: source transition의 Catalog photo record를 immutable Qt-free snapshot으로 투영
// 입력: photo: repository transition 이후 persisted record
// 출력: identity/source/fingerprint 상태를 보존한 client snapshot
[[nodiscard]] core::client::CatalogPhotoSnapshot toClientPhoto(const core::catalog::CatalogPhotoRecord& photo)
{
    core::client::CatalogPhotoSnapshot snapshot;
    snapshot.id = {photo.id.value};
    if (photo.source.has_value())
    {
        snapshot.sourcePath = toClientString(photo.source->path);
    }
    snapshot.lastKnownPath = toClientString(photo.lastKnownPath);
    snapshot.extension = toClientString(photo.extension);
    snapshot.displayName = toClientString(photo.displayName);
    snapshot.kind = toClientFileKind(photo.kind);
    snapshot.scanStatus = toClientScanStatus(photo.scanStatus);
    snapshot.fingerprint = toClientFingerprint(photo.fingerprint);
    snapshot.sourceState = toClientSourceState(photo.sourceState);
    return snapshot;
}

}  // namespace

struct QtSourceResolutionEventSource::SubscriptionState
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
    core::client::SourceResolutionCallback callback;
};

class QtSourceResolutionEventSource::Subscription final : public core::client::ISourceResolutionSubscription
{
public:
    // 목적: public handle과 shared Source Resolution delivery state 연결
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

    // 목적: queued event와 이후 Source Resolution callback 전달을 idempotent하게 차단
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

// 목적: Catalog-owned source fingerprint lifecycle을 Qt-free Source Resolution event로 변환
// 입력: catalogOrchestrator: authoritative request/mutation owner, parent: Qt lifetime owner
// 출력: 현재 Qt delivery context에 bound된 event adapter
QtSourceResolutionEventSource::QtSourceResolutionEventSource(CatalogOrchestrator& catalogOrchestrator, QObject* parent)
    : QObject(parent),
      m_catalogOrchestrator(&catalogOrchestrator),
      m_snapshot(catalogOrchestrator.sourceResolutionSnapshot())
{
    Q_ASSERT(m_catalogOrchestrator->thread() == thread());
    connect(m_catalogOrchestrator,
            &CatalogOrchestrator::sourceBindingStarted,
            this,
            &QtSourceResolutionEventSource::recordStarted,
            Qt::DirectConnection);
    connect(m_catalogOrchestrator,
            &CatalogOrchestrator::sourceBindingUpdated,
            this,
            &QtSourceResolutionEventSource::recordUpdated,
            Qt::DirectConnection);
    connect(m_catalogOrchestrator,
            &CatalogOrchestrator::sourceBindingFailed,
            this,
            &QtSourceResolutionEventSource::recordFailed,
            Qt::DirectConnection);
    connect(m_catalogOrchestrator,
            &CatalogOrchestrator::sourceBindingCancelled,
            this,
            &QtSourceResolutionEventSource::recordCancelled,
            Qt::DirectConnection);
}

// 목적: queued callback을 차단하고 outliving subscription handle을 inactive 상태로 전환
// 입력: 없음
// 출력: adapter destruction 이후 callback 없음
QtSourceResolutionEventSource::~QtSourceResolutionEventSource()
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

// 목적: initial active source requests와 이후 accepted·update·issue·terminal 구독
// 입력: callback: immutable Source Resolution event consumer
// 출력: RAII unsubscribe handle 또는 callback·thread 오류
core::client::SourceResolutionSubscriptionResult QtSourceResolutionEventSource::subscribeToSourceResolution(
    core::client::SourceResolutionCallback callback)
{
    if (!callback)
    {
        return core::client::SourceResolutionSubscriptionResult::failure(
            {core::client::ClientErrorCode::InvalidArgument, "Source Resolution callback must not be empty."});
    }
    if (QThread::currentThread() != thread())
    {
        return core::client::SourceResolutionSubscriptionResult::failure(
            {core::client::ClientErrorCode::Conflict,
             "Source Resolution subscription must be created on its adapter delivery context."});
    }
    if (m_shuttingDown)
    {
        return core::client::SourceResolutionSubscriptionResult::failure(
            {core::client::ClientErrorCode::Conflict, "Source Resolution event adapter is shutting down."});
    }

    const SubscriptionStatePtr state = std::make_shared<SubscriptionState>();
    state->callback = std::move(callback);
    core::client::SourceResolutionSubscriptionHandle handle = std::make_shared<Subscription>(state);
    std::erase_if(m_subscriptions, [](const std::weak_ptr<SubscriptionState>& weakState) {
        const SubscriptionStatePtr existing = weakState.lock();
        return existing == nullptr || !existing->isActive();
    });
    m_subscriptions.emplace_back(state);
    const core::client::SourceResolutionEvent initialEvent{
        nextEventSequence(), true, m_snapshot, std::nullopt, std::nullopt, std::nullopt, std::nullopt};
    if (!enqueueEvent(state, initialEvent))
    {
        state->unsubscribe();
        return core::client::SourceResolutionSubscriptionResult::failure(
            {core::client::ClientErrorCode::Unknown, "Unable to queue the initial Source Resolution event."});
    }
    return core::client::SourceResolutionSubscriptionResult::success(std::move(handle));
}

// 목적: adapter lifetime에서 0을 사용하지 않는 event ordering sequence 발급
// 입력: 없음
// 출력: 다음 Source Resolution event sequence
core::client::SourceResolutionEventSequence QtSourceResolutionEventSource::nextEventSequence() noexcept
{
    const std::uint64_t value = m_nextEventSequence;
    m_nextEventSequence = value == std::numeric_limits<std::uint64_t>::max() ? 1 : value + 1;
    return {value};
}

// 목적: owner accepted request를 current active snapshot과 accepted event로 투영
// 입력: requestId: owner identity, photoId: stable target identity
// 출력: request kind·locator가 포함된 queued event
void QtSourceResolutionEventSource::recordStarted(core::types::RequestId requestId, core::types::PhotoId photoId)
{
    m_snapshot = m_catalogOrchestrator->sourceResolutionSnapshot();
    const std::optional<core::client::SourceRequestReceipt> receipt = findActiveRequest(requestId);
    if (!receipt.has_value() || receipt->photoId.value != photoId.value)
    {
        return;
    }
    publishEvent(receipt, std::nullopt, std::nullopt, std::nullopt);
}

// 목적: repository source transition을 immutable photo update와 completed terminal로 투영
// 입력: update: request identity와 갱신된 기존·optional 신규 photo
// 출력: active request가 제거된 update/terminal event
void QtSourceResolutionEventSource::recordUpdated(const CatalogSourceUpdate& update)
{
    const std::optional<core::client::SourceRequestReceipt> receipt = findActiveRequest(update.requestId);
    m_snapshot = m_catalogOrchestrator->sourceResolutionSnapshot();
    if (!receipt.has_value() || receipt->photoId.value != update.photoId.value)
    {
        return;
    }

    core::client::SourceResolutionUpdate projected{*receipt, toClientPhoto(update.photo), std::nullopt};
    if (update.createdPhoto.has_value())
    {
        projected.createdPhoto = toClientPhoto(*update.createdPhoto);
    }
    const core::client::SourceResolutionTerminal terminal{
        *receipt, core::client::SourceResolutionTerminalState::Completed, std::nullopt};
    publishEvent(std::nullopt, std::move(projected), std::nullopt, terminal);
}

// 목적: fingerprint 또는 repository failure를 issue와 failed terminal로 투영
// 입력: issue: request·Photo identity와 technical error
// 출력: active request가 제거된 issue/terminal event
void QtSourceResolutionEventSource::recordFailed(const CatalogIssue& issue)
{
    const std::optional<core::client::SourceRequestReceipt> receipt = findActiveRequest(issue.requestId);
    m_snapshot = m_catalogOrchestrator->sourceResolutionSnapshot();
    if (!receipt.has_value() || receipt->photoId.value != issue.photoId.value)
    {
        return;
    }

    const core::client::ClientError error = core::orchestration::toClientError(issue.error);
    publishEvent(
        std::nullopt,
        std::nullopt,
        core::client::SourceResolutionIssue{*receipt, error},
        core::client::SourceResolutionTerminal{*receipt, core::client::SourceResolutionTerminalState::Failed, error});
}

// 목적: owner cancellation을 exact cancelled terminal로 투영
// 입력: requestId: 취소된 owner request identity
// 출력: active request가 제거된 terminal event
void QtSourceResolutionEventSource::recordCancelled(core::types::RequestId requestId)
{
    const std::optional<core::client::SourceRequestReceipt> receipt = findActiveRequest(requestId);
    m_snapshot = m_catalogOrchestrator->sourceResolutionSnapshot();
    if (!receipt.has_value())
    {
        return;
    }
    publishEvent(std::nullopt,
                 std::nullopt,
                 std::nullopt,
                 core::client::SourceResolutionTerminal{
                     *receipt, core::client::SourceResolutionTerminalState::Cancelled, std::nullopt});
}

// 목적: 이전 active snapshot에서 terminal request context 조회
// 입력: requestId: completed·failed·cancelled owner identity
// 출력: kind·Photo·locator receipt 또는 찾을 수 없으면 빈 값
std::optional<core::client::SourceRequestReceipt> QtSourceResolutionEventSource::findActiveRequest(
    core::types::RequestId requestId) const
{
    const auto receipt = std::find_if(
        m_snapshot.activeRequests.cbegin(),
        m_snapshot.activeRequests.cend(),
        [requestId](const core::client::SourceRequestReceipt& candidate) { return candidate.id.value == requestId; });
    return receipt == m_snapshot.activeRequests.cend() ? std::nullopt
                                                       : std::optional<core::client::SourceRequestReceipt>{*receipt};
}

// 목적: current snapshot과 transition payload를 모든 active subscription에 fan-out
// 입력: accepted/update/issue/terminal: 이번 source lifecycle transition payload
// 출력: subscription별 Qt queued callback 등록
void QtSourceResolutionEventSource::publishEvent(std::optional<core::client::SourceRequestReceipt> accepted,
                                                 std::optional<core::client::SourceResolutionUpdate> update,
                                                 std::optional<core::client::SourceResolutionIssue> issue,
                                                 std::optional<core::client::SourceResolutionTerminal> terminal)
{
    if (m_shuttingDown)
    {
        return;
    }
    const core::client::SourceResolutionEvent event{nextEventSequence(),
                                                    false,
                                                    m_snapshot,
                                                    std::move(accepted),
                                                    std::move(update),
                                                    std::move(issue),
                                                    std::move(terminal)};
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
// 입력: state: subscription lifetime, event: 전달할 Source Resolution event
// 출력: 전달 불필요 또는 queue 성공이면 true
bool QtSourceResolutionEventSource::enqueueEvent(const SubscriptionStatePtr& state,
                                                 core::client::SourceResolutionEvent event)
{
    if (!state->isActive())
    {
        return true;
    }
    return QMetaObject::invokeMethod(
        this, [state, event = std::move(event)] { deliverEvent(state, event); }, Qt::QueuedConnection);
}

// 목적: queued immutable event를 active callback 하나에 예외 격리하여 전달
// 입력: state: subscription lifetime, event: 전달할 Source Resolution event
// 출력: 없음
void QtSourceResolutionEventSource::deliverEvent(const SubscriptionStatePtr& state,
                                                 const core::client::SourceResolutionEvent& event) noexcept
{
    try
    {
        const std::scoped_lock lock(state->mutex);
        if (!state->active)
        {
            return;
        }
        const core::client::SourceResolutionCallback callback = state->callback;
        callback(event);
    }
    catch (...)
    {
        // Consumer 예외는 Qt event loop와 다른 subscription delivery로 전파하지 않는다.
    }
}

}  // namespace flexraw::core::orchestration
