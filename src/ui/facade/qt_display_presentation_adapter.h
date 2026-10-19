#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>

#include <QByteArray>
#include <QImage>

#include "display_frame.h"
#include "display_profile_service.h"

class QScreen;

namespace flexraw::ui::facade
{

enum class DisplayPresentationSlot : std::uint8_t
{
    Current,
    Comparison,
};

enum class DisplayPresentationFallbackReason : std::uint8_t
{
    None,
    ServiceUnavailable,
    TargetResolveFailed,
    NoExplicitProfile,
    TargetUnavailable,
    QueryFailed,
    ReadFailed,
    PayloadTooLarge,
    InvalidProfile,
    TransformFailed,
};

using DisplayTargetResolver = std::function<std::optional<platform::DisplayTargetId>(
    const QScreen& screen, const platform::IDisplayProfileService& service)>;

class QtDisplayPresentationAdapter final
{
public:
    // 목적: Qt Preview presentation을 current display profile과 독립된 cache owner로 초기화
    // 입력: profileService: optional one-shot Platform profile service, targetResolver: Desktop-only screen mapper
    // 출력: service가 없으면 sRGB fallback을 유지하는 adapter
    explicit QtDisplayPresentationAdapter(platform::IDisplayProfileService* profileService = nullptr,
                                          DisplayTargetResolver targetResolver = {});

    // 목적: QWindow가 선택한 screen의 target/profile로 presentation generation 갱신
    // 입력: screen: top-level QWindow::screen 또는 아직 native window가 없으면 nullptr
    // 출력: target/profile/fallback state가 바뀌어 cached frame을 다시 변환했으면 true
    [[nodiscard]] bool setScreen(const QScreen* screen);

    // 목적: immutable monitor-neutral frame을 slot에 저장하고 current profile로 표시 변환
    // 입력: slot: current 또는 comparison surface, frame: BGRA8/top-down/sRGB frame
    // 출력: current presentation generation에 대응하는 transformed image 또는 sRGB fallback
    [[nodiscard]] QImage presentFrame(DisplayPresentationSlot slot, const core::client::DisplayFrame& frame);

    // 목적: 더 이상 유효하지 않은 frame과 monitor별 transformed cache 제거
    // 입력: slot: 비울 current 또는 comparison surface
    // 출력: 해당 slot의 source/cache lifetime 종료
    void clearFrame(DisplayPresentationSlot slot);

    // 목적: current target/profile generation에 대응하는 slot image 조회
    // 입력: slot: current 또는 comparison surface
    // 출력: current transformed image, sRGB fallback 또는 frame이 없으면 null image
    [[nodiscard]] QImage presentedFrame(DisplayPresentationSlot slot) const;

    // 목적: 현재 monitor별 color transform 사용 가능 여부 조회
    // 입력: 없음
    // 출력: valid target ICC profile이 active이면 true
    [[nodiscard]] bool hasManagedProfile() const noexcept;

    // 목적: current presentation이 sRGB fallback인 이유 조회
    // 입력: 없음
    // 출력: managed profile이면 None, 아니면 구체적인 discovery/validation failure
    [[nodiscard]] DisplayPresentationFallbackReason fallbackReason() const noexcept;

    // 목적: current Platform display target identity 조회
    // 입력: 없음
    // 출력: resolver가 unique target을 찾았으면 identity
    [[nodiscard]] const std::optional<platform::DisplayTargetId>& targetId() const noexcept;

    // 목적: target/profile state change마다 증가하는 presentation generation 조회
    // 입력: 없음
    // 출력: 0을 사용하지 않는 process-local generation
    [[nodiscard]] std::uint64_t presentationGeneration() const noexcept;

    // 목적: current ICC payload의 stable content hash 조회
    // 입력: 없음
    // 출력: managed profile이면 SHA-256, fallback이면 빈 byte array
    [[nodiscard]] const QByteArray& profileHash() const noexcept;

private:
    struct CachedFrame
    {
        std::optional<core::client::DisplayFrame> source;
        QImage presented;
        std::uint64_t presentationGeneration{0};
    };

    // 목적: enum slot을 fixed cache array index로 변환
    // 입력: slot: current 또는 comparison surface
    // 출력: m_frames의 유효 index
    [[nodiscard]] static std::size_t slotIndex(DisplayPresentationSlot slot) noexcept;

    // 목적: stored source frame을 active profile 또는 sRGB fallback으로 다시 투영
    // 입력: slot: 갱신할 cache slot
    // 출력: current generation image를 cache에 저장
    void reprojectFrame(DisplayPresentationSlot slot);

    // 목적: 두 presentation slot의 retained source를 새 target/profile로 다시 투영
    // 입력: 없음
    // 출력: current와 comparison cache가 같은 generation으로 갱신됨
    void reprojectFrames();

    platform::IDisplayProfileService* m_profileService{nullptr};
    DisplayTargetResolver m_targetResolver;
    std::optional<platform::DisplayTargetId> m_targetId;
    QByteArray m_profileData;
    QByteArray m_profileHash;
    DisplayPresentationFallbackReason m_fallbackReason{DisplayPresentationFallbackReason::ServiceUnavailable};
    std::uint64_t m_presentationGeneration{1};
    std::array<CachedFrame, 2> m_frames;
};

}  // namespace flexraw::ui::facade
