#include <gtest/gtest.h>

#include "source_path.h"

namespace flexraw::core::catalog
{
namespace
{

TEST(SourcePathTest, NormalizesFolderSeparatorsAndSegments)
{
    EXPECT_EQ(QStringLiteral("C:/photos/session"), normalizeSourceFolderPath(QStringLiteral("C:\\photos\\session\\.")));
    EXPECT_EQ(QStringLiteral("/photos/session"),
              normalizeSourceFolderPath(QStringLiteral("/photos/archive/../session")));
    EXPECT_TRUE(normalizeSourceFolderPath(QStringLiteral(" ")).isEmpty());
}

TEST(SourcePathTest, RejectsOuterWhitespaceInsteadOfChangingTheLocator)
{
    EXPECT_TRUE(normalizeSourceFolderPath(QStringLiteral(" /photos/session")).isEmpty());
    EXPECT_TRUE(normalizeSourceFolderPath(QStringLiteral("/photos/session ")).isEmpty());
    EXPECT_TRUE(sourceParentPath(QStringLiteral(" /photos/image.CR3")).isEmpty());
    EXPECT_TRUE(sourceParentPath(QStringLiteral("/photos/image.CR3 ")).isEmpty());
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

// 목적: host OS와 무관하게 drive root와 UNC locator 정규화의 멱등성 검증
// 입력: drive root와 UNC separator를 포함한 lexical path
// 출력: root를 보존하고 반복 정규화해도 동일한 path
TEST(SourcePathTest, PreservesPortableRootsAcrossRepeatedNormalization)
{
    const QString driveRoot = normalizeSourceFolderPath(QStringLiteral("C:/photos/.."));
    EXPECT_EQ(QStringLiteral("C:/"), driveRoot);
    EXPECT_EQ(driveRoot, normalizeSourceFolderPath(driveRoot));
    EXPECT_EQ(QStringLiteral("C:/image.CR3"), normalizeSourceFolderPath(QStringLiteral("C:/photos/../image.CR3")));
    const QString uncFolder = normalizeSourceFolderPath(QStringLiteral("\\\\server\\share\\photos"));
    EXPECT_EQ(QStringLiteral("//server/share/photos"), uncFolder);
    EXPECT_EQ(uncFolder, normalizeSourceFolderPath(uncFolder));
    EXPECT_EQ(QStringLiteral("/"), sourceParentPath(QStringLiteral("/image.CR3")));
}

}  // namespace
}  // namespace flexraw::core::catalog
