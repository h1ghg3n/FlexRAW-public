#pragma once

#include <QCoreApplication>
#include <QString>

#include "raw_file_reader.h"

namespace flexraw::ui::cli
{

struct RawDiagnosticsCommand
{
    QString inputPath;
};

struct RawDiagnosticsCommandParseResult
{
    bool recognized{false};
    bool valid{false};
    RawDiagnosticsCommand command;
    QString errorMessage;
};

struct RawDiagnosticsExecutionResult
{
    bool succeeded{false};
    core::raw::RawDecodeDiagnostics diagnostics;
    QString errorMessage;
};

class RawDiagnosticService final
{
    Q_DECLARE_TR_FUNCTIONS(RawDiagnosticService)

public:
    // 목적: console 입력에서 RAW decode diagnostics command를 분석
    // 입력: commandLine: 사용자가 입력한 전체 command 문자열
    // 출력: command 인식 및 검증 결과
    [[nodiscard]] static RawDiagnosticsCommandParseResult parseRawCommand(const QString& commandLine);

    // 목적: RAW decode diagnostics를 수집
    // 입력: command: 진단할 RAW file 경로
    // 출력: orientation과 decode 크기 또는 구조화된 오류
    [[nodiscard]] static RawDiagnosticsExecutionResult inspectRaw(const RawDiagnosticsCommand& command);
};

}  // namespace flexraw::ui::cli
