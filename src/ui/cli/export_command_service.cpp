#include "export_command_service.h"

#include <algorithm>
#include <limits>
#include <optional>
#include <utility>

#include <QChar>
#include <QColorSpace>
#include <QDir>
#include <QFileInfo>
#include <QImageReader>
#include <QMutex>
#include <QRegularExpression>
#include <QStringList>
#include <QThread>
#include <QThreadPool>
#include <QtConcurrentMap>

#include "catalog_database.h"
#include "catalog_develop_repository.h"
#include "export.h"
#include "folder_scanner.h"

namespace flexraw::ui::cli
{
namespace
{

struct TokenizeResult
{
    bool valid{false};
    QStringList tokens;
};

// 목적: double quote로 묶인 경로를 보존하며 command token을 분리
// 입력: commandLine: 사용자가 입력한 전체 command 문자열
// 출력: quote 검증 상태와 분리된 token 목록
[[nodiscard]] TokenizeResult tokenizeCommand(const QString& commandLine)
{
    QStringList tokens;
    QString token;
    bool inQuotes = false;
    bool tokenStarted = false;
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

        if (character == QLatin1Char('"'))
        {
            inQuotes = !inQuotes;
            tokenStarted = true;
            continue;
        }

        if (character.isSpace() && !inQuotes)
        {
            if (tokenStarted)
            {
                tokens.append(token);
                token.clear();
                tokenStarted = false;
            }
            continue;
        }

        token.append(character);
        tokenStarted = true;
    }

    if (inQuotes)
    {
        return {};
    }

    if (tokenStarted)
    {
        tokens.append(token);
    }

    return {true, tokens};
}

// 목적: 인식된 export command의 parsing 실패 결과 생성
// 입력: message: 사용자에게 표시할 검증 오류
// 출력: export command로 인식된 실패 결과
[[nodiscard]] RasterExportCommandParseResult parseFailure(QString message)
{
    RasterExportCommandParseResult result;
    result.recognized = true;
    result.errorMessage = std::move(message);
    return result;
}

// 목적: 인식된 batch export command의 parsing 실패 결과 생성
// 입력: message: 사용자에게 표시할 검증 오류
// 출력: batch export command로 인식된 실패 결과
[[nodiscard]] BatchExportCommandParseResult parseBatchFailure(QString message)
{
    BatchExportCommandParseResult result;
    result.recognized = true;
    result.errorMessage = std::move(message);
    return result;
}

// 목적: 양의 정수 또는 0으로 표현된 command option 값을 변환
// 입력: value: 정수로 해석할 option 문자열
// 출력: 변환된 정수 또는 형식 오류를 나타내는 빈 값
[[nodiscard]] std::optional<int> parseNonNegativeInteger(const QString& value)
{
    bool converted = false;
    const int parsed = value.toInt(&converted);

    if (!converted || parsed < 0)
    {
        return std::nullopt;
    }

    return parsed;
}

// 목적: command format 문자열을 raster export format enum으로 변환
// 입력: value: jpeg, png 또는 tiff 문자열
// 출력: 대응 format 또는 지원하지 않는 값을 나타내는 빈 값
[[nodiscard]] std::optional<core::export_::RasterExportFormat> parseFormat(const QString& value)
{
    if (value.compare(QStringLiteral("jpeg"), Qt::CaseInsensitive) == 0 ||
        value.compare(QStringLiteral("jpg"), Qt::CaseInsensitive) == 0)
    {
        return core::export_::RasterExportFormat::Jpeg;
    }

    if (value.compare(QStringLiteral("png"), Qt::CaseInsensitive) == 0)
    {
        return core::export_::RasterExportFormat::Png;
    }

    if (value.compare(QStringLiteral("tiff"), Qt::CaseInsensitive) == 0 ||
        value.compare(QStringLiteral("tif"), Qt::CaseInsensitive) == 0)
    {
        return core::export_::RasterExportFormat::Tiff;
    }

    return std::nullopt;
}

// 목적: command 색 공간 문자열을 raster export 색 공간 enum으로 변환
// 입력: value: srgb, adobe-rgb 또는 display-p3 문자열
// 출력: 대응 색 공간 또는 지원하지 않는 값을 나타내는 빈 값
[[nodiscard]] std::optional<core::export_::RasterOutputColorSpace> parseColorSpace(const QString& value)
{
    if (value.compare(QStringLiteral("srgb"), Qt::CaseInsensitive) == 0)
    {
        return core::export_::RasterOutputColorSpace::Srgb;
    }

    if (value.compare(QStringLiteral("adobe-rgb"), Qt::CaseInsensitive) == 0)
    {
        return core::export_::RasterOutputColorSpace::AdobeRgb;
    }

    if (value.compare(QStringLiteral("display-p3"), Qt::CaseInsensitive) == 0)
    {
        return core::export_::RasterOutputColorSpace::DisplayP3;
    }

    return std::nullopt;
}

// 목적: command TIFF compression 문자열을 export compression enum으로 변환
// 입력: value: none 또는 lzw 문자열
// 출력: 대응 compression 또는 지원하지 않는 값을 나타내는 빈 값
[[nodiscard]] std::optional<core::export_::TiffCompression> parseTiffCompression(const QString& value)
{
    if (value.compare(QStringLiteral("none"), Qt::CaseInsensitive) == 0)
    {
        return core::export_::TiffCompression::None;
    }

    if (value.compare(QStringLiteral("lzw"), Qt::CaseInsensitive) == 0)
    {
        return core::export_::TiffCompression::Lzw;
    }

    return std::nullopt;
}

// 목적: command metadata 문자열을 원본 EXIF 포함 여부로 변환
// 입력: value: include 또는 exclude 문자열
// 출력: 대응 metadata 포함 여부 또는 지원하지 않는 값을 나타내는 빈 값
[[nodiscard]] std::optional<bool> parseMetadataInclusion(const QString& value)
{
    if (value.compare(QStringLiteral("include"), Qt::CaseInsensitive) == 0)
    {
        return true;
    }

    if (value.compare(QStringLiteral("exclude"), Qt::CaseInsensitive) == 0)
    {
        return false;
    }

    return std::nullopt;
}

}  // namespace

