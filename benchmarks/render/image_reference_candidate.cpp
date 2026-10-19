#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>

#include <QByteArray>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QString>
#include <QUuid>

#include "local_render_executor.h"
#include "representative_develop_profile.h"
#include "resolved_render_pipeline.h"

namespace
{

constexpr qsizetype HashChunkBytes = 1024 * 1024;
constexpr int ReceiptSchemaVersion = 1;
constexpr char OutputProfileId[] = "png-srgb-full-v1";

struct CandidateReceipt
{
    QString runId;
    QString target;
    QString producerRevision;
    QString sourceLabel;
    QByteArray sourceSha256;
    QString candidateLabel;
    QByteArray artifactSha256;
    QByteArray rgbSha256;
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::uint64_t pixelCount{0};
};

// 목적: QString을 CSV와 stderr에 기록할 UTF-8 standard string으로 변환
// 입력: value: 변환할 Qt text
// 출력: UTF-8 byte sequence
[[nodiscard]] std::string toUtf8(const QString& value)
{
    const QByteArray bytes = value.toUtf8();
    return {bytes.constData(), static_cast<std::size_t>(bytes.size())};
}

// 목적: CSV text field를 RFC 4180 호환 double-quote escaping으로 출력
// 입력: output: 대상 stream, value: UTF-8 field value
// 출력: output stream에 escaping된 field 추가
void writeCsvField(std::ostream& output, const std::string& value)
{
    output << '"';
    for (const char character : value)
    {
        if (character == '"')
        {
            output << '"';
        }
        output << character;
    }
    output << '"';
}

// 목적: 큰 RAW와 candidate artifact를 전체 memory 적재 없이 SHA-256으로 식별
// 입력: path: 읽을 file, error: 실패 사유 수신 문자열
// 출력: 성공 시 lowercase hexadecimal SHA-256, 실패 시 빈 값
[[nodiscard]] std::optional<QByteArray> sha256File(const QString& path, QString& error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        error = QCoreApplication::translate("ImageReferenceCandidate", "Unable to open %1: %2")
                    .arg(path, file.errorString());
        return std::nullopt;
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd())
    {
        const QByteArray chunk = file.read(HashChunkBytes);
        if (chunk.isEmpty() && file.error() != QFileDevice::NoError)
        {
            error = QCoreApplication::translate("ImageReferenceCandidate", "Unable to read %1: %2")
                        .arg(path, file.errorString());
            return std::nullopt;
        }
        hash.addData(chunk);
    }
    return hash.result().toHex();
}

