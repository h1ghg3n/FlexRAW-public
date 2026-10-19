#include "qt_display_presentation_adapter.h"

#include <limits>
#include <utility>
#include <variant>

#include <lcms2.h>

#include <QCryptographicHash>
#include <QScreen>

#include "display_frame_qt_adapter.h"

namespace flexraw::ui::facade
{
namespace
{

// 목적: Platform profile unavailable reason을 Qt presentation fallback reason으로 변환
// 입력: reason: profile discovery contract의 구체적인 실패 분류
// 출력: 같은 의미의 GUI presentation fallback 분류
[[nodiscard]] DisplayPresentationFallbackReason toFallbackReason(platform::DisplayProfileUnavailableReason reason)
{
    switch (reason)
    {
    case platform::DisplayProfileUnavailableReason::NoExplicitProfile:
        return DisplayPresentationFallbackReason::NoExplicitProfile;
    case platform::DisplayProfileUnavailableReason::TargetUnavailable:
        return DisplayPresentationFallbackReason::TargetUnavailable;
    case platform::DisplayProfileUnavailableReason::QueryFailed:
        return DisplayPresentationFallbackReason::QueryFailed;
    case platform::DisplayProfileUnavailableReason::ReadFailed:
        return DisplayPresentationFallbackReason::ReadFailed;
    case platform::DisplayProfileUnavailableReason::PayloadTooLarge:
        return DisplayPresentationFallbackReason::PayloadTooLarge;
    }
    return DisplayPresentationFallbackReason::QueryFailed;
}

// 목적: bounded ICC payload가 LittleCMS RGB output profile로 열리는지 검증
// 입력: profileData: Platform service가 반환한 owned profile byte
// 출력: valid RGB profile이면 true
[[nodiscard]] bool isValidRgbProfile(const QByteArray& profileData)
{
    if (profileData.isEmpty() || profileData.size() > std::numeric_limits<cmsUInt32Number>::max())
    {
        return false;
    }

    cmsHPROFILE profile =
        cmsOpenProfileFromMem(profileData.constData(), static_cast<cmsUInt32Number>(profileData.size()));
    if (profile == nullptr)
    {
        return false;
    }
    const bool valid = cmsGetColorSpace(profile) == cmsSigRgbData;
    cmsCloseProfile(profile);
    return valid;
}

// 목적: monitor-neutral sRGB QImage를 target ICC profile의 RGBA8 presentation image로 변환
// 입력: source: opaque RGBA8888 image, profileData: validation된 target ICC byte
// 출력: LittleCMS transform 성공 시 image, 실패 시 null image
[[nodiscard]] QImage transformForDisplay(const QImage& source, const QByteArray& profileData)
{
    if (source.isNull() || profileData.isEmpty() || profileData.size() > std::numeric_limits<cmsUInt32Number>::max())
    {
        return {};
    }

    const quint64 pixelCount = static_cast<quint64>(source.width()) * static_cast<quint64>(source.height());
    if (pixelCount == 0 || pixelCount > std::numeric_limits<cmsUInt32Number>::max())
    {
        return {};
    }

    cmsHPROFILE sourceProfile = cmsCreate_sRGBProfile();
    cmsHPROFILE destinationProfile =
        cmsOpenProfileFromMem(profileData.constData(), static_cast<cmsUInt32Number>(profileData.size()));
    if (sourceProfile == nullptr || destinationProfile == nullptr)
    {
        if (destinationProfile != nullptr)
        {
            cmsCloseProfile(destinationProfile);
        }
        if (sourceProfile != nullptr)
        {
            cmsCloseProfile(sourceProfile);
        }
        return {};
    }

    cmsHTRANSFORM transform = cmsCreateTransform(
        sourceProfile, TYPE_RGBA_8, destinationProfile, TYPE_RGBA_8, INTENT_PERCEPTUAL, cmsFLAGS_COPY_ALPHA);
    if (transform == nullptr)
    {
        cmsCloseProfile(destinationProfile);
        cmsCloseProfile(sourceProfile);
        return {};
    }

    const QImage sourceRgba = source.convertToFormat(QImage::Format_RGBA8888);
    QImage destination(sourceRgba.size(), QImage::Format_RGBA8888);
    if (!sourceRgba.isNull() && !destination.isNull())
    {
        cmsDoTransform(transform, sourceRgba.constBits(), destination.bits(), static_cast<cmsUInt32Number>(pixelCount));
    }
    else
    {
        destination = {};
    }

    cmsDeleteTransform(transform);
    cmsCloseProfile(destinationProfile);
    cmsCloseProfile(sourceProfile);
    return destination;
}

}  // namespace

// 목적: Qt Preview presentation을 current display profile과 독립된 cache owner로 초기화
// 입력: profileService: optional one-shot Platform profile service, targetResolver: Desktop-only screen mapper
// 출력: service가 없으면 sRGB fallback을 유지하는 adapter
QtDisplayPresentationAdapter::QtDisplayPresentationAdapter(platform::IDisplayProfileService* profileService,
                                                           DisplayTargetResolver targetResolver)
    : m_profileService(profileService), m_targetResolver(std::move(targetResolver))
{}

// 목적: QWindow가 선택한 screen의 target/profile로 presentation generation 갱신
// 입력: screen: top-level QWindow::screen 또는 아직 native window가 없으면 nullptr
// 출력: target/profile/fallback state가 바뀌어 cached frame을 다시 변환했으면 true
bool QtDisplayPresentationAdapter::setScreen(const QScreen* screen)
{
    std::optional<platform::DisplayTargetId> targetId;
    QByteArray profileData;
    QByteArray profileHash;
    DisplayPresentationFallbackReason fallbackReason = DisplayPresentationFallbackReason::ServiceUnavailable;

    if (m_profileService != nullptr && m_targetResolver && screen != nullptr)
    {
        targetId = m_targetResolver(*screen, *m_profileService);
        fallbackReason = DisplayPresentationFallbackReason::TargetResolveFailed;
        if (targetId.has_value())
        {
            const platform::DisplayProfileResult profile = m_profileService->displayProfile(*targetId);
            if (const auto* snapshot = std::get_if<platform::DisplayProfileSnapshot>(&profile);
                snapshot != nullptr && snapshot->targetId == *targetId)
            {
                profileData = QByteArray(reinterpret_cast<const char*>(snapshot->iccProfile.data()),
                                         static_cast<qsizetype>(snapshot->iccProfile.size()));
                if (isValidRgbProfile(profileData))
                {
                    profileHash = QCryptographicHash::hash(profileData, QCryptographicHash::Sha256);
                    fallbackReason = DisplayPresentationFallbackReason::None;
                }
                else
                {
                    profileData.clear();
                    fallbackReason = DisplayPresentationFallbackReason::InvalidProfile;
                }
            }
            else if (const auto* unavailable = std::get_if<platform::DisplayProfileUnavailable>(&profile))
            {
                fallbackReason = toFallbackReason(unavailable->reason);
            }
        }
    }

    if (targetId == m_targetId && profileHash == m_profileHash && fallbackReason == m_fallbackReason)
    {
        return false;
    }

    m_targetId = std::move(targetId);
    m_profileData = std::move(profileData);
    m_profileHash = std::move(profileHash);
    m_fallbackReason = fallbackReason;
    ++m_presentationGeneration;
    reprojectFrames();
    return true;
}

// 목적: immutable monitor-neutral frame을 slot에 저장하고 current profile로 표시 변환
// 입력: slot: current 또는 comparison surface, frame: BGRA8/top-down/sRGB frame
// 출력: current presentation generation에 대응하는 transformed image 또는 sRGB fallback
QImage QtDisplayPresentationAdapter::presentFrame(DisplayPresentationSlot slot, const core::client::DisplayFrame& frame)
{
    m_frames[slotIndex(slot)].source = frame;
    reprojectFrame(slot);
    return presentedFrame(slot);
}

// 목적: 더 이상 유효하지 않은 frame과 monitor별 transformed cache 제거
// 입력: slot: 비울 current 또는 comparison surface
// 출력: 해당 slot의 source/cache lifetime 종료
void QtDisplayPresentationAdapter::clearFrame(DisplayPresentationSlot slot)
{
    m_frames[slotIndex(slot)] = {};
}

// 목적: current target/profile generation에 대응하는 slot image 조회
// 입력: slot: current 또는 comparison surface
// 출력: current transformed image, sRGB fallback 또는 frame이 없으면 null image
QImage QtDisplayPresentationAdapter::presentedFrame(DisplayPresentationSlot slot) const
{
    const CachedFrame& cached = m_frames[slotIndex(slot)];
    return cached.presentationGeneration == m_presentationGeneration ? cached.presented : QImage{};
}

// 목적: 현재 monitor별 color transform 사용 가능 여부 조회
// 입력: 없음
// 출력: valid target ICC profile이 active이면 true
bool QtDisplayPresentationAdapter::hasManagedProfile() const noexcept
{
    return m_fallbackReason == DisplayPresentationFallbackReason::None && !m_profileData.isEmpty();
}

// 목적: current presentation이 sRGB fallback인 이유 조회
// 입력: 없음
// 출력: managed profile이면 None, 아니면 구체적인 discovery/validation failure
DisplayPresentationFallbackReason QtDisplayPresentationAdapter::fallbackReason() const noexcept
{
    return m_fallbackReason;
}

// 목적: current Platform display target identity 조회
// 입력: 없음
// 출력: resolver가 unique target을 찾았으면 identity
const std::optional<platform::DisplayTargetId>& QtDisplayPresentationAdapter::targetId() const noexcept
{
    return m_targetId;
}

// 목적: target/profile state change마다 증가하는 presentation generation 조회
// 입력: 없음
// 출력: 0을 사용하지 않는 process-local generation
std::uint64_t QtDisplayPresentationAdapter::presentationGeneration() const noexcept
{
    return m_presentationGeneration;
}

// 목적: current ICC payload의 stable content hash 조회
// 입력: 없음
// 출력: managed profile이면 SHA-256, fallback이면 빈 byte array
const QByteArray& QtDisplayPresentationAdapter::profileHash() const noexcept
{
    return m_profileHash;
}

// 목적: enum slot을 fixed cache array index로 변환
// 입력: slot: current 또는 comparison surface
// 출력: m_frames의 유효 index
std::size_t QtDisplayPresentationAdapter::slotIndex(DisplayPresentationSlot slot) noexcept
{
    return slot == DisplayPresentationSlot::Current ? 0U : 1U;
}

// 목적: stored source frame을 active profile 또는 sRGB fallback으로 다시 투영
// 입력: slot: 갱신할 cache slot
// 출력: current generation image를 cache에 저장
void QtDisplayPresentationAdapter::reprojectFrame(DisplayPresentationSlot slot)
{
    CachedFrame& cached = m_frames[slotIndex(slot)];
    if (!cached.source.has_value())
    {
        cached.presented = {};
        cached.presentationGeneration = m_presentationGeneration;
        return;
    }

    const QImage neutral = toQImage(*cached.source);
    cached.presented = neutral;
    if (!neutral.isNull() && hasManagedProfile())
    {
        const QImage transformed = transformForDisplay(neutral, m_profileData);
        if (!transformed.isNull())
        {
            cached.presented = transformed;
        }
        else
        {
            m_fallbackReason = DisplayPresentationFallbackReason::TransformFailed;
        }
    }
    cached.presentationGeneration = m_presentationGeneration;
}

// 목적: 두 presentation slot의 retained source를 새 target/profile로 다시 투영
// 입력: 없음
// 출력: current와 comparison cache가 같은 generation으로 갱신됨
void QtDisplayPresentationAdapter::reprojectFrames()
{
    reprojectFrame(DisplayPresentationSlot::Current);
    reprojectFrame(DisplayPresentationSlot::Comparison);
}

}  // namespace flexraw::ui::facade
