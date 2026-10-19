#include "terminal_cli.h"

#include <optional>
#include <ostream>
#include <string>
#include <string_view>

#include "portable_png_render_pipeline.h"

namespace flexraw::cli
{
namespace
{

constexpr int SuccessExitCode = 0;
constexpr int ExecutionFailureExitCode = 1;
constexpr int UsageFailureExitCode = 2;

struct ExportArguments
{
    std::filesystem::path inputPath;
    std::filesystem::path outputPath;
};

// 목적: fixed Terminal CLI usage text 출력
// 입력: output: help 또는 usage error stream
// 출력: usage line을 stream에 기록
void writeUsage(std::ostream& output)
{
    output << "Usage: flexraw-cli export --input <raw> --output <png>\n";
}

// 목적: filesystem path를 UTF-8 bytes로 stream에 기록
// 입력: output: destination stream, path: platform-native path
// 출력: UTF-8 path bytes를 stream에 기록
void writeUtf8Path(std::ostream& output, const std::filesystem::path& path)
{
    const std::u8string utf8 = path.u8string();
    output.write(reinterpret_cast<const char*>(utf8.data()), static_cast<std::streamsize>(utf8.size()));
}

// 목적: command token이 ASCII option text와 같은지 비교
// 입력: argument: platform-native argv token, expected: ASCII command 또는 option
// 출력: exact match이면 true
[[nodiscard]] bool matches(const std::filesystem::path& argument, const std::string_view expected)
{
    return argument == std::filesystem::path(expected);
}

// 목적: export command의 exact input/output option pair parsing
// 입력: arguments: export 다음 option tokens
// 출력: required option이 한 번씩 있으면 typed request, 아니면 빈 값
[[nodiscard]] std::optional<ExportArguments> parseExportArguments(
    const std::span<const std::filesystem::path> arguments)
{
    ExportArguments parsed;
    bool hasInput = false;
    bool hasOutput = false;
    for (std::size_t index = 0; index < arguments.size();)
    {
        if (index + 1U >= arguments.size())
        {
            return std::nullopt;
        }

        if (matches(arguments[index], "--input") && !hasInput)
        {
            parsed.inputPath = arguments[index + 1U];
            hasInput = true;
        }
        else if (matches(arguments[index], "--output") && !hasOutput)
        {
            parsed.outputPath = arguments[index + 1U];
            hasOutput = true;
        }
        else
        {
            return std::nullopt;
        }
        index += 2U;
    }

    if (!hasInput || !hasOutput || parsed.inputPath.empty() || parsed.outputPath.empty())
    {
        return std::nullopt;
    }
    return parsed;
}

// 목적: pipeline stage를 stable diagnostic ASCII label로 변환
// 입력: stage: 실패한 portable render stage
// 출력: stderr용 stage label
[[nodiscard]] std::string_view stageName(const core::render::PortablePngRenderStage stage) noexcept
{
    switch (stage)
    {
    case core::render::PortablePngRenderStage::Decode:
        return "decode";
    case core::render::PortablePngRenderStage::SourceBridge:
        return "source-bridge";
    case core::render::PortablePngRenderStage::Develop:
        return "develop";
    case core::render::PortablePngRenderStage::Color:
        return "color";
    case core::render::PortablePngRenderStage::Encode:
        return "encode";
    case core::render::PortablePngRenderStage::Publish:
        return "publish";
    }
    return "unknown";
}

}  // namespace

int runTerminalCli(const std::span<const std::filesystem::path> arguments, std::ostream& output, std::ostream& error)
{
    if (arguments.size() == 2U && (matches(arguments[1], "--help") || matches(arguments[1], "-h")))
    {
        writeUsage(output);
        return SuccessExitCode;
    }
    if (arguments.size() < 2U || !matches(arguments[1], "export"))
    {
        error << "Unknown or missing command.\n";
        writeUsage(error);
        return UsageFailureExitCode;
    }
    if (arguments.size() == 3U && (matches(arguments[2], "--help") || matches(arguments[2], "-h")))
    {
        writeUsage(output);
        return SuccessExitCode;
    }

    const std::optional<ExportArguments> parsed = parseExportArguments(arguments.subspan(2U));
    if (!parsed.has_value())
    {
        error << "export requires exactly one --input and one --output option.\n";
        writeUsage(error);
        return UsageFailureExitCode;
    }

    core::render::PortablePngRenderRequest request;
    request.inputPath = parsed->inputPath;
    request.outputPath = parsed->outputPath;
    const core::render::PortablePngRenderResult result = core::render::renderRawToPortablePng(request);
    if (result.hasError())
    {
        error << "Flexraw export failed at " << stageName(result.error().stage) << " stage.\n";
        return ExecutionFailureExitCode;
    }

    writeUtf8Path(output, result.value().path);
    output << '\n';
    return SuccessExitCode;
}

}  // namespace flexraw::cli
