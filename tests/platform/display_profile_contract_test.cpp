#include <cstdint>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "display_profile_service.h"

namespace flexraw::platform
{
namespace
{

TEST(DisplayProfileContractTest, KeepsTargetIdentityAndPayloadOwnedBySnapshot)
{
    DisplayTargetId targetId{"display-a"};
    DisplayProfileResult result = DisplayProfileSnapshot{targetId, std::vector<std::uint8_t>{1, 2, 3, 4}};

    ASSERT_TRUE(std::holds_alternative<DisplayProfileSnapshot>(result));
    const DisplayProfileSnapshot& snapshot = std::get<DisplayProfileSnapshot>(result);
    EXPECT_EQ(targetId, snapshot.targetId);
    EXPECT_EQ((std::vector<std::uint8_t>{1, 2, 3, 4}), snapshot.iccProfile);
    EXPECT_LE(snapshot.iccProfile.size(), MaximumDisplayProfileBytes);
}

TEST(DisplayProfileContractTest, RepresentsExplicitFallbackWithoutSyntheticPayload)
{
    DisplayProfileResult result = DisplayProfileUnavailable{DisplayProfileUnavailableReason::NoExplicitProfile};

    ASSERT_TRUE(std::holds_alternative<DisplayProfileUnavailable>(result));
    EXPECT_EQ(DisplayProfileUnavailableReason::NoExplicitProfile, std::get<DisplayProfileUnavailable>(result).reason);
}

static_assert(!std::is_polymorphic_v<DisplayTargetSnapshot>);
static_assert(!std::is_polymorphic_v<DisplayProfileSnapshot>);

}  // namespace
}  // namespace flexraw::platform
