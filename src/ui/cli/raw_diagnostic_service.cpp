#include "raw_diagnostic_service.h"

#include <QRegularExpression>

#include <utility>

namespace flexraw::ui::cli
{
namespace
{

// 목적: 따옴표 안의 줄바꿈과 들여쓰기를 제거해 Windows path continuation을 연결
// 입력: commandLine: 정규화할 전체 console command
// 출력: 줄바꿈 continuation이 제거된 command 문자열
[[nodiscard]] QString joinQuotedPathContinuations(const QString& commandLine)
{
    QString normalized;
    bool inQuotes = false;
    bool skipContinuationIndent = false;

    for (const QChar character : commandLine)
    {
        if (inQuotes && (character == QLatin1Char('\r') || character == QLatin1Char('\n')))
        {
            skipContinuationIndent = true;
            continue;
        }

        if (inQuotes && skipContinuationIndent && (character == QLatin1Char(' ') || character == QLatin1Char('\t')))
        {
            continue;
        }

        skipContinuationIndent = false;
        normalized.append(character);
        if (character == QLatin1Char('"'))
        {
            inQuotes = !inQuotes;
        }
    }

    return normalized;
}

// 목적: RAW diagnostics command parsing 실패 결과 생성
// 입력: message: console에 표시할 검증 오류
// 출력: 인식된 command의 실패 결과
[[nodiscard]] RawDiagnosticsCommandParseResult parseFailure(QString message)
{
    RawDiagnosticsCommandParseResult result;
    result.recognized = true;
    result.errorMessage = std::move(message);
    return result;
}

}  // namespace

// 목적: console 입력에서 RAW decode diagnostics command를 분석
// 입력: commandLine: 사용자가 입력한 전체 command 문자열
// 출력: command 인식 및 검증 결과
RawDiagnosticsCommandParseResult RawDiagnosticService::parseRawCommand(const QString& commandLine)
{
    const QString normalizedCommand = joinQuotedPathContinuations(commandLine).trimmed();
    if (!normalizedCommand.startsWith(QStringLiteral("diagnose"), Qt::CaseInsensitive))
    {
        return {};
    }

    const QRegularExpression pattern(
        QStringLiteral(R"raw(^diagnose\s+raw\s+--input\s+"([^"]+)"\s*$)raw"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = pattern.match(normalizedCommand);
    if (!match.hasMatch())
    {
        return parseFailure(tr("Usage: diagnose raw --input \"<raw-file>\""));
    }

    RawDiagnosticsCommandParseResult result;
    result.recognized = true;
    result.valid = true;
    result.command.inputPath = match.captured(1);
    return result;
}

// 목적: RAW decode diagnostics를 수집
// 입력: command: 진단할 RAW file 경로
// 출력: orientation과 decode 크기 또는 구조화된 오류
RawDiagnosticsExecutionResult RawDiagnosticService::inspectRaw(const RawDiagnosticsCommand& command)
{
    const core::raw::RawDecodeDiagnosticsResult diagnostics = core::raw::inspectRawDecode(command.inputPath);
    if (diagnostics.hasError())
    {
        return {false, {}, diagnostics.error().message};
    }

    return {true, diagnostics.value(), {}};
}

}  // namespace flexraw::ui::cli
