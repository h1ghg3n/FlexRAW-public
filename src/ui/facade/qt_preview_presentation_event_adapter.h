#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include <QObject>

#include "preview_presentation_client.h"

namespace flexraw::core::develop
{
struct ClippingSummary;
struct ImageHistogram;
}  // namespace flexraw::core::develop

namespace flexraw::core::orchestration
{
class EditorOrchestrator;
struct PreviewIssue;
struct PreviewResult;
}  // namespace flexraw::core::orchestration

namespace flexraw::ui::facade
{

// 목적: Qt-free histogram snapshot을 기존 Qt Develop panel 값으로 변환
// 입력: snapshot: fixed-width 256-bin RGB/luminance 분석
// 출력: 같은 channel count를 보존한 develop histogram
[[nodiscard]] core::develop::ImageHistogram toDevelopHistogram(const core::client::PreviewHistogramSnapshot& snapshot);

// 목적: Qt-free clipping snapshot을 기존 Qt Preview widget 값으로 변환
// 입력: snapshot: shadow/highlight/전체 pixel count
// 출력: 같은 count를 보존한 develop clipping summary
[[nodiscard]] core::develop::ClippingSummary toDevelopClipping(const core::client::PreviewClippingSnapshot& snapshot);

class QtPreviewPresentationEventAdapter final : public QObject, public core::client::IPreviewPresentationEventSource
{
public:
    // 목적: Editor Preview lifecycle을 Qt-free presentation snapshot/event로 변환
    // 입력: editorOrchestrator: authoritative Preview request owner, parent: Qt lifetime owner
    // 출력: 현재 Qt delivery context에 bound된 adapter
    explicit QtPreviewPresentationEventAdapter(core::orchestration::EditorOrchestrator& editorOrchestrator,
                                               QObject* parent = nullptr);

    // 목적: queued callback을 차단하고 outliving subscription handle을 inactive 상태로 전환
    // 입력: 없음
    // 출력: adapter destruction 이후 callback 없음
    ~QtPreviewPresentationEventAdapter() override;

    // 목적: adapter delivery context에서 initial snapshot과 이후 frame·warning·terminal 구독
    // 입력: callback: immutable Preview presentation event consumer
    // 출력: RAII unsubscribe handle 또는 callback·thread 오류
    [[nodiscard]] core::client::PreviewPresentationSubscriptionResult subscribeToPreviewPresentation(
        core::client::PreviewPresentationCallback callback) override;

private:
    struct SubscriptionState;
    class Subscription;
    using SubscriptionStatePtr = std::shared_ptr<SubscriptionState>;

    struct ActiveRequestContext
    {
        core::client::PreviewRequestId requestId;
        std::optional<core::client::ClientPhotoId> photoId;
        core::client::SessionDevelopRevision developRevision;
        core::client::PreviewSequence previewSequence;
    };

    // 목적: adapter lifetime에서 0을 사용하지 않는 단조 event sequence 발급
    // 입력: 없음
    // 출력: 다음 Preview presentation event sequence
    [[nodiscard]] core::client::PreviewPresentationEventSequence nextEventSequence() noexcept;

    // 목적: Editor owner의 viewport·selection·request state를 presentation snapshot에 동기화
    // 입력: 없음
    // 출력: photo·develop revision·preview sequence가 다른 frame을 제거한 최신 snapshot
    void synchronizeOwnerState();

    // 목적: owner state invalidation을 frame 없는 presentation event로 전달
    // 입력: 없음
    // 출력: 최신 snapshot event queue 등록
    void recordOwnerStateChanged();

    // 목적: accepted Preview identity와 당시 photo/sequence context 기록
    // 입력: requestId: owner-issued nonzero request identity
    // 출력: active request가 포함된 snapshot event queue 등록
    void recordStarted(std::uint64_t requestId);

    // 목적: stale filtering이 끝난 Qt Preview result를 client frame/analysis로 투영
    // 입력: result: current request의 image, analysis와 identity
    // 출력: currentFrame이 갱신된 event 또는 projection warning
    void recordFrame(const core::orchestration::PreviewResult& result);

    // 목적: owner의 비치명적 Preview issue를 client warning으로 투영
    // 입력: issue: optional accepted identity와 technical error
    // 출력: warning event queue 등록
    void recordWarning(const core::orchestration::PreviewIssue& issue);

    // 목적: accepted Preview 정상 완료를 exact terminal로 투영
    // 입력: requestId: 완료된 owner request identity
    // 출력: active identity 제거와 Completed terminal event
    void recordCompleted(std::uint64_t requestId);

    // 목적: Preview submit 또는 실행 실패를 typed terminal로 투영
    // 입력: issue: accepted identity가 0일 수 있는 failure context
    // 출력: accepted request면 exact terminal, 아니면 identity 없는 failed terminal
    void recordFailed(const core::orchestration::PreviewIssue& issue);

    // 목적: accepted Preview cancellation을 exact terminal로 투영
    // 입력: requestId: 취소된 owner request identity
    // 출력: active identity 제거와 Cancelled terminal event
    void recordCancelled(std::uint64_t requestId);

    // 목적: current snapshot과 optional warning/terminal을 모든 active subscription에 fan-out
    // 입력: requestStarted/frameUpdated: transition 종류, warning/terminal: 이번 transition payload
    // 출력: subscription별 Qt queued callback 등록
    void publishEvent(bool requestStarted,
                      bool frameUpdated,
                      std::optional<core::client::PreviewWarning> warning,
                      std::optional<core::client::PreviewTerminal> terminal);

    // 목적: 한 subscription에 immutable event를 Qt queued callback으로 등록
    // 입력: state: subscription lifetime, event: 전달할 Preview presentation event
    // 출력: 전달 불필요 또는 queue 성공이면 true
    [[nodiscard]] bool enqueueEvent(const SubscriptionStatePtr& state, core::client::PreviewPresentationEvent event);

    // 목적: queued immutable event를 active callback 하나에 예외 격리하여 전달
    // 입력: state: subscription lifetime, event: 전달할 Preview presentation event
    // 출력: 없음
    static void deliverEvent(const SubscriptionStatePtr& state,
                             const core::client::PreviewPresentationEvent& event) noexcept;

    core::orchestration::EditorOrchestrator* m_editorOrchestrator{nullptr};
    core::client::PreviewPresentationSnapshot m_snapshot;
    std::optional<ActiveRequestContext> m_activeRequest;
    std::vector<std::weak_ptr<SubscriptionState>> m_subscriptions;
    std::uint64_t m_nextEventSequence{1};
    bool m_shuttingDown{false};
};

}  // namespace flexraw::ui::facade
