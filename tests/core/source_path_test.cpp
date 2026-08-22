#include <gtest/gtest.h>

#include "source_path.h"

namespace flexraw::core::catalog
{
namespace
{

TEST(SourcePathTest, NormalizesFolderSeparatorsAndSegments)
{
    EXPECT_EQ(QStringLiteral("C:/photos/session"),
              normalizeSourceFolderPath(QStringLiteral(" C:\\photos\\session\\.\\ ")));
    EXPECT_EQ(QStringLiteral("/photos/session"),
              normalizeSourceFolderPath(QStringLiteral("/photos/archive/../session")));
    EXPECT_TRUE(normalizeSourceFolderPath(QStringLiteral(" ")).isEmpty());
}

TEST(SourcePathTest, DerivesDrivePosixAndUncParentsLexically)
{
    EXPECT_EQ(QStringLiteral("C:/photos"), sourceParentPath(QStringLiteral("C:\\photos\\image.CR3")));
    EXPECT_EQ(QStringLiteral("C:/"), sourceParentPath(QStringLiteral("C:/image.CR3")));
    EXPECT_EQ(QStringLiteral("/photos"), sourceParentPath(QStringLiteral("/photos/image.CR3")));
    EXPECT_EQ(QStringLiteral("//server/share/photos"),
              sourceParentPath(QStringLiteral("//server/share/photos/image.CR3")));
    EXPECT_TRUE(sourceParentPath(QStringLiteral("image.CR3")).isEmpty());
}

}  // namespace
}  // namespace flexraw::core::catalog
