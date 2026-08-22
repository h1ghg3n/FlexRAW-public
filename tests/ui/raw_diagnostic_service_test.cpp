#include <gtest/gtest.h>

#include "raw_diagnostic_service.h"

namespace flexraw::ui::cli
{
namespace
{

TEST(RawDiagnosticServiceTest, ParsesQuotedRawInput)
{
    const RawDiagnosticsCommandParseResult parsed =
        RawDiagnosticService::parseRawCommand(QStringLiteral("diagnose raw --input \"D:\\Photos\\sample.raw\""));

    ASSERT_TRUE(parsed.recognized);
    ASSERT_TRUE(parsed.valid);
    EXPECT_EQ(QStringLiteral("D:\\Photos\\sample.raw"), parsed.command.inputPath);
}

TEST(RawDiagnosticServiceTest, JoinsLineWrappedQuotedRawInput)
{
    const RawDiagnosticsCommandParseResult parsed = RawDiagnosticService::parseRawCommand(
        QStringLiteral("diagnose raw --input \"D:\\Photos\\very-\n  long-sample.raw\""));

    ASSERT_TRUE(parsed.recognized);
    ASSERT_TRUE(parsed.valid);
    EXPECT_EQ(QStringLiteral("D:\\Photos\\very-long-sample.raw"), parsed.command.inputPath);
}

TEST(RawDiagnosticServiceTest, RejectsMissingInput)
{
    const RawDiagnosticsCommandParseResult parsed = RawDiagnosticService::parseRawCommand(QStringLiteral("diagnose raw"));

    EXPECT_TRUE(parsed.recognized);
    EXPECT_FALSE(parsed.valid);
}

}  // namespace
}  // namespace flexraw::ui::cli