// 목적: decoded candidate의 alpha를 제외한 row-major RGB byte identity 계산
// 입력: image: RGBA8888로 정규화할 candidate, error: 실패 사유 수신 문자열
// 출력: 성공 시 lowercase hexadecimal SHA-256, 실패 시 빈 값
[[nodiscard]] std::optional<QByteArray> sha256RgbPixels(const QImage& image, QString& error)
{
    const QImage rgba = image.convertToFormat(QImage::Format_RGBA8888);
    if (rgba.isNull())
    {
        error = QCoreApplication::translate("ImageReferenceCandidate", "Unable to normalize candidate to RGBA8.");
        return std::nullopt;
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    QByteArray rgbRow(static_cast<qsizetype>(rgba.width()) * 3, Qt::Uninitialized);
    for (int row = 0; row < rgba.height(); ++row)
    {
        const auto* source = rgba.constScanLine(row);
        auto* destination = reinterpret_cast<std::uint8_t*>(rgbRow.data());
        for (int column = 0; column < rgba.width(); ++column)
        {
            const std::size_t sourceOffset = static_cast<std::size_t>(column) * 4;
            const std::size_t destinationOffset = static_cast<std::size_t>(column) * 3;
            destination[destinationOffset] = source[sourceOffset];
            destination[destinationOffset + 1] = source[sourceOffset + 1];
            destination[destinationOffset + 2] = source[sourceOffset + 2];
        }
        hash.addData(rgbRow);
    }
    return hash.result().toHex();
}

// 목적: candidate와 source identity를 보존하는 version 1 receipt CSV 출력
// 입력: output: 대상 stream, receipt: source/profile/artifact identity와 geometry
// 출력: header와 data row를 output stream에 기록
void writeReceipt(std::ostream& output, const CandidateReceipt& receipt)
{
    output << "schema_version,run_id,target,producer_revision,source_label,source_sha256,develop_profile,"
              "output_profile,candidate_label,width,height,pixel_count,artifact_sha256,rgb_sha256\n";
    output << ReceiptSchemaVersion << ',';
    writeCsvField(output, toUtf8(receipt.runId));
    output << ',';
    writeCsvField(output, toUtf8(receipt.target));
    output << ',';
    writeCsvField(output, toUtf8(receipt.producerRevision));
    output << ',';
    writeCsvField(output, toUtf8(receipt.sourceLabel));
    output << ',' << receipt.sourceSha256.constData() << ',';
    writeCsvField(output, flexraw::benchmarks::render::RepresentativeDevelopProfileId);
    output << ',';
    writeCsvField(output, OutputProfileId);
    output << ',';
    writeCsvField(output, toUtf8(receipt.candidateLabel));
    output << ',' << receipt.width << ',' << receipt.height << ',' << receipt.pixelCount << ','
           << receipt.artifactSha256.constData() << ',' << receipt.rgbSha256.constData() << '\n';
}

// 목적: fixed profile RAW를 lossless PNG candidate로 render하고 receipt identity 수집
// 입력: sourcePath/outputPath: source와 create-only candidate 경로, receipt: 식별 결과 수신 객체
// 출력: 성공 시 true, 실패 시 stderr 진단과 false
[[nodiscard]] bool renderCandidate(const QString& sourcePath, const QString& outputPath, CandidateReceipt& receipt)
{
    QString error;
    const std::optional<QByteArray> sourceHash = sha256File(sourcePath, error);
    if (!sourceHash.has_value())
    {
        std::cerr << toUtf8(error) << '\n';
        return false;
    }

    const QFileInfo outputInfo(outputPath);
    const QString temporaryPath =
        QDir(outputInfo.absolutePath())
            .filePath(
                QStringLiteral(".flexraw-candidate-%1.png").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));

    flexraw::core::render::ResolvedRenderRequest request;
    request.sourcePath = sourcePath;
    request.outputPath = temporaryPath;
    request.developParams = flexraw::benchmarks::render::makeRepresentativeDevelopParams();
    request.outputOptions.format = flexraw::core::export_::RasterExportFormat::Png;
    request.outputOptions.pngCompression = 6;
    request.outputOptions.outputColorSpace = flexraw::core::export_::RasterOutputColorSpace::Srgb;
    request.outputOptions.includeMetadata = false;

    const flexraw::core::render::ResolvedRenderPipeline pipeline;
    const flexraw::core::render::LocalRenderExecutor executor(pipeline);
    const flexraw::core::types::CancellationSource cancellation;
    const flexraw::core::render::ResolvedRenderPipelineResult result = executor.execute(request, cancellation.token());
    if (result.hasError())
    {
        QFile::remove(temporaryPath);
        std::cerr << toUtf8(result.error().cause.message) << '\n';
        return false;
    }
    if (!QFile::rename(temporaryPath, outputPath))
    {
        QFile::remove(temporaryPath);
        std::cerr << toUtf8(QCoreApplication::translate(
                                "ImageReferenceCandidate",
                                "Unable to publish candidate without replacing an existing file: %1")
                                .arg(outputPath))
                  << '\n';
        return false;
    }

    const std::optional<QByteArray> artifactHash = sha256File(outputPath, error);
    QImageReader reader(outputPath);
    const QImage decoded = reader.read();
    if (!artifactHash.has_value() || decoded.isNull())
    {
        if (decoded.isNull())
        {
            error = QCoreApplication::translate("ImageReferenceCandidate", "Unable to decode candidate %1: %2")
                        .arg(outputPath, reader.errorString());
        }
        QFile::remove(outputPath);
        std::cerr << toUtf8(error) << '\n';
        return false;
    }
    const std::optional<QByteArray> rgbHash = sha256RgbPixels(decoded, error);
    if (!rgbHash.has_value())
    {
        QFile::remove(outputPath);
        std::cerr << toUtf8(error) << '\n';
        return false;
    }

    receipt.sourceLabel = QFileInfo(sourcePath).fileName();
    receipt.sourceSha256 = *sourceHash;
    receipt.candidateLabel = outputInfo.fileName();
    receipt.artifactSha256 = *artifactHash;
    receipt.rgbSha256 = *rgbHash;
    receipt.width = static_cast<std::uint32_t>(decoded.width());
    receipt.height = static_cast<std::uint32_t>(decoded.height());
    receipt.pixelCount = static_cast<std::uint64_t>(receipt.width) * receipt.height;
    return true;
}

