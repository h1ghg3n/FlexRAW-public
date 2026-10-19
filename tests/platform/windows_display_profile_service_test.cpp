#include <optional>
#include <set>
#include <string>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "windows_display_profile_service.h"

namespace flexraw::platform
{
namespace
{

TEST(WindowsDisplayProfileServiceTest, EnumeratesUniqueActiveTargetsAndReturnsBoundedProfileState)
{
    WindowsDisplayProfileService service;
    const std::optional<std::vector<DisplayTargetSnapshot>> targets = service.displayTargets();
    ASSERT_TRUE(targets.has_value());
    if (targets->empty())
    {
        GTEST_SKIP() << "The current Windows session has no active display target.";
    }

    std::set<std::string> identities;
    std::size_t primaryCount = 0;
    for (const DisplayTargetSnapshot& target : *targets)
    {
        EXPECT_FALSE(target.id.bytes.empty());
        EXPECT_LT(target.desktopBounds.left, target.desktopBounds.right);
        EXPECT_LT(target.desktopBounds.top, target.desktopBounds.bottom);
        EXPECT_TRUE(identities.insert(target.id.bytes).second);
        primaryCount += target.primary ? 1U : 0U;

        const DisplayProfileResult profile = service.displayProfile(target.id);
        if (const auto* snapshot = std::get_if<DisplayProfileSnapshot>(&profile))
        {
            EXPECT_EQ(target.id, snapshot->targetId);
            EXPECT_FALSE(snapshot->iccProfile.empty());
            EXPECT_LE(snapshot->iccProfile.size(), MaximumDisplayProfileBytes);
        }
    }
    EXPECT_EQ(1U, primaryCount);
}

TEST(WindowsDisplayProfileServiceTest, RejectsIdentityThatWasNotEnumerated)
{
    WindowsDisplayProfileService service;
    const DisplayProfileResult profile = service.displayProfile(DisplayTargetId{"not-an-enumerated-display"});

    ASSERT_TRUE(std::holds_alternative<DisplayProfileUnavailable>(profile));
    EXPECT_EQ(DisplayProfileUnavailableReason::TargetUnavailable, std::get<DisplayProfileUnavailable>(profile).reason);
}

}  // namespace
}  // namespace flexraw::platform
