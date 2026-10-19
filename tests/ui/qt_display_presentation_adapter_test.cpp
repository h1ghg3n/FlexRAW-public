#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include <QColor>
#include <QColorSpace>
#include <QGuiApplication>
#include <QImage>
#include <QScreen>

#include <gtest/gtest.h>

#include "display_frame.h"
#include "qt_display_presentation_adapter.h"

namespace flexraw::ui::facade
{
namespace
{

class FakeDisplayProfileService final : public platform::IDisplayProfileService
{
public:
    std::vector<platform::DisplayTargetSnapshot> targets;
    std::unordered_map<std::string, platform::DisplayProfileResult> profiles;

    // 목적: test가 구성한 active display target snapshot 반환
    // 입력: 없음
    // 출력: deterministic target 목록
    [[nodiscard]] std::optional<std::vector<platform::DisplayTargetSnapshot>> displayTargets() const override
    {
        return targets;
    }

    // 목적: target identity별 test profile result 반환
    // 입력: targetId: 조회할 fake display identity
    // 출력: configured profile 또는 TargetUnavailable
    [[nodiscard]] platform::DisplayProfileResult displayProfile(
        const platform::DisplayTargetId& targetId) const override
    {
        const auto profile = profiles.find(targetId.bytes);
        return profile != profiles.end() ? profile->second
                                         : platform::DisplayProfileResult(platform::DisplayProfileUnavailable{
                                               platform::DisplayProfileUnavailableReason::TargetUnavailable});
    }
};

// 목적: Qt standard color space의 ICC payload를 Platform byte snapshot으로 변환
// 입력: colorSpace: ICC profile을 제공하는 Qt color space
// 출력: owned profile byte
[[nodiscard]] std::vector<std::uint8_t> profileBytes(const QColorSpace& colorSpace)
{
    const QByteArray profile = colorSpace.iccProfile();
    return {reinterpret_cast<const std::uint8_t*>(profile.constData()),
            reinterpret_cast<const std::uint8_t*>(profile.constData()) + profile.size()};
}

// 목적: adapter test용 opaque BGRA8 sRGB frame 생성
// 입력: red/green/blue: pixel channel, revision: frame identity
// 출력: 1x1 immutable display frame
[[nodiscard]] core::client::DisplayFrame frame(std::uint8_t red,
                                               std::uint8_t green,
                                               std::uint8_t blue,
                                               std::uint64_t revision)
{
    return *core::client::DisplayFrame::create(1, 1, 4, revision, {blue, green, red, 255});
}

// 목적: headless Qt test platform의 current screen 반환
// 입력: 없음
// 출력: application primary screen
[[nodiscard]] QScreen* currentScreen()
{
    QScreen* screen = QGuiApplication::primaryScreen();
    EXPECT_NE(nullptr, screen);
    return screen;
}

TEST(QtDisplayPresentationAdapterTest, AppliesValidProfileAndCachesCurrentGeneration)
{
    FakeDisplayProfileService service;
    service.targets = {{{"display-a"}, {0, 0, 1920, 1080}, true}};
    service.profiles.emplace(
        "display-a", platform::DisplayProfileSnapshot{{"display-a"}, profileBytes(QColorSpace(QColorSpace::SRgb))});
    std::optional<platform::DisplayTargetId> selected = platform::DisplayTargetId{"display-a"};
    QtDisplayPresentationAdapter adapter(
        &service, [&selected](const QScreen&, const platform::IDisplayProfileService&) { return selected; });

    ASSERT_TRUE(adapter.setScreen(currentScreen()));
    ASSERT_TRUE(adapter.hasManagedProfile());
    EXPECT_FALSE(adapter.profileHash().isEmpty());
    const std::uint64_t generation = adapter.presentationGeneration();

    const QImage presented = adapter.presentFrame(DisplayPresentationSlot::Current, frame(41, 82, 123, 7));

    ASSERT_FALSE(presented.isNull());
    EXPECT_EQ(QColor(41, 82, 123), presented.pixelColor(0, 0));
    EXPECT_EQ(generation, adapter.presentationGeneration());
    EXPECT_EQ(presented, adapter.presentedFrame(DisplayPresentationSlot::Current));
}

TEST(QtDisplayPresentationAdapterTest, TargetChangeReprojectsRetainedCurrentAndComparisonFrames)
{
    FakeDisplayProfileService service;
    service.targets = {{{"display-a"}, {0, 0, 1920, 1080}, true}, {{"display-b"}, {1920, 0, 3840, 1080}, false}};
    const std::vector<std::uint8_t> srgbProfile = profileBytes(QColorSpace(QColorSpace::SRgb));
    service.profiles.emplace("display-a", platform::DisplayProfileSnapshot{{"display-a"}, srgbProfile});
    service.profiles.emplace("display-b", platform::DisplayProfileSnapshot{{"display-b"}, srgbProfile});
    std::optional<platform::DisplayTargetId> selected = platform::DisplayTargetId{"display-a"};
    QtDisplayPresentationAdapter adapter(
        &service, [&selected](const QScreen&, const platform::IDisplayProfileService&) { return selected; });
    ASSERT_TRUE(adapter.setScreen(currentScreen()));
    const QImage current = adapter.presentFrame(DisplayPresentationSlot::Current, frame(20, 40, 60, 1));
    const QImage comparison = adapter.presentFrame(DisplayPresentationSlot::Comparison, frame(90, 70, 50, 2));
    const std::uint64_t firstGeneration = adapter.presentationGeneration();

    selected = platform::DisplayTargetId{"display-b"};
    ASSERT_TRUE(adapter.setScreen(currentScreen()));

    EXPECT_GT(adapter.presentationGeneration(), firstGeneration);
    EXPECT_EQ("display-b", adapter.targetId()->bytes);
    EXPECT_EQ(current, adapter.presentedFrame(DisplayPresentationSlot::Current));
    EXPECT_EQ(comparison, adapter.presentedFrame(DisplayPresentationSlot::Comparison));
}

TEST(QtDisplayPresentationAdapterTest, InvalidProfileFailsOpenToNeutralSrgbFrame)
{
    FakeDisplayProfileService service;
    service.targets = {{{"display-a"}, {0, 0, 1920, 1080}, true}};
    service.profiles.emplace("display-a", platform::DisplayProfileSnapshot{{"display-a"}, {1, 2, 3, 4}});
    QtDisplayPresentationAdapter adapter(&service, [](const QScreen&, const platform::IDisplayProfileService&) {
        return std::optional<platform::DisplayTargetId>(platform::DisplayTargetId{"display-a"});
    });

    ASSERT_TRUE(adapter.setScreen(currentScreen()));
    EXPECT_FALSE(adapter.hasManagedProfile());
    EXPECT_EQ(DisplayPresentationFallbackReason::InvalidProfile, adapter.fallbackReason());

    const QImage presented = adapter.presentFrame(DisplayPresentationSlot::Current, frame(11, 22, 33, 3));
    ASSERT_FALSE(presented.isNull());
    EXPECT_EQ(QColor(11, 22, 33), presented.pixelColor(0, 0));
}

TEST(QtDisplayPresentationAdapterTest, ResolveFailureClearsPreviousTargetAndKeepsSrgbFallback)
{
    FakeDisplayProfileService service;
    service.targets = {{{"display-a"}, {0, 0, 1920, 1080}, true}};
    service.profiles.emplace(
        "display-a", platform::DisplayProfileSnapshot{{"display-a"}, profileBytes(QColorSpace(QColorSpace::SRgb))});
    std::optional<platform::DisplayTargetId> selected = platform::DisplayTargetId{"display-a"};
    QtDisplayPresentationAdapter adapter(
        &service, [&selected](const QScreen&, const platform::IDisplayProfileService&) { return selected; });
    ASSERT_TRUE(adapter.setScreen(currentScreen()));
    (void)adapter.presentFrame(DisplayPresentationSlot::Current, frame(44, 55, 66, 4));

    selected.reset();
    ASSERT_TRUE(adapter.setScreen(currentScreen()));

    EXPECT_FALSE(adapter.targetId().has_value());
    EXPECT_FALSE(adapter.hasManagedProfile());
    EXPECT_EQ(DisplayPresentationFallbackReason::TargetResolveFailed, adapter.fallbackReason());
    EXPECT_EQ(QColor(44, 55, 66), adapter.presentedFrame(DisplayPresentationSlot::Current).pixelColor(0, 0));
}

}  // namespace
}  // namespace flexraw::ui::facade