// 목적: console 입력에서 raster export command와 option을 분석
// 입력: commandLine: 사용자가 입력한 전체 command 문자열
// 출력: command 인식 여부, 검증 결과와 export 인자
RasterExportCommandParseResult ExportCommandService::parseRasterCommand(
    const QString& commandLine, const core::export_::RasterExportOptions& defaultOptions)
{
    const QString trimmedCommand = commandLine.trimmed();
    const qsizetype firstSeparator = trimmedCommand.indexOf(QChar::Space);
    const QString commandName = firstSeparator < 0 ? trimmedCommand : trimmedCommand.first(firstSeparator);

    if (commandName.compare(QStringLiteral("export"), Qt::CaseInsensitive) != 0)
    {
        return {};
    }

    const TokenizeResult tokenized = tokenizeCommand(commandLine);
    if (!tokenized.valid)
    {
        return parseFailure(tr("Export command contains an unterminated quote."));
    }

    const QStringList& tokens = tokenized.tokens;
    if (tokens.size() < 2 || tokens.at(1).compare(QStringLiteral("raster"), Qt::CaseInsensitive) != 0)
    {
        return parseFailure(tr("Usage: export raster --input \"<image-file>\" --output \"<output-file>\" "
                               "--format <jpeg|png|tiff> [--color-space <srgb|adobe-rgb|display-p3>] "
                               "[--quality <1-100>] [--compression <0-9>] [--tiff-compression <none|lzw>] "
                               "[--max-dimension <pixels>] [--metadata <include|exclude>] [--catalog <catalog-file>]"));
    }

    RasterExportCommand command;
    command.options = defaultOptions;
    bool hasInput = false;
    bool hasOutput = false;
    bool hasFormat = false;
    QStringList seenOptions;

    for (qsizetype index = 2; index < tokens.size(); index += 2)
    {
        const QString& option = tokens.at(index);
        if (index + 1 >= tokens.size())
        {
            return parseFailure(tr("Missing value for option: %1").arg(option));
        }

        const QString normalizedOption = option.toLower();
        if (seenOptions.contains(normalizedOption))
        {
            return parseFailure(tr("Option %1 may only be specified once.").arg(option));
        }
        seenOptions.append(normalizedOption);

        const QString& value = tokens.at(index + 1);
        if (normalizedOption == QStringLiteral("--input"))
        {
            command.inputPath = value;
            hasInput = true;
            continue;
        }

        if (normalizedOption == QStringLiteral("--output"))
        {
            command.outputPath = value;
            hasOutput = true;
            continue;
        }

        if (normalizedOption == QStringLiteral("--catalog"))
        {
            command.catalogPath = value;
            continue;
        }

        if (normalizedOption == QStringLiteral("--format"))
        {
            const std::optional<core::export_::RasterExportFormat> format = parseFormat(value);
            if (!format.has_value())
            {
                return parseFailure(tr("Unsupported raster export format: %1").arg(value));
            }
            command.options.format = *format;
            hasFormat = true;
            continue;
        }

        if (normalizedOption == QStringLiteral("--color-space"))
        {
            const std::optional<core::export_::RasterOutputColorSpace> colorSpace = parseColorSpace(value);
            if (!colorSpace.has_value())
            {
                return parseFailure(tr("Unsupported raster export color space: %1").arg(value));
            }
            command.options.outputColorSpace = *colorSpace;
            continue;
        }

        if (normalizedOption == QStringLiteral("--quality"))
        {
            const std::optional<int> quality = parseNonNegativeInteger(value);
            if (!quality.has_value() || *quality < 1 || *quality > 100)
            {
                return parseFailure(tr("JPEG quality must be between 1 and 100."));
            }
            command.options.jpegQuality = *quality;
            continue;
        }

        if (normalizedOption == QStringLiteral("--compression"))
        {
            const std::optional<int> compression = parseNonNegativeInteger(value);
            if (!compression.has_value() || *compression > 9)
            {
                return parseFailure(tr("PNG compression must be between 0 and 9."));
            }
            command.options.pngCompression = *compression;
            continue;
        }

        if (normalizedOption == QStringLiteral("--tiff-compression"))
        {
            const std::optional<core::export_::TiffCompression> compression = parseTiffCompression(value);
            if (!compression.has_value())
            {
                return parseFailure(tr("Unsupported TIFF compression: %1").arg(value));
            }
            command.options.tiffCompression = *compression;
            continue;
        }

        if (normalizedOption == QStringLiteral("--max-dimension"))
        {
            const std::optional<int> maximumDimension = parseNonNegativeInteger(value);
            if (!maximumDimension.has_value())
            {
                return parseFailure(tr("Export maximum dimension must be a non-negative integer."));
            }
            command.options.maximumDimension = *maximumDimension;
            continue;
        }

        if (normalizedOption == QStringLiteral("--metadata"))
        {
            const std::optional<bool> includeMetadata = parseMetadataInclusion(value);
            if (!includeMetadata.has_value())
            {
                return parseFailure(tr("Export metadata option must be include or exclude."));
            }
            command.options.includeMetadata = *includeMetadata;
            continue;
        }

        return parseFailure(tr("Unknown raster export option: %1").arg(option));
    }

    if (!hasInput || !hasOutput || !hasFormat)
    {
        return parseFailure(tr("Options --input, --output, and --format are required."));
    }

    RasterExportCommandParseResult result;
    result.recognized = true;
    result.valid = true;
    result.command = std::move(command);
    return result;
}

