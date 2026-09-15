#include "qt_preview_presentation_event_adapter.h"

#include <algorithm>
#include <limits>
#include <mutex>
#include <utility>

#include <QMetaObject>
#include <QSize>
#include <QThread>

#include "client_error_projection.h"
#include "clipping.h"
#include "display_frame_qt_adapter.h"
#include "editor_orchestrator.h"
#include "histogram.h"
#include "preview_contracts.h"

namespace flexraw::ui::facade
{
namespace
{

// 목적: internal Preview tier를 frontend-neutral presentation tier로 변환
// 입력: tier: thumbnail 또는 standard pipeline 단계
// 출력: 같은 의미의 client enum
[[nodiscard]] core::client::PreviewFrameTier toClientTier(core::orchestration::PreviewTier tier) noexcept
{
    return tier == core::orchestration::PreviewTier::Standard ? core::client::PreviewFrameTier::Standard
                                                              : core::client::PreviewFrameTier::Thumbnail;
}

// 목적: internal Preview render mode를 frontend-neutral presentation mode로 변환
// 입력: mode: interactive 또는 final render 의미
// 출력: 같은 의미의 client enum
[[nodiscard]] core::client::PreviewPresentationMode toClientMode(core::orchestration::PreviewRenderMode mode) noexcept
{
    return mode == core::orchestration::PreviewRenderMode::Interactive
               ? core::client::PreviewPresentationMode::Interactive
               : core::client::PreviewPresentationMode::Final;
}

// 목적: Qt histogram value를 fixed-width client snapshot으로 변환
// 입력: histogram: worker가 계산한 256-bin channel count
// 출력: ownership이 독립된 fixed-width histogram
[[nodiscard]] core::client::PreviewHistogramSnapshot toClientHistogram(const core::develop::ImageHistogram& histogram)
{
    core::client::PreviewHistogramSnapshot snapshot;
    std::copy(histogram.red.cbegin(), histogram.red.cend(), snapshot.red.begin());
    std::copy(histogram.green.cbegin(), histogram.green.cend(), snapshot.green.begin());
    std::copy(histogram.blue.cbegin(), histogram.blue.cend(), snapshot.blue.begin());
    std::copy(histogram.luminance.cbegin(), histogram.luminance.cend(), snapshot.luminance.begin());
    snapshot.pixelCount = histogram.pixelCount;
    return snapshot;
}

// 목적: Qt clipping value를 fixed-width client snapshot으로 변환
// 입력: clipping: shadow/highlight/전체 pixel count
// 출력: 같은 count를 가진 client clipping snapshot
[[nodiscard]] core::client::PreviewClippingSnapshot toClientClipping(
    const core::develop::ClippingSummary& clipping) noexcept
{
    return {clipping.shadowPixelCount, clipping.highlightPixelCount, clipping.pixelCount};
}

// 목적: internal Preview issue의 optional request identity를 client 값으로 변환
// 입력: requestId: accepted 전 failure이면 0
// 출력: nonzero request만 가진 optional identity
[[nodiscard]] std::optional<core::client::PreviewRequestId> toOptionalRequestId(std::uint64_t requestId)
{
    return requestId == 0 ? std::nullopt
                          : std::optional<core::client::PreviewRequestId>{core::client::PreviewRequestId{requestId}};
}

// 목적: internal photo identity의 optional client projection 생성
// 입력: photoId: 선택되지 않았으면 0
// 출력: 양수 PhotoId만 가진 optional identity
[[nodiscard]] std::optional<core::client::ClientPhotoId> toOptionalPhotoId(std::int64_t photoId)
{
    return photoId <= 0 ? std::nullopt
                        : std::optional<core::client::ClientPhotoId>{core::client::ClientPhotoId{photoId}};
}

}  // namespace

struct QtPreviewPresentationEventAdapter::SubscriptionState
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
    core::client::PreviewPresentationCallback callback;
};

class QtPreviewPresentationEventAdapter::Subscription final : public core::client::IPreviewPresentationSubscription
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

    // 목적: queued event와 이후 Preview presentation callback 전달을 idempotent하게 차단
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

