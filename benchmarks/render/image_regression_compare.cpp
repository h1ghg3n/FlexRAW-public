#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <optional>
#include <span>
#include <string>

#include <QByteArray>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QString>

#include "image_regression.h"

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

// 목적: CSV text field에 comma, quote와 줄바꿈이 있어도 안전하게 출력
// 입력: output: 기록할 stream, value: UTF-8 text
// 출력: output stream에 escaped field 추가
void writeEscapedField(std::ostream& output, const std::string& value)
{
    const bool needsQuotes = value.find_first_of(",\"\r\n") != std::string::npos;
    if (!needsQuotes)
    {
        output << value;
        return;
    }

    output << '"';
    for (const char character : value)
    {
        if (character == '"')
        {
            output << "\"\"";
        }
        else
        {
            output << character;
        }
    }
    output << '"';
}

// 목적: raster file을 비교용 연속 RGBA8 image로 decode
// 입력: path: PNG/JPEG/TIFF 등 Qt image reader가 지원하는 경로, error: 실패 설명 출력 위치
// 출력: 성공 시 RGBA8 image, 실패 시 null image
[[nodiscard]] QImage readRgba8Image(const QString& path, QString& error)
{
    QImageReader reader(path);
    const QImage decoded = reader.read();
    if (decoded.isNull())
    {
        error = QCoreApplication::translate("ImageRegressionCompare", "Unable to decode %1: %2")
                    .arg(path, reader.errorString());
        return {};
    }

    const QImage rgba = decoded.convertToFormat(QImage::Format_RGBA8888);
    if (rgba.isNull() || rgba.bytesPerLine() != rgba.width() * 4)
    {
        error = QCoreApplication::translate("ImageRegressionCompare", "Unable to normalize %1 to contiguous RGBA8.")
                    .arg(path);
        return {};
    }
    return rgba;
}

// 목적: 두 raster image의 RGBA8 RGB-channel 차이를 versioned CSV로 출력
// 입력: application: reference/candidate/run-id/target option이 등록될 Qt application
// 출력: 성공 0, 인자 오류 2, decode 오류 3, geometry/layout 오류 4
int runComparison(QCoreApplication& application)
{
    QCommandLineParser parser;
    parser.setApplicationDescription(QCoreApplication::translate(
        "ImageRegressionCompare", "Measures RGB-channel differences between reference and candidate raster images."));
    parser.addHelpOption();

    const QCommandLineOption referenceOption(
        {QStringLiteral("r"), QStringLiteral("reference")},
        QCoreApplication::translate("ImageRegressionCompare", "Human-reviewed reference raster path."),
        QCoreApplication::translate("ImageRegressionCompare", "path"));
    const QCommandLineOption candidateOption(
        {QStringLiteral("c"), QStringLiteral("candidate")},
        QCoreApplication::translate("ImageRegressionCompare", "Candidate raster path to measure."),
        QCoreApplication::translate("ImageRegressionCompare", "path"));
    const QCommandLineOption runIdOption(
        QStringLiteral("run-id"),
        QCoreApplication::translate("ImageRegressionCompare", "Stable label for this comparison."),
        QCoreApplication::translate("ImageRegressionCompare", "id"),
        QStringLiteral("manual-1"));
    const QCommandLineOption targetOption(
        QStringLiteral("target"),
        QCoreApplication::translate("ImageRegressionCompare", "Machine and architecture label recorded in CSV."),
        QCoreApplication::translate("ImageRegressionCompare", "name"),
        QStringLiteral("unspecified"));
    parser.addOptions({referenceOption, candidateOption, runIdOption, targetOption});
    parser.process(application);

    const QString referencePath = parser.value(referenceOption);
    const QString candidatePath = parser.value(candidateOption);
    if (referencePath.trimmed().isEmpty() || candidatePath.trimmed().isEmpty())
    {
        std::cerr << toUtf8(QCoreApplication::translate("ImageRegressionCompare",
                                                        "Both --reference and --candidate paths are required."))
                  << '\n';
        return 2;
    }

    QString error;
    const QImage reference = readRgba8Image(referencePath, error);
    if (reference.isNull())
    {
        std::cerr << toUtf8(error) << '\n';
        return 3;
    }
    const QImage candidate = readRgba8Image(candidatePath, error);
    if (candidate.isNull())
    {
        std::cerr << toUtf8(error) << '\n';
        return 3;
    }
    if (reference.size() != candidate.size())
    {
        std::cerr << toUtf8(QCoreApplication::translate("ImageRegressionCompare",
                                                        "Reference and candidate dimensions differ."))
                  << '\n';
        return 4;
    }

    const auto referenceBytes =
        std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(reference.constBits()),
                                      static_cast<std::size_t>(reference.sizeInBytes()));
    const auto candidateBytes =
        std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(candidate.constBits()),
                                      static_cast<std::size_t>(candidate.sizeInBytes()));
    const std::optional<flexraw::core::measurement::Rgba8DifferenceMetrics> metrics =
        flexraw::core::measurement::measureRgba8Difference(referenceBytes, candidateBytes);
    if (!metrics.has_value())
    {
        std::cerr << toUtf8(QCoreApplication::translate("ImageRegressionCompare",
                                                        "Unable to compare normalized RGBA8 buffers."))
                  << '\n';
        return 4;
    }

    std::cout << "schema_version,run_id,target,reference,candidate,width,height,pixel_count,mean_absolute_error,"
                 "root_mean_square_error,peak_signal_to_noise_ratio_db,maximum_absolute_error\n";
    std::cout << "1,";
    writeEscapedField(std::cout, toUtf8(parser.value(runIdOption)));
    std::cout << ',';
    writeEscapedField(std::cout, toUtf8(parser.value(targetOption)));
    std::cout << ',';
    writeEscapedField(std::cout, toUtf8(QFileInfo(referencePath).fileName()));
    std::cout << ',';
    writeEscapedField(std::cout, toUtf8(QFileInfo(candidatePath).fileName()));
    std::cout << ',' << reference.width() << ',' << reference.height() << ',' << metrics->comparedPixelCount << ','
              << std::setprecision(10) << metrics->meanAbsoluteError << ',' << metrics->rootMeanSquareError << ','
              << metrics->peakSignalToNoiseRatioDb << ',' << static_cast<unsigned int>(metrics->maximumAbsoluteError)
              << '\n';
    return 0;
}

}  // namespace

// 목적: image regression comparison용 Qt Core runtime 생성과 측정 실행
// 입력: argc/argv: benchmark command-line arguments
// 출력: 비교 성공 또는 실패 process exit code
int main(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("Flexraw Image Regression Compare"));
    return runComparison(application);
}
