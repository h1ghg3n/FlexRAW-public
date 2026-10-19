#include <vector>

#include <gtest/gtest.h>

#include "windows_display_target_resolver.h"

namespace flexraw::platform
{
namespace
{

// 목적: resolver test용 display target snapshot 생성
// 입력: id: opaque target identity
// 출력: deterministic desktop metadata를 포함한 snapshot
[[nodiscard]] DisplayTargetSnapshot target(std::string id)
{
    return {{std::move(id)}, {0, 0, 1920, 1080}, false};
}

TEST(WindowsDisplayTargetResolverTest, MatchesOneExactNativeDeviceIdentity)
{
    const std::vector targets{target("display-a"), target("display-b")};

    const std::optional<DisplayTargetId> matched = matchWindowsDisplayTargetId("display-b", targets);

    ASSERT_TRUE(matched.has_value());
    EXPECT_EQ("display-b", matched->bytes);
}

TEST(WindowsDisplayTargetResolverTest, RejectsUnknownOrAmbiguousNativeDeviceIdentity)
{
    const std::vector targets{target("display-a"), target("display-a")};

    EXPECT_FALSE(matchWindowsDisplayTargetId("display-a", targets).has_value());
    EXPECT_FALSE(matchWindowsDisplayTargetId("display-b", targets).has_value());
}

}  // namespace
}  // namespace flexraw::platform