// 목적: Qt-free histogram snapshot을 기존 Qt Develop panel 값으로 변환
// 입력: snapshot: fixed-width 256-bin RGB/luminance 분석
// 출력: 같은 channel count를 보존한 develop histogram
core::develop::ImageHistogram toDevelopHistogram(const core::client::PreviewHistogramSnapshot& snapshot)
{
    core::develop::ImageHistogram histogram;
    std::copy(snapshot.red.cbegin(), snapshot.red.cend(), histogram.red.begin());
    std::copy(snapshot.green.cbegin(), snapshot.green.cend(), histogram.green.begin());
    std::copy(snapshot.blue.cbegin(), snapshot.blue.cend(), histogram.blue.begin());
    std::copy(snapshot.luminance.cbegin(), snapshot.luminance.cend(), histogram.luminance.begin());
    histogram.pixelCount = snapshot.pixelCount;
    return histogram;
}

// 목적: Qt-free clipping snapshot을 기존 Qt Preview widget 값으로 변환
// 입력: snapshot: shadow/highlight/전체 pixel count
// 출력: 같은 count를 보존한 develop clipping summary
core::develop::ClippingSummary toDevelopClipping(const core::client::PreviewClippingSnapshot& snapshot)
{
    return {snapshot.shadowPixelCount, snapshot.highlightPixelCount, snapshot.pixelCount};
}

// 목적: Editor Preview lifecycle을 Qt-free presentation snapshot/event로 변환
// 입력: editorOrchestrator: authoritative Preview request owner, parent: Qt lifetime owner
// 출력: 현재 Qt delivery context에 bound된 adapter
QtPreviewPresentationEventAdapter::QtPreviewPresentationEventAdapter(
    core::orchestration::EditorOrchestrator& editorOrchestrator, QObject* parent)
    : QObject(parent), m_editorOrchestrator(&editorOrchestrator)
{
    Q_ASSERT(m_editorOrchestrator->thread() == thread());
    synchronizeOwnerState();
    connect(m_editorOrchestrator,
            &core::orchestration::EditorOrchestrator::previewPresentationStateChanged,
            this,
            &QtPreviewPresentationEventAdapter::recordOwnerStateChanged,
            Qt::DirectConnection);
    connect(m_editorOrchestrator,
            &core::orchestration::EditorOrchestrator::previewStarted,
            this,
            &QtPreviewPresentationEventAdapter::recordStarted,
            Qt::DirectConnection);
    connect(m_editorOrchestrator,
            &core::orchestration::EditorOrchestrator::previewUpdated,
            this,
            &QtPreviewPresentationEventAdapter::recordFrame,
            Qt::DirectConnection);
    connect(m_editorOrchestrator,
            &core::orchestration::EditorOrchestrator::previewWarning,
            this,
            &QtPreviewPresentationEventAdapter::recordWarning,
            Qt::DirectConnection);
    connect(m_editorOrchestrator,
            &core::orchestration::EditorOrchestrator::previewCompleted,
            this,
            &QtPreviewPresentationEventAdapter::recordCompleted,
            Qt::DirectConnection);
    connect(m_editorOrchestrator,
            &core::orchestration::EditorOrchestrator::previewFailed,
            this,
            &QtPreviewPresentationEventAdapter::recordFailed,
            Qt::DirectConnection);
    connect(m_editorOrchestrator,
            &core::orchestration::EditorOrchestrator::previewCancelled,
            this,
            &QtPreviewPresentationEventAdapter::recordCancelled,
            Qt::DirectConnection);
}

// 목적: queued callback을 차단하고 outliving subscription handle을 inactive 상태로 전환
// 입력: 없음
// 출력: adapter destruction 이후 callback 없음
QtPreviewPresentationEventAdapter::~QtPreviewPresentationEventAdapter()
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

