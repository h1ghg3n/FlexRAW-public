#pragma once

#include <QCoreApplication>
#include <QString>

#include "export_options.h"

namespace flexraw::ui::cli
{

struct RasterExportCommand
{
    QString inputPath;
    QString outputPath;
    QString catalogPath;
    core::export_::RasterExportOptions options;
};

struct RasterExportCommandParseResult
{
    bool recognized{false};
    bool valid{false};
    RasterExportCommand command;
    QString errorMessage;
};

struct RasterExportCommandExecutionResult
{
    bool succeeded{false};
    QString outputPath;
    QString errorMessage;
};

struct BatchExportCommand
{
    QString inputFolderPath;
    QString outputFolderPath;
    QString catalogPath;
    int workerCount{0};
    core::export_::RasterExportOptions options;
};

struct BatchExportCommandParseResult
{
    bool recognized{false};
    bool valid{false};
    BatchExportCommand command;
    QString errorMessage;
};

struct BatchExportCommandExecutionResult
{
    bool completed{false};
    int totalCount{0};
    int succeededCount{0};
    int failedCount{0};
    QString firstError;
};

class ExportCommandService final
{
    Q_DECLARE_TR_FUNCTIONS(ExportCommandService)

public:
    // 목적: console 입력에서 raster export command와 option을 분석
    // 입력: commandLine: 사용자가 입력한 전체 command 문자열
    // 출력: command 인식 여부, 검증 결과와 export 인자
    [[nodiscard]] static RasterExportCommandParseResult parseRasterCommand(
        const QString& commandLine, const core::export_::RasterExportOptions& defaultOptions = {});

    // 목적: console 입력에서 기본 develop parameter를 사용하는 RAW export command를 분석
    // 입력: commandLine: 사용자가 입력한 전체 command 문자열
    // 출력: command 인식 여부, 검증 결과와 export 인자
    [[nodiscard]] static RasterExportCommandParseResult parseRawCommand(
        const QString& commandLine, const core::export_::RasterExportOptions& defaultOptions = {});

    // 목적: console 입력에서 병렬 folder export command와 option을 분석
    // 입력: commandLine: 사용자가 입력한 전체 command 문자열
    // 출력: command 인식 여부, 검증 결과와 batch export 인자
    [[nodiscard]] static BatchExportCommandParseResult parseBatchCommand(
        const QString& commandLine, const core::export_::RasterExportOptions& defaultOptions = {});

    // 목적: raster input file을 sRGB로 정규화해 지정 export option으로 저장
    // 입력: command: 입력/출력 경로와 raster export option
    // 출력: 완료된 출력 경로 또는 image decode/export 실패 정보
    [[nodiscard]] static RasterExportCommandExecutionResult exportRaster(const RasterExportCommand& command);

    // 목적: RAW input file을 full decode해 기본 develop parameter와 지정 export option으로 저장
    // 입력: command: 입력/출력 경로와 raster export option
    // 출력: 완료된 출력 경로 또는 RAW decode/develop/export 실패 정보
    [[nodiscard]] static RasterExportCommandExecutionResult exportRaw(const RasterExportCommand& command);

    // 목적: 지원 input folder를 worker pool로 병렬 처리해 지정 output folder에 저장
    // 입력: command: input/output folder, worker 수, catalog과 raster export option
    // 출력: 완료 여부와 전체 성공/실패 집계 또는 준비 단계 실패 정보
    [[nodiscard]] static BatchExportCommandExecutionResult exportBatch(const BatchExportCommand& command);
};

}  // namespace flexraw::ui::cli
