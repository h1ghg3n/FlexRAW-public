#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>

#include <QByteArray>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QFileInfo>
#include <QImageReader>
#include <QString>

#include "local_render_executor.h"
#include "measurement_csv.h"
#include "resolved_render_pipeline.h"
#include "stage_timer.h"

namespace
{

// 목적: QString을 CSV record와 stderr에 보존할 UTF-8 standard string으로 변환
// 입력: value: 변환할 Qt text
// 출력: UTF-8 byte sequence
[[nodiscard]] std::string toUtf8(const QString& value)
{
    const QByteArray bytes = value.toUtf8();
    return {bytes.constData(), static_cast<std::size_t>(bytes.size())};
}

// 목적: LocalRenderExecutor로 resolved RAW render 1회를 실행하고 CSV 결과 출력
// 입력: application: --input, --output, --run-id, --target option이 등록될 Qt application
// 출력: 성공 0, 인자 누락 2, render 실패 3
int runSmoke(QCoreApplication& application)
{
    QCommandLineParser parser;
    parser.setApplicationDescription(QCoreApplication::translate(
        "ResolvedRenderSmoke", "Runs one resolved RAW render and writes a versioned CSV record."));
    parser.addHelpOption();

    const QCommandLineOption inputOption(
        {QStringLiteral("i"), QStringLiteral("input")},
        QCoreApplication::translate("ResolvedRenderSmoke", "RAW source path resolved on this machine."),
        QCoreApplication::translate("ResolvedRenderSmoke", "path"));
    const QCommandLineOption outputOption(
        {QStringLiteral("o"), QStringLiteral("output")},
        QCoreApplication::translate("ResolvedRenderSmoke", "JPEG output path resolved on this machine."),
        QCoreApplication::translate("ResolvedRenderSmoke", "path"));
    const QCommandLineOption runIdOption(
        QStringLiteral("run-id"),
        QCoreApplication::translate("ResolvedRenderSmoke", "Stable label for this smoke invocation."),
        QCoreApplication::translate("ResolvedRenderSmoke", "id"),
        QStringLiteral("manual-1"));
    const QCommandLineOption targetOption(
        QStringLiteral("target"),
        QCoreApplication::translate("ResolvedRenderSmoke", "Machine and architecture label recorded in CSV."),
        QCoreApplication::translate("ResolvedRenderSmoke", "name"),
        QStringLiteral("unspecified"));
    parser.addOptions({inputOption, outputOption, runIdOption, targetOption});
    parser.process(application);

    const QString inputPath = parser.value(inputOption);
    const QString outputPath = parser.value(outputOption);
    if (inputPath.trimmed().isEmpty() || outputPath.trimmed().isEmpty())
    {
        std::cerr << toUtf8(QCoreApplication::translate("ResolvedRenderSmoke",
                                                        "Both --input and --output paths are required."))
                  << '\n';
        return 2;
    }

    flexraw::core::render::ResolvedRenderRequest request;
    request.sourcePath = inputPath;
    request.outputPath = outputPath;
    request.outputOptions.includeMetadata = false;

    const flexraw::core::render::ResolvedRenderPipeline pipeline;
    const flexraw::core::render::LocalRenderExecutor executor(pipeline);
    const flexraw::core::types::CancellationSource cancellation;
    const flexraw::core::measurement::StageTimer endToEndTimer;
    const flexraw::core::render::ResolvedRenderPipelineResult result = executor.execute(request, cancellation.token());

    flexraw::core::measurement::RenderMeasurementRecord record;
    record.runId = toUtf8(parser.value(runIdOption));
    record.mode = "local-resolved-render-smoke";
    record.target = toUtf8(parser.value(targetOption));
    record.sourceLabel = toUtf8(QFileInfo(inputPath).fileName());
    record.sampleIndex = 1;
    record.succeeded = result.hasValue();
    record.render = result.hasValue() ? result.value().stats : result.error().stats;
    record.endToEndNanoseconds = endToEndTimer.elapsedNanoseconds();

    if (result.hasValue())
    {
        QImageReader outputReader(result.value().artifact.outputPath);
        const QSize outputSize = outputReader.size();
        record.width = static_cast<std::uint32_t>(std::max(outputSize.width(), 0));
        record.height = static_cast<std::uint32_t>(std::max(outputSize.height(), 0));
    }

    flexraw::core::measurement::writeRenderMeasurementCsvHeader(std::cout);
    flexraw::core::measurement::writeRenderMeasurementCsvRow(std::cout, record);
    if (result.hasError())
    {
        std::cerr << toUtf8(result.error().cause.message) << '\n';
        return 3;
    }
    return 0;
}

}  // namespace

// 목적: resolved render smoke용 Qt Core runtime 생성과 local executor 실행
// 입력: argc/argv: benchmark command-line arguments
// 출력: smoke render 성공 또는 실패 process exit code
int main(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("Flexraw Resolved Render Smoke"));
    return runSmoke(application);
}
