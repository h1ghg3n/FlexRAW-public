#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "terminal_cli.h"

namespace flexraw::cli
{
namespace
{

// 목적: string literal argv를 platform-native path vector로 변환
// 입력: arguments: executable name을 포함한 ASCII test tokens
// 출력: runTerminalCli()에 전달할 path vector
[[nodiscard]] std::vector<std::filesystem::path> makeArguments(const std::initializer_list<const char*> arguments)
{
    std::vector<std::filesystem::path> result;
    for (const char* argument : arguments)
    {
        result.emplace_back(argument);
    }
    return result;
}

TEST(TerminalCliTest, PrintsHelpToStdout)
{
    const std::vector<std::filesystem::path> arguments = makeArguments({"flexraw-cli", "--help"});
    std::ostringstream output;
    std::ostringstream error;

    const int exitCode = runTerminalCli(arguments, output, error);

    EXPECT_EQ(0, exitCode);
    EXPECT_NE(std::string::npos, output.str().find("Usage:"));
    EXPECT_TRUE(error.str().empty());
}

TEST(TerminalCliTest, RejectsUnknownOrIncompleteCommandsAsUsageErrors)
{
    for (const std::vector<std::filesystem::path>& arguments :
         {makeArguments({"flexraw-cli"}),
          makeArguments({"flexraw-cli", "unknown"}),
          makeArguments({"flexraw-cli", "export", "--input", "missing.raw"}),
          makeArguments({"flexraw-cli", "export", "--input", "a.raw", "--input", "b.raw", "--output", "c.png"})})
    {
        std::ostringstream output;
        std::ostringstream error;
        EXPECT_EQ(2, runTerminalCli(arguments, output, error));
        EXPECT_TRUE(output.str().empty());
        EXPECT_NE(std::string::npos, error.str().find("Usage:"));
    }
}

TEST(TerminalCliTest, ProjectsDecodeFailureToExecutionExitCode)
{
    const std::vector<std::filesystem::path> arguments =
        makeArguments({"flexraw-cli", "export", "--input", "missing.raw", "--output", "result.png"});
    std::ostringstream output;
    std::ostringstream error;

    const int exitCode = runTerminalCli(arguments, output, error);

    EXPECT_EQ(1, exitCode);
    EXPECT_TRUE(output.str().empty());
    EXPECT_NE(std::string::npos, error.str().find("decode"));
}

}  // namespace
}  // namespace flexraw::cli
