#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

#include "client_error.h"
#include "client_identity.h"
#include "client_result.h"
#include "display_frame.h"
#include "editor_client.h"

namespace flexraw::core::client
{

struct PreviewRequestId
{
    std::uint64_t value{0};

    bool operator==(const PreviewRequestId&) const = default;
};

struct PreviewSequence
{
    std::uint64_t value{0};

    bool operator==(const PreviewSequence&) const = default;
};

struct PreviewViewport
{
    std::uint32_t width{0};
    std::uint32_t height{0};

    bool operator==(const PreviewViewport&) const = default;
};

struct SetPreviewViewportCommand
{
    PreviewViewport viewport;
};

struct SetPreviewComparisonCommand
{
    // 기본 Develop recipe와 현재 보정 Preview의 비교를 요청하거나 해제한다.
    bool enabled{false};
};

enum class PreviewFrameTier : std::uint8_t
{
    Thumbnail,
    Standard,
};

enum class PreviewPresentationMode : std::uint8_t
{
    Interactive,
    Final,
};

using PreviewHistogramBins = std::array<std::uint32_t, 256>;

struct PreviewHistogramSnapshot
{
    PreviewHistogramBins red{};
    PreviewHistogramBins green{};
    PreviewHistogramBins blue{};
    PreviewHistogramBins luminance{};
    std::uint64_t pixelCount{0};

    bool operator==(const PreviewHistogramSnapshot&) const = default;
};

struct PreviewClippingSnapshot
{
    std::uint64_t shadowPixelCount{0};
    std::uint64_t highlightPixelCount{0};
    std::uint64_t pixelCount{0};

    bool operator==(const PreviewClippingSnapshot&) const = default;
};

struct PreviewAnalysisSnapshot
{
    PreviewHistogramSnapshot histogram;
    PreviewClippingSnapshot clipping;

    bool operator==(const PreviewAnalysisSnapshot&) const = default;
};

struct PreviewFrameSnapshot
{
    PreviewRequestId requestId;
    ClientPhotoId photoId;
    SessionDevelopRevision developRevision;
    PreviewSequence previewSequence;
    PreviewFrameTier tier{PreviewFrameTier::Thumbnail};
    PreviewPresentationMode mode{PreviewPresentationMode::Final};
    DisplayFrame frame;
    std::optional<PreviewAnalysisSnapshot> analysis;
};

struct PreviewComparisonFrameSnapshot
{
    PreviewRequestId requestId;
    ClientPhotoId photoId;
    // 기본 recipe의 revision이 아니라 요청 당시 Editor의 session revision이다.
    SessionDevelopRevision capturedDevelopRevision;
    PreviewFrameTier tier{PreviewFrameTier::Thumbnail};
    DisplayFrame frame;
};

struct PreviewComparisonSnapshot
{
    bool enabled{false};
    // 일반 Preview의 activeRequestId와 독립적인 비교 요청이다.
    std::optional<PreviewRequestId> activeRequestId;
    std::optional<PreviewComparisonFrameSnapshot> baselineFrame;
};

struct PreviewComparisonCommandState
{
    bool enabled{false};
    std::optional<PreviewRequestId> activeRequestId;

    bool operator==(const PreviewComparisonCommandState&) const = default;
};

struct PreviewPresentationSnapshot
{
    std::optional<PreviewViewport> viewport;
    std::optional<ClientPhotoId> selectedPhotoId;
    SessionDevelopRevision developRevision;
    PreviewSequence previewSequence;
    std::optional<PreviewRequestId> activeRequestId;
    std::optional<PreviewFrameSnapshot> currentFrame;
    PreviewComparisonSnapshot comparison;
};

struct PreviewWarning
{
    std::optional<PreviewRequestId> requestId;
    std::optional<ClientPhotoId> photoId;
    SessionDevelopRevision developRevision;
    PreviewSequence previewSequence;
    ClientError error;

    bool operator==(const PreviewWarning&) const = default;
};

enum class PreviewTerminalState : std::uint8_t
{
    Completed,
    Failed,
    Cancelled,
};

struct PreviewTerminal
{
    std::optional<PreviewRequestId> requestId;
    std::optional<ClientPhotoId> photoId;
    SessionDevelopRevision developRevision;
    PreviewSequence previewSequence;
    PreviewTerminalState state{PreviewTerminalState::Completed};
    std::optional<ClientError> error;

