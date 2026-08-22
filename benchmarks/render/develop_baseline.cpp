#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>

#include <QByteArray>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QString>

#include "develop.h"
#include "measurement_csv.h"
#include "stage_timer.h"

namespace
{

// 목적: 현재 CPU develop 경로의 주요 보정 stage를 실행하는 대표 parameter 생성
// 입력: 없음
// 출력: Core 허용 범위 안의 비기본 DevelopParams
[[nodiscard]] flexraw::core::types::DevelopParams makeRepresentativeParams()
{
    flexraw::core::types::DevelopParams params;
    params.exposureEv = 0.35F;
    params.contrast = 0.20F;
    params.highlights = -0.25F;
    params.shadows = 0.25F;
    params.whites = 0.10F;
    params.blacks = -0.10F;
    params.saturation = 0.10F;
    params.vibrance = 0.20F;
    params.whiteBalanceMode = flexraw::core::types::WhiteBalanceMode::Custom;
    params.whiteBalanceTemperatureKelvin = 6200.0F;
    params.whiteBalanceTint = 0.05F;
    params.clarity = 0.15F;
    params.dehaze = 0.10F;
    params.sharpeningAmount = 0.25F;
    params.sharpeningRadius = 1.2F;
    params.sharpeningDetail = 0.20F;
    params.sharpeningMasking = 0.10F;
    params.luminanceNoiseReduction = 0.15F;
    params.colorNoiseReduction = 0.15F;
    params.toneCurveShadows = -0.05F;
    params.toneCurveDarks = -0.05F;
    params.toneCurveLights = 0.05F;
    params.toneCurveHighlights = 0.05F;
    return params;
}

// 목적: QString을 CSV record에 보존할 UTF-8 standard string으로 변환
// 입력: value: 변환할 Qt text
// 출력: UTF-8 byte sequence
[[nodiscard]] std::string toUtf8(const QString& value)
{
    const QByteArray bytes = value.toUtf8();
    return {bytes.constData(), static_cast<std::size_t>(bytes.size())};
}

// 목적: CPU develop baseline 인자 해석, 입력 decode와 1회 측정 결과 출력
// 입력: argc/argv: --input, --run-id, --target command-line option
// 출력: 성공 0, 인자·decode·develop 실패 시 non-zero process code
int runBaseline(QCoreApplication& application)
{
    QCommandLineParser parser;
    parser.setApplicationDescription(QCoreApplication::translate(
        "DevelopBaseline", "Runs one CPU develop measurement and writes a versioned CSV record."));
    parser.addHelpOption();

    const QCommandLineOption inputOption(
        {QStringLiteral("i"), QStringLiteral("input")},
        QCoreApplication::translate("DevelopBaseline", "Decoded raster image used as the develop source."),
        QCoreApplication::translate("DevelopBaseline", "path"));
    const QCommandLineOption runIdOption(
        QStringLiteral("run-id"),
        QCoreApplication::translate("DevelopBaseline", "Stable label for this measurement invocation."),
        QCoreApplication::translate("DevelopBaseline", "id"),
        QStringLiteral("manual-1"));
    const QCommandLineOption targetOption(
        QStringLiteral("target"),
        QCoreApplication::translate("DevelopBaseline", "Machine and architecture label recorded in CSV."),
        QCoreApplication::translate("DevelopBaseline", "name"),
        QStringLiteral("unspecified"));
    parser.addOptions({inputOption, runIdOption, targetOption});
    parser.process(application);

    const QString inputPath = parser.value(inputOption);
    if (inputPath.trimmed().isEmpty())
    {
        std::cerr << toUtf8(QCoreApplication::translate("DevelopBaseline", "Missing required --input path.")) << '\n';
        return 2;
    }

    QImageReader reader(inputPath);
    const QImage sourceImage = reader.read();
    if (sourceImage.isNull())
    {
        std::cerr << toUtf8(QCoreApplication::translate("DevelopBaseline", "Unable to decode benchmark input: "))
                  << toUtf8(reader.errorString()) << '\n';
        return 3;
    }

    const flexraw::core::measurement::StageTimer timer;
    const flexraw::core::develop::DevelopImageResult developed =
        flexraw::core::develop::applyDevelop(sourceImage, makeRepresentativeParams());
    const std::uint64_t elapsedNanoseconds = timer.elapsedNanoseconds();

    flexraw::core::measurement::RenderMeasurementRecord record;
    record.runId = toUtf8(parser.value(runIdOption));
    record.mode = "cpu-develop";
    record.target = toUtf8(parser.value(targetOption));
    record.sourceLabel = toUtf8(QFileInfo(inputPath).fileName());
    record.width = static_cast<std::uint32_t>(sourceImage.width());
    record.height = static_cast<std::uint32_t>(sourceImage.height());
    record.sampleIndex = 1;
    record.succeeded = developed.hasValue();
    record.render.developNanoseconds = elapsedNanoseconds;
    record.render.totalNanoseconds = elapsedNanoseconds;
    record.endToEndNanoseconds = elapsedNanoseconds;

    flexraw::core::measurement::writeRenderMeasurementCsvHeader(std::cout);
    flexraw::core::measurement::writeRenderMeasurementCsvRow(std::cout, record);
    if (developed.hasError())
    {
        std::cerr << toUtf8(QCoreApplication::translate("DevelopBaseline", "Develop baseline failed: "))
                  << toUtf8(developed.error().message) << '\n';
        return 4;
    }
    return 0;
}

}  // namespace

// 목적: CPU develop baseline용 Qt Core runtime 생성과 측정 실행
// 입력: argc/argv: benchmark command-line arguments
// 출력: baseline 성공 또는 실패 process exit code
int main(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("Flexraw Develop Baseline"));
    return runBaseline(application);
}