// 목적: adapter delivery context에서 initial snapshot과 이후 frame·warning·terminal 구독
// 입력: callback: immutable Preview presentation event consumer
// 출력: RAII unsubscribe handle 또는 callback·thread 오류
core::client::PreviewPresentationSubscriptionResult QtPreviewPresentationEventAdapter::subscribeToPreviewPresentation(
    core::client::PreviewPresentationCallback callback)
{
    if (!callback)
    {
        return core::client::PreviewPresentationSubscriptionResult::failure(
            {core::client::ClientErrorCode::InvalidArgument, "Preview presentation callback must not be empty."});
    }
    if (QThread::currentThread() != thread())
    {
        return core::client::PreviewPresentationSubscriptionResult::failure(
            {core::client::ClientErrorCode::Conflict,
             "Preview presentation subscription must be created on its adapter delivery context."});
    }
    if (m_shuttingDown)
    {
        return core::client::PreviewPresentationSubscriptionResult::failure(
            {core::client::ClientErrorCode::Conflict, "Preview presentation event adapter is shutting down."});
    }

    const SubscriptionStatePtr state = std::make_shared<SubscriptionState>();
    state->callback = std::move(callback);
    core::client::PreviewPresentationSubscriptionHandle handle = std::make_shared<Subscription>(state);
    std::erase_if(m_subscriptions, [](const std::weak_ptr<SubscriptionState>& weakState) {
        const SubscriptionStatePtr existingState = weakState.lock();
        return existingState == nullptr || !existingState->isActive();
    });
    m_subscriptions.emplace_back(state);
    core::client::PreviewPresentationEvent initialEvent{
        nextEventSequence(),
        true,
        m_snapshot,
        false,
        false,
        std::nullopt,
        std::nullopt,
    };
    if (!enqueueEvent(state, std::move(initialEvent)))
    {
        state->unsubscribe();
        return core::client::PreviewPresentationSubscriptionResult::failure(
            {core::client::ClientErrorCode::Unknown, "Unable to queue the initial Preview presentation event."});
    }
    return core::client::PreviewPresentationSubscriptionResult::success(std::move(handle));
}

// 목적: adapter lifetime에서 0을 사용하지 않는 단조 event sequence 발급
// 입력: 없음
// 출력: 다음 Preview presentation event sequence
core::client::PreviewPresentationEventSequence QtPreviewPresentationEventAdapter::nextEventSequence() noexcept
{
    const std::uint64_t value = m_nextEventSequence;
    m_nextEventSequence = value == std::numeric_limits<std::uint64_t>::max() ? 1 : value + 1;
    return {value};
}

// 목적: Editor owner의 viewport·selection·request state를 presentation snapshot에 동기화
// 입력: 없음
// 출력: photo·develop revision·preview sequence가 다른 frame을 제거한 최신 snapshot
void QtPreviewPresentationEventAdapter::synchronizeOwnerState()
{
    const core::client::EditorSnapshot editor = m_editorOrchestrator->editorSnapshot();
    m_snapshot.selectedPhotoId =
        editor.hasSelection ? std::optional<core::client::ClientPhotoId>{editor.photoId} : std::nullopt;
    m_snapshot.developRevision = editor.developRevision;
    m_snapshot.previewSequence = {m_editorOrchestrator->previewSequence()};
    const QSize targetSize = m_editorOrchestrator->previewTargetSize();
    m_snapshot.viewport =
        targetSize.width() > 0 && targetSize.height() > 0
            ? std::optional<core::client::PreviewViewport>{core::client::PreviewViewport{
                  static_cast<std::uint32_t>(targetSize.width()), static_cast<std::uint32_t>(targetSize.height())}}
            : std::nullopt;
    const std::uint64_t activeRequestId = m_editorOrchestrator->activePreviewRequestId();
    m_snapshot.activeRequestId = toOptionalRequestId(activeRequestId);

    if (m_snapshot.currentFrame.has_value() &&
        (!m_snapshot.selectedPhotoId.has_value() || m_snapshot.currentFrame->photoId != *m_snapshot.selectedPhotoId ||
         m_snapshot.currentFrame->developRevision != m_snapshot.developRevision ||
         m_snapshot.currentFrame->previewSequence != m_snapshot.previewSequence))
    {
        m_snapshot.currentFrame.reset();
    }
    if (!m_snapshot.activeRequestId.has_value())
    {
        m_activeRequest.reset();
    }
}

// 목적: owner state invalidation을 frame 없는 presentation event로 전달
// 입력: 없음
// 출력: 최신 snapshot event queue 등록
void QtPreviewPresentationEventAdapter::recordOwnerStateChanged()
{
    synchronizeOwnerState();
    publishEvent(false, false, std::nullopt, std::nullopt);
}

