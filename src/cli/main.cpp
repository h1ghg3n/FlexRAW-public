#include <filesystem>
#include <iostream>
#include <vector>

#include "terminal_cli.h"

namespace
{

#if defined(_WIN32)
// 목적: Windows wide argv를 lossless platform-native path vector로 변환
// 입력: argc/argv: CRT가 제공한 UTF-16 command line tokens
// 출력: executable name을 포함한 filesystem path vector
[[nodiscard]] std::vector<std::filesystem::path> nativeArguments(const int argc, wchar_t* argv[])
#else
// 목적: POSIX argv를 platform-native path vector로 변환
// 입력: argc/argv: process locale byte command line tokens
// 출력: executable name을 포함한 filesystem path vector
[[nodiscard]] std::vector<std::filesystem::path> nativeArguments(const int argc, char* argv[])
#endif
{
    std::vector<std::filesystem::path> arguments;
    arguments.reserve(static_cast<std::size_t>(argc));
    for (int index = 0; index < argc; ++index)
    {
        arguments.emplace_back(argv[index]);
    }
    return arguments;
}

}  // namespace

#if defined(_WIN32)
// 목적: UTF-16 Windows command line로 Qt-free Terminal CLI 실행
// 입력: argc/argv: platform process arguments
// 출력: export 성공, usage 오류 또는 processing 실패 exit code
int wmain(const int argc, wchar_t* argv[])
#else
// 목적: POSIX command line로 Qt-free Terminal CLI 실행
// 입력: argc/argv: platform process arguments
// 출력: export 성공, usage 오류 또는 processing 실패 exit code
int main(const int argc, char* argv[])
#endif
{
    const std::vector<std::filesystem::path> arguments = nativeArguments(argc, argv);
    return flexraw::cli::runTerminalCli(arguments, std::cout, std::cerr);
}
