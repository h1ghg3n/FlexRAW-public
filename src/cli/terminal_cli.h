#pragma once

#include <filesystem>
#include <iosfwd>
#include <span>

namespace flexraw::cli
{

// 목적: parsed platform-native argv를 최소 Terminal CLI command로 실행
// 입력: arguments: executable name을 포함한 argv path, output/error: stdout/stderr destination
// 출력: 성공 0, usage 오류 2, processing 또는 I/O 실패 1
[[nodiscard]] int runTerminalCli(std::span<const std::filesystem::path> arguments,
                                 std::ostream& output,
                                 std::ostream& error);

}  // namespace flexraw::cli