// 목적: console 입력에서 기본 develop parameter를 사용하는 RAW export command를 분석
// 입력: commandLine: 사용자가 입력한 전체 command 문자열
// 출력: command 인식 여부, 검증 결과와 export 인자
RasterExportCommandParseResult ExportCommandService::parseRawCommand(
    const QString& commandLine, const core::export_::RasterExportOptions& defaultOptions)
{
    static const QRegularExpression rawCommandPattern(QStringLiteral("^\\s*export\\s+raw(?=\\s|$)"),
                                                      QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = rawCommandPattern.match(commandLine);
    if (!match.hasMatch())
    {
        return {};
    }

    QString rasterCommand = commandLine;
    rasterCommand.replace(match.capturedStart(), match.capturedLength(), QStringLiteral("export raster"));
    return parseRasterCommand(rasterCommand, defaultOptions);
}

// 목적: console 입력에서 병렬 folder export command와 option을 분석
// 입력: commandLine: 사용자가 입력한 전체 command 문자열
// 출력: command 인식 여부, 검증 결과와 batch export 인자
BatchExportCommandParseResult ExportCommandService::parseBatchCommand(
    const QString& commandLine, const core::export_::RasterExportOptions& defaultOptions)
{
    static const QRegularExpression batchCommandPattern(QStringLiteral("^\\s*export\\s+batch(?=\\s|$)"),
                                                        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch commandMatch = batchCommandPattern.match(commandLine);
    if (!commandMatch.hasMatch())
    {
        return {};
    }

    const TokenizeResult tokenized = tokenizeCommand(commandLine);
    if (!tokenized.valid)
    {
        return parseBatchFailure(tr("Export command contains an unterminated quote."));
    }

    const QStringList& tokens = tokenized.tokens;
    BatchExportCommand command;
    command.options = defaultOptions;
    bool hasInputFolder = false;
    bool hasOutputFolder = false;
    bool hasFormat = false;
    QStringList seenOptions;

    for (qsizetype index = 2; index < tokens.size(); index += 2)
    {
        const QString& option = tokens.at(index);
        if (index + 1 >= tokens.size())
        {
            return parseBatchFailure(tr("Missing value for option: %1").arg(option));
        }

        const QString normalizedOption = option.toLower();
        if (seenOptions.contains(normalizedOption))
        {
            return parseBatchFailure(tr("Option %1 may only be specified once.").arg(option));
        }
        seenOptions.append(normalizedOption);

        const QString& value = tokens.at(index + 1);
        if (normalizedOption == QStringLiteral("--input-folder"))
        {
            command.inputFolderPath = value;
            hasInputFolder = true;
        }
        else if (normalizedOption == QStringLiteral("--output-folder"))
        {
            command.outputFolderPath = value;
            hasOutputFolder = true;
        }
        else if (normalizedOption == QStringLiteral("--catalog"))
        {
            command.catalogPath = value;
        }
        else if (normalizedOption == QStringLiteral("--format"))
        {
            const std::optional<core::export_::RasterExportFormat> format = parseFormat(value);
            if (!format.has_value())
            {
                return parseBatchFailure(tr("Unsupported raster export format: %1").arg(value));
            }
            command.options.format = *format;
            hasFormat = true;
        }
        else if (normalizedOption == QStringLiteral("--color-space"))
        {
            const std::optional<core::export_::RasterOutputColorSpace> colorSpace = parseColorSpace(value);
            if (!colorSpace.has_value())
            {
                return parseBatchFailure(tr("Unsupported raster export color space: %1").arg(value));
            }
            command.options.outputColorSpace = *colorSpace;
        }
        else if (normalizedOption == QStringLiteral("--quality"))
        {
            const std::optional<int> quality = parseNonNegativeInteger(value);
            if (!quality.has_value() || *quality < 1 || *quality > 100)
            {
                return parseBatchFailure(tr("JPEG quality must be between 1 and 100."));
            }
            command.options.jpegQuality = *quality;
        }
        else if (normalizedOption == QStringLiteral("--compression"))
        {
            const std::optional<int> compression = parseNonNegativeInteger(value);
            if (!compression.has_value() || *compression > 9)
            {
                return parseBatchFailure(tr("PNG compression must be between 0 and 9."));
            }
            command.options.pngCompression = *compression;
        }
        else if (normalizedOption == QStringLiteral("--tiff-compression"))
        {
            const std::optional<core::export_::TiffCompression> compression = parseTiffCompression(value);
            if (!compression.has_value())
            {
                return parseBatchFailure(tr("Unsupported TIFF compression: %1").arg(value));
            }
            command.options.tiffCompression = *compression;
        }
        else if (normalizedOption == QStringLiteral("--max-dimension"))
        {
            const std::optional<int> maximumDimension = parseNonNegativeInteger(value);
            if (!maximumDimension.has_value())
            {
                return parseBatchFailure(tr("Export maximum dimension must be a non-negative integer."));
            }
            command.options.maximumDimension = *maximumDimension;
        }
        else if (normalizedOption == QStringLiteral("--metadata"))
        {
            const std::optional<bool> includeMetadata = parseMetadataInclusion(value);
            if (!includeMetadata.has_value())
            {
                return parseBatchFailure(tr("Export metadata option must be include or exclude."));
            }
            command.options.includeMetadata = *includeMetadata;
        }
        else if (normalizedOption == QStringLiteral("--workers"))
        {
            const std::optional<int> workerCount = parseNonNegativeInteger(value);
            if (!workerCount.has_value() || *workerCount == 0)
            {
                return parseBatchFailure(tr("Export worker count must be a positive integer."));
            }
            command.workerCount = *workerCount;
        }
        else
        {
            return parseBatchFailure(tr("Unknown batch export option: %1").arg(option));
        }
    }

    if (!hasInputFolder || !hasOutputFolder || !hasFormat)
    {
        return parseBatchFailure(tr("Options --input-folder, --output-folder, and --format are required."));
    }

    BatchExportCommandParseResult result;
    result.recognized = true;
    result.valid = true;
    result.command = std::move(command);
    return result;
}

// 목적: raster input file을 sRGB로 정규화해 지정 export option으로 저장
// 입력: command: 입력/출력 경로와 raster export option
// 출력: 완료된 출력 경로 또는 image decode/export 실패 정보
RasterExportCommandExecutionResult ExportCommandService::exportRaster(const RasterExportCommand& command)
{
    if (!command.catalogPath.isEmpty())
    {
        return {false, {}, tr("Option --catalog is available only for RAW export.")};
    }

    const QFileInfo inputInfo(QDir::cleanPath(command.inputPath));
    if (command.inputPath.isEmpty() || command.inputPath != command.inputPath.trimmed())
    {
        return {false, {}, tr("Raster export input path contains outer whitespace.")};
    }
    if (!inputInfo.exists())
    {
        return {false, {}, tr("Raster export input file does not exist.")};
    }

    if (!inputInfo.isFile() || !inputInfo.isReadable())
    {
        return {false, {}, tr("Raster export input file is not readable.")};
    }

    QImageReader reader(inputInfo.absoluteFilePath());
    QImage sourceImage = reader.read();
    if (sourceImage.isNull())
    {
        return {false, {}, tr("Unable to decode raster export input: %1").arg(reader.errorString())};
    }

    if (sourceImage.colorSpace().isValid())
    {
        sourceImage = sourceImage.convertedToColorSpace(QColorSpace(QColorSpace::SRgb));
    }
    else
    {
        sourceImage.setColorSpace(QColorSpace(QColorSpace::SRgb));
    }

    const core::export_::RasterExportResult exportResult =
        core::export_::writeRasterImage(sourceImage, command.outputPath, command.options, inputInfo.absoluteFilePath());
    if (exportResult.hasError())
    {
        return {false, {}, exportResult.error().message};
    }

    return {true, QFileInfo(command.outputPath).absoluteFilePath(), {}};
}

// 목적: RAW input file을 full decode해 기본 develop parameter와 지정 export option으로 저장
// 입력: command: 입력/출력 경로와 raster export option
// 출력: 완료된 출력 경로 또는 RAW decode/develop/export 실패 정보
RasterExportCommandExecutionResult ExportCommandService::exportRaw(const RasterExportCommand& command)
{
    core::types::DevelopParams params;

    if (!command.catalogPath.isEmpty())
    {
        core::catalog::CatalogDatabaseOpenResult catalog = core::catalog::CatalogDatabase::open(command.catalogPath);
        if (catalog.hasError())
        {
            return {false, {}, catalog.error().message};
        }

        core::catalog::CatalogDevelopRepository repository(*catalog.value());
        const core::catalog::CatalogDevelopParamsResult storedParams =
            repository.loadParams(QFileInfo(command.inputPath).absoluteFilePath());
        if (storedParams.hasError())
        {
            return {false, {}, storedParams.error().message};
        }

        if (storedParams.value().has_value())
        {
            params = *storedParams.value();
        }
    }

    const core::export_::RasterExportResult exportResult =
        core::export_::writeRawImage(command.inputPath, params, command.outputPath, command.options);
    if (exportResult.hasError())
    {
        return {false, {}, exportResult.error().message};
    }

    return {true, QFileInfo(command.outputPath).absoluteFilePath(), {}};
}

// 목적: 지원 input folder를 worker pool로 병렬 처리해 지정 output folder에 저장
// 입력: command: input/output folder, worker 수, catalog과 raster export option
// 출력: 완료 여부와 전체 성공/실패 집계 또는 준비 단계 실패 정보
BatchExportCommandExecutionResult ExportCommandService::exportBatch(const BatchExportCommand& command)
{
    const core::catalog::CatalogScanResult scannedEntries = core::catalog::scanFolder(command.inputFolderPath);
    if (scannedEntries.hasError())
    {
        return {false, 0, 0, 0, scannedEntries.error().message};
    }

    const QFileInfo outputFolderInfo(QDir::cleanPath(command.outputFolderPath));
    if (!outputFolderInfo.exists() || !outputFolderInfo.isDir() || !outputFolderInfo.isWritable())
    {
        return {false, 0, 0, 0, tr("Batch export output folder is unavailable or not writable.")};
    }

    const QVector<core::catalog::CatalogEntry>& entries = scannedEntries.value();
    if (entries.size() > static_cast<qsizetype>(std::numeric_limits<int>::max()))
    {
        return {false, 0, 0, 0, tr("Batch export file count exceeds the supported range.")};
    }

    BatchExportCommandExecutionResult result{true, static_cast<int>(entries.size()), 0, 0, {}};
    if (entries.isEmpty())
    {
        return result;
    }

    QThreadPool pool;
    const int idealThreadCount = QThread::idealThreadCount();
    pool.setMaxThreadCount(command.workerCount > 0 ? command.workerCount : std::max(1, idealThreadCount));
    QMutex resultMutex;
    const QDir outputFolder(outputFolderInfo.absoluteFilePath());

    QtConcurrent::blockingMap(&pool, entries, [&](const core::catalog::CatalogEntry& entry) {
        const QFileInfo sourceInfo(entry.file.path);
        const QString outputName =
            QStringLiteral("%1-%2.%3")
                .arg(sourceInfo.completeBaseName(), sourceInfo.suffix().toLower())
                .arg(command.options.format == core::export_::RasterExportFormat::Jpeg  ? QStringLiteral("jpg")
                     : command.options.format == core::export_::RasterExportFormat::Png ? QStringLiteral("png")
                                                                                        : QStringLiteral("tiff"));
        RasterExportCommand itemCommand{
            entry.file.path,
            outputFolder.filePath(outputName),
            entry.file.kind == core::types::SupportedFileKind::Raw ? command.catalogPath : QString{},
            command.options,
        };
        const RasterExportCommandExecutionResult itemResult =
            entry.file.kind == core::types::SupportedFileKind::Raw ? exportRaw(itemCommand) : exportRaster(itemCommand);

        QMutexLocker lock(&resultMutex);
        if (itemResult.succeeded)
        {
            ++result.succeededCount;
        }
        else
        {
            ++result.failedCount;
            if (result.firstError.isEmpty())
            {
                result.firstError = itemResult.errorMessage;
            }
        }
    });

    return result;
}

}  // namespace flexraw::ui::cli