// 목적: candidate 생성 option을 검증하고 fixed-profile render와 receipt 출력을 수행
// 입력: application: command-line option을 소유하는 Qt application
// 출력: 성공 0, 인자 오류 2, render/identity 실패 3
int runCandidate(QCoreApplication& application)
{
    QCommandLineParser parser;
    parser.setApplicationDescription(QCoreApplication::translate(
        "ImageReferenceCandidate", "Creates one fixed-profile lossless image regression candidate and receipt."));
    parser.addHelpOption();
    const QCommandLineOption inputOption({QStringLiteral("i"), QStringLiteral("input")},
                                         QCoreApplication::translate("ImageReferenceCandidate", "RAW source path."),
                                         QStringLiteral("path"));
    const QCommandLineOption outputOption(
        {QStringLiteral("o"), QStringLiteral("output")},
        QCoreApplication::translate("ImageReferenceCandidate", "New lossless PNG candidate path."),
        QStringLiteral("path"));
    const QCommandLineOption revisionOption(
        QStringLiteral("producer-revision"),
        QCoreApplication::translate("ImageReferenceCandidate", "Exact source revision producing this candidate."),
        QStringLiteral("revision"));
    const QCommandLineOption runIdOption(QStringLiteral("run-id"),
                                         QCoreApplication::translate("ImageReferenceCandidate", "Stable run label."),
                                         QStringLiteral("id"));
    const QCommandLineOption targetOption(
        QStringLiteral("target"),
        QCoreApplication::translate("ImageReferenceCandidate", "Machine and architecture label."),
        QStringLiteral("name"));
    parser.addOptions({inputOption, outputOption, revisionOption, runIdOption, targetOption});
    parser.process(application);

    const QString inputPath = QDir::cleanPath(parser.value(inputOption));
    const QString outputPath = QDir::cleanPath(parser.value(outputOption));
    CandidateReceipt receipt;
    receipt.runId = parser.value(runIdOption).trimmed();
    receipt.target = parser.value(targetOption).trimmed();
    receipt.producerRevision = parser.value(revisionOption).trimmed();
    if (inputPath.trimmed().isEmpty() || outputPath.trimmed().isEmpty() || receipt.runId.isEmpty() ||
        receipt.target.isEmpty() || receipt.producerRevision.isEmpty())
    {
        std::cerr << toUtf8(QCoreApplication::translate(
                         "ImageReferenceCandidate",
                         "--input, --output, --producer-revision, --run-id and --target are required."))
                  << '\n';
        return 2;
    }

    const QFileInfo sourceInfo(inputPath);
    const QFileInfo outputInfo(outputPath);
    if (!sourceInfo.isFile() || !sourceInfo.isReadable() ||
        outputInfo.suffix().compare(QStringLiteral("png"), Qt::CaseInsensitive) != 0 || outputInfo.exists() ||
        !QDir(outputInfo.absolutePath()).exists())
    {
        std::cerr << toUtf8(QCoreApplication::translate(
                         "ImageReferenceCandidate",
                         "The source must be a readable file and output must be a new .png in an existing directory."))
                  << '\n';
        return 2;
    }

    if (!renderCandidate(sourceInfo.absoluteFilePath(), outputInfo.absoluteFilePath(), receipt))
    {
        return 3;
    }
    writeReceipt(std::cout, receipt);
    return 0;
}

}  // namespace

// 목적: image regression candidate용 Qt Core runtime 생성과 fixed-profile render 실행
// 입력: argc/argv: candidate 생성 command-line arguments
// 출력: candidate와 receipt 생성 성공 또는 실패 process exit code
int main(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("Flexraw Image Reference Candidate"));
    return runCandidate(application);
}
