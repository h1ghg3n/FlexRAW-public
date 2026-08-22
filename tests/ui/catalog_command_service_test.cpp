#include <gtest/gtest.h>

#include "catalog_command_service.h"

namespace flexraw::ui::cli
{
namespace
{

TEST(CatalogCommandServiceTest, ParsesQuotedWindowsAndUncPaths)
{
    const CatalogImportCommandParseResult parsed = CatalogCommandService::parseImportCommand(
        QStringLiteral("catalog import --catalog \"C:\\Photo Catalog\\library.db\" "
                       "--folder \"\\\\server\\share\\Photo Folder\""));

    ASSERT_TRUE(parsed.recognized);
    ASSERT_TRUE(parsed.valid);
    EXPECT_EQ(QStringLiteral("C:\\Photo Catalog\\library.db"), parsed.command.catalogPath);
    EXPECT_EQ(QStringLiteral("\\\\server\\share\\Photo Folder"), parsed.command.folderPath);
}

TEST(CatalogCommandServiceTest, RejectsMissingAndUnknownOptions)
{
    const CatalogImportCommandParseResult missing =
        CatalogCommandService::parseImportCommand(QStringLiteral("catalog import --catalog library.db"));
    const CatalogImportCommandParseResult unknown = CatalogCommandService::parseImportCommand(
        QStringLiteral("catalog import --catalog library.db --source photos"));

    EXPECT_TRUE(missing.recognized);
    EXPECT_FALSE(missing.valid);
    EXPECT_TRUE(unknown.recognized);
    EXPECT_FALSE(unknown.valid);
}

TEST(CatalogCommandServiceTest, LeavesUnrelatedCommandsUnrecognized)
{
    const CatalogImportCommandParseResult parsed = CatalogCommandService::parseImportCommand(QStringLiteral("help"));

    EXPECT_FALSE(parsed.recognized);
}

}  // namespace
}  // namespace flexraw::ui::cli