// 목적: accepted Preview identity와 당시 photo/sequence context 기록
// 입력: requestId: owner-issued nonzero request identity
// 출력: active request가 포함된 snapshot event queue 등록
void QtPreviewPresentationEventAdapter::recordStarted(std::uint64_t requestId)
{
    if (requestId == 0)
    {
        return;
    }
    synchronizeOwnerState();
    m_activeRequest = ActiveRequestContext{
        {requestId}, m_snapshot.selectedPhotoId, m_snapshot.developRevision, m_snapshot.previewSequence};
    m_snapshot.activeRequestId = core::client::PreviewRequestId{requestId};
    publishEvent(true, false, std::nullopt, std::nullopt);
}

// 목적: stale filtering이 끝난 Qt Preview result를 client frame/analysis로 투영
// 입력: result: current request의 image, analysis와 identity
// 출력: currentFrame이 갱신된 event 또는 projection warning
void QtPreviewPresentationEventAdapter::recordFrame(const core::orchestration::PreviewResult& result)
{
    synchronizeOwnerState();
    const std::optional<core::client::DisplayFrame> displayFrame = toDisplayFrame(result.image, result.previewSequence);
    if (!displayFrame.has_value())
    {
        publishEvent(false,
                     false,
                     core::client::PreviewWarning{toOptionalRequestId(result.requestId),
                                                  toOptionalPhotoId(result.photo.photoId.value),
                                                  {result.photo.developRevision},
                                                  {result.previewSequence},
                                                  {core::client::ClientErrorCode::Unknown,
                                                   "Unable to project the current Preview image into DisplayFrame."}},
                     std::nullopt);
        return;
    }

    std::optional<core::client::PreviewAnalysisSnapshot> analysis;
    if (result.renderMode == core::orchestration::PreviewRenderMode::Final)
    {
        analysis = core::client::PreviewAnalysisSnapshot{toClientHistogram(result.histogram),
                                                         toClientClipping(result.clipping)};
    }

    core::client::PreviewFrameSnapshot frame{
        {result.requestId},
        {result.photo.photoId.value},
        {result.photo.developRevision},
        {result.previewSequence},
        toClientTier(result.tier),
        toClientMode(result.renderMode),
        *displayFrame,
        std::move(analysis),
    };
    m_snapshot.selectedPhotoId = frame.photoId;
    m_snapshot.developRevision = frame.developRevision;
    m_snapshot.previewSequence = frame.previewSequence;
    m_snapshot.activeRequestId = frame.requestId;
    m_snapshot.currentFrame = std::move(frame);
    publishEvent(false, true, std::nullopt, std::nullopt);
}

// 목적: owner의 비치명적 Preview issue를 client warning으로 투영
// 입력: issue: optional accepted identity와 technical error
// 출력: warning event queue 등록
void QtPreviewPresentationEventAdapter::recordWarning(const core::orchestration::PreviewIssue& issue)
{
    synchronizeOwnerState();
    publishEvent(false,
                 false,
                 core::client::PreviewWarning{toOptionalRequestId(issue.requestId),
                                              toOptionalPhotoId(issue.photo.photoId.value),
                                              {issue.photo.developRevision},
                                              {issue.previewSequence},
                                              core::orchestration::toClientError(issue.error)},
                 std::nullopt);
}

// 목적: accepted Preview 정상 완료를 exact terminal로 투영
// 입력: requestId: 완료된 owner request identity
// 출력: active identity 제거와 Completed terminal event
void QtPreviewPresentationEventAdapter::recordCompleted(std::uint64_t requestId)
{
    const ActiveRequestContext context = m_activeRequest.value_or(ActiveRequestContext{
        {requestId}, m_snapshot.selectedPhotoId, m_snapshot.developRevision, m_snapshot.previewSequence});
    synchronizeOwnerState();
    m_snapshot.activeRequestId.reset();
    m_activeRequest.reset();
    publishEvent(false,
                 false,
                 std::nullopt,
                 core::client::PreviewTerminal{context.requestId,
                                               context.photoId,
                                               context.developRevision,
                                               context.previewSequence,
                                               core::client::PreviewTerminalState::Completed,
                                               std::nullopt});
}