    bool operator==(const PreviewTerminal&) const = default;
};

struct PreviewComparisonTransition
{
    // 비교 상태 변경이나 frame 폐기는 이 값의 존재로 구별한다.
    bool requestStarted{false};
    bool frameUpdated{false};
    // 비교 warning/terminal의 revision과 sequence는 요청 당시 캡처한 값이다.
    std::optional<PreviewWarning> warning;
    std::optional<PreviewTerminal> terminal;
};

struct PreviewPresentationEventSequence
{
    std::uint64_t value{0};

    bool operator==(const PreviewPresentationEventSequence&) const = default;
};

struct PreviewPresentationEvent
{
    PreviewPresentationEventSequence sequence;
    bool initial{false};
    PreviewPresentationSnapshot snapshot;
    bool requestStarted{false};
    bool frameUpdated{false};
    std::optional<PreviewWarning> warning;
    std::optional<PreviewTerminal> terminal;
    // 일반 Preview lifecycle과 구별되는 기본 Develop 비교 transition.
    std::optional<PreviewComparisonTransition> comparison;
};

using PreviewViewportResult = ClientResult<PreviewViewport, ClientError>;
using PreviewRequestCancelResult = ClientResult<PreviewRequestId, ClientError>;
using PreviewComparisonResult = ClientResult<PreviewComparisonCommandState, ClientError>;
using PreviewPresentationCallback = std::function<void(const PreviewPresentationEvent&)>;

class IPreviewPresentationSubscription
{
public:
    // 목적: implementation별 Preview presentation subscription resource를 concrete destructor로 정리
    // 입력: 없음
    // 출력: 없음
    virtual ~IPreviewPresentationSubscription() = default;

    // 목적: queued event와 이후 Preview presentation callback 전달 차단
    // 입력: 없음
    // 출력: 없음; 여러 번 호출해도 같은 inactive 상태 유지
    virtual void unsubscribe() noexcept = 0;

    // 목적: subscription이 이후 callback을 받을 수 있는지 조회
    // 입력: 없음
    // 출력: callback 전달이 허용된 상태이면 true
    [[nodiscard]] virtual bool isActive() const noexcept = 0;
};

using PreviewPresentationSubscriptionHandle = std::shared_ptr<IPreviewPresentationSubscription>;
using PreviewPresentationSubscriptionResult = ClientResult<PreviewPresentationSubscriptionHandle, ClientError>;

class IPreviewPresentationClient
{
public:
    // 목적: implementation별 Preview presentation command resource를 concrete destructor로 정리
    // 입력: 없음
    // 출력: 없음
    virtual ~IPreviewPresentationClient() = default;

    // 목적: frontend viewport를 Preview owner의 최신 render target으로 설정
    // 입력: command: 양수 pixel width와 height
    // 출력: 적용된 viewport 또는 validation 오류
    [[nodiscard]] virtual PreviewViewportResult setPreviewViewport(const SetPreviewViewportCommand& command) = 0;

    // 목적: 현재 accepted Preview request의 향후 frame publication과 processing 취소
    // 입력: requestId: owner가 발급해 presentation snapshot에 공개한 identity
    // 출력: 취소된 identity 또는 stale·validation 오류
    [[nodiscard]] virtual PreviewRequestCancelResult cancelPreviewRequest(PreviewRequestId requestId) = 0;
};

class IPreviewComparisonClient
{
public:
    // 목적: 별도 비교 command port의 concrete implementation을 안전하게 소멸
    // 입력: 없음
    // 출력: 없음
    virtual ~IPreviewComparisonClient() = default;

    // 목적: 현재 선택 사진의 기본 Develop 비교를 시작하거나 현재 비교를 해제
    // 입력: command: 활성화 여부; 표시 mode와 분할 위치는 frontend가 소유
    // 출력: 적용된 활성 상태와 accepted request ID 또는 선택·source·viewport·submit 오류
    [[nodiscard]] virtual PreviewComparisonResult setPreviewComparison(const SetPreviewComparisonCommand& command) = 0;
};

class IPreviewPresentationEventSource
{
public:
    // 목적: implementation별 Preview presentation event resource를 concrete destructor로 정리
    // 입력: 없음
    // 출력: 없음
    virtual ~IPreviewPresentationEventSource() = default;

    // 목적: adapter delivery context에서 initial snapshot과 이후 frame·warning·terminal 구독
    // 입력: callback: immutable Preview presentation event consumer
    // 출력: unsubscribe lifetime handle 또는 callback·delivery context 오류
    [[nodiscard]] virtual PreviewPresentationSubscriptionResult subscribeToPreviewPresentation(
        PreviewPresentationCallback callback) = 0;
};

}  // namespace flexraw::core::client