// 목적: Preview submit 또는 실행 실패를 typed terminal로 투영
// 입력: issue: accepted identity가 0일 수 있는 failure context
// 출력: accepted request면 exact terminal, 아니면 identity 없는 failed terminal
void QtPreviewPresentationEventAdapter::recordFailed(const core::orchestration::PreviewIssue& issue)
{
    const std::optional<core::client::PreviewRequestId> requestId = toOptionalRequestId(issue.requestId);
    const std::optional<core::client::ClientPhotoId> photoId = toOptionalPhotoId(issue.photo.photoId.value);
    const core::client::SessionDevelopRevision developRevision{issue.photo.developRevision};
    const core::client::PreviewSequence sequence{issue.previewSequence};
    synchronizeOwnerState();
    if (requestId.has_value() && m_snapshot.activeRequestId == requestId)
    {
        m_snapshot.activeRequestId.reset();
    }
    m_snapshot.currentFrame.reset();
    m_activeRequest.reset();
    publishEvent(false,
                 false,
                 std::nullopt,
                 core::client::PreviewTerminal{requestId,
                                               photoId,
                                               developRevision,
                                               sequence,
                                               core::client::PreviewTerminalState::Failed,
                                               core::orchestration::toClientError(issue.error)});
}

// 목적: accepted Preview cancellation을 exact terminal로 투영
// 입력: requestId: 취소된 owner request identity
// 출력: active identity 제거와 Cancelled terminal event
void QtPreviewPresentationEventAdapter::recordCancelled(std::uint64_t requestId)
{
    const ActiveRequestContext context = m_activeRequest.value_or(ActiveRequestContext{
        {requestId}, m_snapshot.selectedPhotoId, m_snapshot.developRevision, m_snapshot.previewSequence});
    synchronizeOwnerState();
    if (m_snapshot.activeRequestId == context.requestId)
    {
        m_snapshot.activeRequestId.reset();
    }
    m_activeRequest.reset();
    publishEvent(false,
                 false,
                 std::nullopt,
                 core::client::PreviewTerminal{context.requestId,
                                               context.photoId,
                                               context.developRevision,
                                               context.previewSequence,
                                               core::client::PreviewTerminalState::Cancelled,
                                               std::nullopt});
}

// 목적: current snapshot과 optional warning/terminal을 모든 active subscription에 fan-out
// 입력: requestStarted/frameUpdated: transition 종류, warning/terminal: 이번 transition payload
// 출력: subscription별 Qt queued callback 등록
void QtPreviewPresentationEventAdapter::publishEvent(bool requestStarted,
                                                     bool frameUpdated,
                                                     std::optional<core::client::PreviewWarning> warning,
                                                     std::optional<core::client::PreviewTerminal> terminal)
{
    if (m_shuttingDown)
    {
        return;
    }
    const core::client::PreviewPresentationEvent event{
        nextEventSequence(),
        false,
        m_snapshot,
        requestStarted,
        frameUpdated,
        std::move(warning),
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

// 목적: 한 subscription에 immutable event를 Qt queued callback으로 등록
// 입력: state: subscription lifetime, event: 전달할 Preview presentation event
// 출력: 전달 불필요 또는 queue 성공이면 true
bool QtPreviewPresentationEventAdapter::enqueueEvent(const SubscriptionStatePtr& state,
                                                     core::client::PreviewPresentationEvent event)
{
    if (!state->isActive())
    {
        return true;
    }
    return QMetaObject::invokeMethod(
        this, [state, event = std::move(event)] { deliverEvent(state, event); }, Qt::QueuedConnection);
}

// 목적: queued immutable event를 active callback 하나에 예외 격리하여 전달
// 입력: state: subscription lifetime, event: 전달할 Preview presentation event
// 출력: 없음
void QtPreviewPresentationEventAdapter::deliverEvent(const SubscriptionStatePtr& state,
                                                     const core::client::PreviewPresentationEvent& event) noexcept
{
    try
    {
        const std::scoped_lock lock(state->mutex);
        if (!state->active)
        {
            return;
        }
        const core::client::PreviewPresentationCallback callback = state->callback;
        callback(event);
    }
    catch (...)
    {
        // Consumer 예외는 Qt event loop와 다른 subscription delivery로 전파하지 않는다.
    }
}

}  // namespace flexraw::ui::facade
