#include "export_pipeline.h"

#include <algorithm>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

#include <QColorSpace>
#include <QDir>
#include <QFileInfo>
#include <QImageReader>
#include <QMutex>
#include <QMutexLocker>
#include <QThread>
#include <QThreadPool>
#include <QtConcurrentMap>

#include "catalog_database.h"
#include "catalog_develop_repository.h"
#include "develop.h"
#include "export.h"
#include "folder_scanner.h"

namespace flexraw::core::orchestration
{
namespace
{

constexpr int DefaultMaximumBatchWorkers = 4;

// 목적: export pipeline 내부 오류를 project-owned CoreError로 생성
// 입력: code: 오류 분류, message: technical 상세 message
// 출력: 구성된 CoreError 값
[[nodiscard]] types::CoreError makeError(types::ErrorCode code, QString message)
{
    return {code, std::move(message)};
}

// 목적: cooperative cancellation 상태를 project-owned CoreError로 생성
// 입력: 없음
// 출력: Orchestrator가 terminal cancellation으로 처리할 오류
[[nodiscard]] types::CoreError makeCancelledError()
{
    return makeError(types::ErrorCode::Cancelled, QStringLiteral("Export pipeline cancellation requested."));
}

// 목적: cooperative cancellation로 중단된 pipeline 결과 생성
// 입력: 없음
// 출력: Orchestrator가 cancellation terminal event로 처리할 오류
[[nodiscard]] ExportPipelineResult makeCancelledResult()
{
    return ExportPipelineResult::failure(makeCancelledError());
}

// 목적: 한 source의 성공 결과를 상세 report item으로 생성
// 입력: sourcePath: 입력 파일 경로, outputPath: 생성된 절대 출력 경로
// 출력: 성공 상태 ExportItemResult
[[nodiscard]] ExportItemResult makeSucceededItem(QString sourcePath, QString outputPath)
{
    return {std::move(sourcePath), std::move(outputPath), true, {}};
}

// 목적: 한 source의 실패 결과를 상세 report item으로 생성
// 입력: sourcePath/outputPath: 요청 경로, error: domain 또는 준비 단계 오류
// 출력: 실패 상태 ExportItemResult
[[nodiscard]] ExportItemResult makeFailedItem(QString sourcePath, QString outputPath, types::CoreError error)
{
    return {std::move(sourcePath), std::move(outputPath), false, std::move(error)};
}

// 목적: raster source를 sRGB working image로 decode하고 optional develop state 적용
// 입력: request: source, output, develop state와 encoding option
// 출력: item 단위 성공 또는 실패 상세 결과
[[nodiscard]] ExportItemResult exportRasterFile(const ExportFileRequest& request,
                                                const types::CancellationToken& cancellationToken)
{
    const QFileInfo inputInfo(QDir::cleanPath(request.source.path));
    if (request.source.path.trimmed().isEmpty() || !inputInfo.exists())
    {
        return makeFailedItem(
            request.source.path,
            request.outputPath,
            makeError(types::ErrorCode::NotFound, QStringLiteral("Raster export input file does not exist.")));
    }

    if (!inputInfo.isFile() || !inputInfo.isReadable())
    {
        return makeFailedItem(
            request.source.path,
            request.outputPath,
            makeError(types::ErrorCode::PermissionDenied, QStringLiteral("Raster export input file is not readable.")));
    }

    QImageReader reader(inputInfo.absoluteFilePath());
    QImage sourceImage = reader.read();
    if (sourceImage.isNull())
    {
        return makeFailedItem(
            request.source.path,
            request.outputPath,
            makeError(types::ErrorCode::DecodeFailed,
                      QStringLiteral("Unable to decode raster export input: %1").arg(reader.errorString())));
    }

    if (cancellationToken.isCancellationRequested())
    {
        return makeFailedItem(request.source.path, request.outputPath, makeCancelledError());
    }

    if (sourceImage.colorSpace().isValid())
    {
        sourceImage = sourceImage.convertedToColorSpace(QColorSpace(QColorSpace::SRgb));
    }
    else
    {
        sourceImage.setColorSpace(QColorSpace(QColorSpace::SRgb));
    }

    if (sourceImage.isNull())
    {
        return makeFailedItem(request.source.path,
                              request.outputPath,
                              makeError(types::ErrorCode::DecodeFailed,
                                        QStringLiteral("Unable to convert raster export input to sRGB.")));
    }

    if (cancellationToken.isCancellationRequested())
    {
        return makeFailedItem(request.source.path, request.outputPath, makeCancelledError());
    }

    if (request.developParams.has_value())
    {
        const develop::DevelopImageResult developed = develop::applyDevelop(sourceImage, *request.developParams);
        if (developed.hasError())
        {
            return makeFailedItem(request.source.path, request.outputPath, developed.error());
        }
        sourceImage = developed.value();
    }

    const export_::RasterExportResult exported = export_::writeRasterImage(
        sourceImage, request.outputPath, request.options, inputInfo.absoluteFilePath(), &cancellationToken);
    if (exported.hasError())
    {
        return makeFailedItem(request.source.path, request.outputPath, exported.error());
    }

    return makeSucceededItem(inputInfo.absoluteFilePath(), QFileInfo(request.outputPath).absoluteFilePath());
}

// 목적: RAW source에 resolved develop state를 적용해 raster file 생성
// 입력: request: source, output과 encoding option, params: catalog 또는 caller에서 확정된 develop state
// 출력: item 단위 성공 또는 실패 상세 결과
[[nodiscard]] ExportItemResult exportRawFile(const ExportFileRequest& request,
                                             const types::DevelopParams& params,
                                             const types::CancellationToken& cancellationToken)
{
    const export_::RasterExportResult exported =
        export_::writeRawImage(request.source.path, params, request.outputPath, request.options, &cancellationToken);
    if (exported.hasError())
    {
        return makeFailedItem(request.source.path, request.outputPath, exported.error());
    }

    return makeSucceededItem(QFileInfo(request.source.path).absoluteFilePath(),
                             QFileInfo(request.outputPath).absoluteFilePath());
}

// 목적: caller override 또는 catalog에서 RAW develop state 확정
// 입력: sourcePath: catalog photo path, overrideParams: caller snapshot, repository: optional catalog repository
// 출력: 검증된 develop state 또는 catalog/parameter 오류
[[nodiscard]] types::Result<types::DevelopParams, types::CoreError> resolveDevelopParams(
    const QString& sourcePath,
    const std::optional<types::DevelopParams>& overrideParams,
    const catalog::CatalogDevelopRepository* repository)
{
    if (overrideParams.has_value())
    {
        return develop::validateDevelopParams(*overrideParams);
    }

    if (repository == nullptr)
    {
        return types::Result<types::DevelopParams, types::CoreError>::success({});
    }

    const catalog::CatalogDevelopParamsResult storedParams =
        repository->loadParams(QFileInfo(sourcePath).absoluteFilePath());
    if (storedParams.hasError())
    {
        return types::Result<types::DevelopParams, types::CoreError>::failure(storedParams.error());
    }

    return types::Result<types::DevelopParams, types::CoreError>::success(
        storedParams.value().value_or(types::DevelopParams{}));
}

// 목적: 단일 RAW request의 catalog open과 develop-state 조회를 pipeline instance 안에서 직렬화
// 입력: request: catalog path와 optional caller override, catalogAccessMutex: shared catalog access lock
// 출력: 검증된 develop state 또는 catalog/parameter 오류
[[nodiscard]] types::Result<types::DevelopParams, types::CoreError> resolveFileDevelopParams(
    const ExportFileRequest& request, QMutex& catalogAccessMutex)
{
    if (request.catalogPath.isEmpty() || request.developParams.has_value())
    {
        return resolveDevelopParams(request.source.path, request.developParams, nullptr);
    }

    QMutexLocker lock(&catalogAccessMutex);
    catalog::CatalogDatabaseOpenResult opened = catalog::CatalogDatabase::open(request.catalogPath);
    if (opened.hasError())
    {
        return types::Result<types::DevelopParams, types::CoreError>::failure(opened.error());
    }

    catalog::CatalogDevelopRepository repository(*opened.value());
    const types::Result<types::DevelopParams, types::CoreError> resolved =
        resolveDevelopParams(request.source.path, request.developParams, &repository);
    if (resolved.hasError() && resolved.error().code == types::ErrorCode::NotFound &&
        request.useDefaultDevelopParamsWhenCatalogPhotoMissing)
    {
        return types::Result<types::DevelopParams, types::CoreError>::success(types::DevelopParams{});
    }
    return resolved;
}

// 목적: 단일 file request의 develop state를 worker 실행 전에 확정
// 입력: request: source/output/catalog 값, catalogAccessMutex: shared catalog lock, cancellationToken: 취소 상태
// 출력: resolved immutable item 하나 또는 cancellation 오류
[[nodiscard]] ExportPreparationResult prepareFileRequest(const ExportFileRequest& request,
                                                         QMutex& catalogAccessMutex,
                                                         const types::CancellationToken& cancellationToken)
{
    if (cancellationToken.isCancellationRequested())
    {
        return ExportPreparationResult::failure(makeCancelledError());
    }

    PreparedExportItem item{request, {}};
    if (request.source.kind == types::SupportedFileKind::Raw)
    {
        const types::Result<types::DevelopParams, types::CoreError> params =
            resolveFileDevelopParams(request, catalogAccessMutex);
        if (params.hasError())
        {
            item.preparationError = params.error();
        }
        else
        {
            item.request.catalogPath.clear();
            item.request.developParams = params.value();
        }
    }

    PreparedExport prepared;
    prepared.items.push_back(std::move(item));
    return ExportPreparationResult::success(std::move(prepared));
}

// 목적: 한 file request를 decode, develop, encode 순서로 실행하고 progress 집계
// 입력: request: 단일 file 요청, cancellationToken: 중단 상태, progress: callback, catalogAccessMutex: catalog lock
// 출력: item 하나의 상세 report 또는 cancellation 오류
[[nodiscard]] ExportPipelineResult executeFileRequest(const ExportFileRequest& request,
                                                      const types::CancellationToken& cancellationToken,
                                                      const ExportProgressCallback& progress,
                                                      QMutex& catalogAccessMutex)
{
    if (progress)
    {
        progress({0, 1, 0, 0, request.source.path});
    }

    if (cancellationToken.isCancellationRequested())
    {
        return makeCancelledResult();
    }

    std::optional<ExportItemResult> item;
    if (request.source.kind == types::SupportedFileKind::Raw)
    {
        const types::Result<types::DevelopParams, types::CoreError> params =
            resolveFileDevelopParams(request, catalogAccessMutex);
        item = params.hasError() ? makeFailedItem(request.source.path, request.outputPath, params.error())
                                 : exportRawFile(request, params.value(), cancellationToken);
    }
    else
    {
        item = exportRasterFile(request, cancellationToken);
    }

    if (cancellationToken.isCancellationRequested())
    {
        return makeCancelledResult();
    }

    ExportReport report;
    report.totalCount = 1;
    report.succeededCount = item->succeeded ? 1 : 0;
    report.failedCount = item->succeeded ? 0 : 1;
    report.items.push_back(std::move(*item));
    if (progress)
    {
        progress({1, 1, report.succeededCount, report.failedCount, report.items.constFirst().sourcePath});
    }
    return ExportPipelineResult::success(std::move(report));
}

// 목적: 명시적 file request 목록의 develop state를 item scheduling 전에 확정
// 입력: request: source/output intent 목록, cancellationToken: 중단 상태, catalogAccessMutex: shared catalog lock
// 출력: 원래 순서를 보존한 immutable item 목록 또는 cancellation 오류
[[nodiscard]] ExportPreparationResult prepareItemListRequest(const ExportItemListRequest& request,
                                                             const types::CancellationToken& cancellationToken,
                                                             QMutex& catalogAccessMutex)
{
    PreparedExport prepared;
    prepared.items.reserve(request.items.size());
    for (const ExportFileRequest& requestItem : request.items)
    {
        const ExportPreparationResult itemResult =
            prepareFileRequest(requestItem, catalogAccessMutex, cancellationToken);
        if (itemResult.hasError())
        {
            return itemResult;
        }
        prepared.items.push_back(itemResult.value().items.constFirst());
    }
    return ExportPreparationResult::success(std::move(prepared));
}

// 목적: 준비된 item을 RAW 또는 raster processing leaf로 실행하는 뒤쪽 helper 선언
// 입력: item: resolved export intent, cancellationToken: cooperative cancellation 상태
// 출력: item 단위 성공 또는 실패 상세 결과
[[nodiscard]] ExportItemResult executePreparedItem(const PreparedExportItem& item,
                                                   const types::CancellationToken& cancellationToken);

// 목적: 명시적 file request 목록을 synchronous compatibility 경로에서 순서대로 실행
// 입력: request: item 목록, cancellationToken: 중단 상태, progress: 누적 callback, catalogAccessMutex: lock
// 출력: 모든 item의 성공·실패 report 또는 준비·cancellation 오류
[[nodiscard]] ExportPipelineResult executeItemListRequest(const ExportItemListRequest& request,
                                                          const types::CancellationToken& cancellationToken,
                                                          const ExportProgressCallback& progress,
                                                          QMutex& catalogAccessMutex)
{
    const ExportPreparationResult preparation = prepareItemListRequest(request, cancellationToken, catalogAccessMutex);
    if (preparation.hasError())
    {
        return ExportPipelineResult::failure(preparation.error());
    }

    const QVector<PreparedExportItem>& items = preparation.value().items;
    ExportReport report;
    report.totalCount = static_cast<int>(items.size());
    if (progress)
    {
        progress({0, report.totalCount, 0, 0, {}});
    }
    for (const PreparedExportItem& item : items)
    {
        if (cancellationToken.isCancellationRequested())
        {
            return makeCancelledResult();
        }
        ExportItemResult result = executePreparedItem(item, cancellationToken);
        if (cancellationToken.isCancellationRequested())
        {
            return makeCancelledResult();
        }
        result.succeeded ? ++report.succeededCount : ++report.failedCount;
        report.items.push_back(std::move(result));
        if (progress)
        {
            progress({static_cast<int>(report.items.size()),
                      report.totalCount,
                      report.succeededCount,
                      report.failedCount,
                      item.request.source.path});
        }
    }
    return ExportPipelineResult::success(std::move(report));
}

// 목적: output format에 대응하는 batch file 확장자 반환
// 입력: format: raster output format
// 출력: 점을 제외한 lower-case 확장자
[[nodiscard]] QString outputExtension(export_::RasterExportFormat format)
{
    switch (format)
    {
    case export_::RasterExportFormat::Jpeg:
        return QStringLiteral("jpg");
    case export_::RasterExportFormat::Png:
        return QStringLiteral("png");
    case export_::RasterExportFormat::Tiff:
        return QStringLiteral("tiff");
    }

    return {};
}

// 목적: batch source 이름과 원본 확장자를 보존하는 flat output 경로 생성
// 입력: source: 입력 file descriptor, outputFolder: 검증된 출력 folder, format: 출력 format
// 출력: batch item의 output file 경로
[[nodiscard]] QString makeBatchOutputPath(const types::FileDescriptor& source,
                                          const QDir& outputFolder,
                                          export_::RasterExportFormat format)
{
    const QFileInfo sourceInfo(source.path);
    const QString outputName = QStringLiteral("%1-%2.%3")
                                   .arg(sourceInfo.completeBaseName(), sourceInfo.suffix().toLower())
                                   .arg(outputExtension(format));
    return outputFolder.filePath(outputName);
}

// 목적: batch worker 수를 machine logical CPU와 item 수 범위로 제한
// 입력: requestedWorkerCount: 0이면 안정적인 기본값, itemCount: 처리할 전체 file 수
// 출력: 1 이상 itemCount 이하의 worker 수
[[nodiscard]] int resolveBatchWorkerCount(int requestedWorkerCount, int itemCount)
{
    const int availableWorkers = std::max(1, QThread::idealThreadCount());
    const int configuredWorkers = requestedWorkerCount > 0 ? std::min(requestedWorkerCount, availableWorkers)
                                                           : std::min(DefaultMaximumBatchWorkers, availableWorkers);
    return std::max(1, std::min(configuredWorkers, itemCount));
}

// 목적: batch item별 output 경로와 RAW develop state를 worker 실행 전에 준비
// 입력: entries: scan 결과, request: option, outputFolder: 출력 folder, catalogAccessMutex: catalog lock
// 출력: 병렬 worker가 thread-affine database 없이 처리할 immutable item 목록
[[nodiscard]] QVector<PreparedExportItem> prepareBatchItems(const QVector<catalog::CatalogEntry>& entries,
                                                            const ExportBatchRequest& request,
                                                            const QDir& outputFolder,
                                                            QMutex& catalogAccessMutex,
                                                            const types::CancellationToken& cancellationToken)
{
    std::unique_ptr<QMutexLocker<QMutex>> catalogLock;
    catalog::CatalogDatabasePtr catalogDatabase;
    std::unique_ptr<catalog::CatalogDevelopRepository> repository;
    std::optional<types::CoreError> catalogError;
    const bool containsRaw = std::any_of(entries.cbegin(), entries.cend(), [](const catalog::CatalogEntry& entry) {
        return entry.file.kind == types::SupportedFileKind::Raw;
    });
    if (containsRaw && !request.catalogPath.isEmpty() && !request.developParams.has_value())
    {
        catalogLock = std::make_unique<QMutexLocker<QMutex>>(&catalogAccessMutex);
        catalog::CatalogDatabaseOpenResult opened = catalog::CatalogDatabase::open(request.catalogPath);
        if (opened.hasError())
        {
            catalogError = opened.error();
        }
        else
        {
            catalogDatabase = std::move(opened.value());
            repository = std::make_unique<catalog::CatalogDevelopRepository>(*catalogDatabase);
        }
    }

    QVector<PreparedExportItem> items;
    items.reserve(entries.size());
    for (const catalog::CatalogEntry& entry : entries)
    {
        if (cancellationToken.isCancellationRequested())
        {
            break;
        }

        PreparedExportItem item{{entry.file,
                                 makeBatchOutputPath(entry.file, outputFolder, request.options.format),
                                 {},
                                 {},
                                 request.options},
                                {}};
        if (entry.file.kind == types::SupportedFileKind::Raw)
        {
            if (catalogError.has_value())
            {
                item.preparationError = *catalogError;
            }
            else
            {
                const types::Result<types::DevelopParams, types::CoreError> params =
                    resolveDevelopParams(entry.file.path, request.developParams, repository.get());
                if (params.hasError())
                {
                    item.preparationError = params.error();
                }
                else
                {
                    item.request.developParams = params.value();
                }
            }
        }
        else if (request.developParams.has_value())
        {
            item.request.developParams = request.developParams;
        }
        items.push_back(std::move(item));
    }
    return items;
}

// 목적: 준비된 batch item 하나를 해당 RAW 또는 raster domain pipeline에 전달
// 입력: item: source/output/develop 준비값, options: 공통 encoding option
// 출력: item 단위 성공 또는 실패 상세 결과
[[nodiscard]] ExportItemResult executePreparedItem(const PreparedExportItem& item,
                                                   const types::CancellationToken& cancellationToken)
{
    if (item.preparationError.has_value())
    {
        return makeFailedItem(item.request.source.path, item.request.outputPath, *item.preparationError);
    }

    if (item.request.source.kind == types::SupportedFileKind::Raw)
    {
        return exportRawFile(
            item.request, item.request.developParams.value_or(types::DevelopParams{}), cancellationToken);
    }
    return exportRasterFile(item.request, cancellationToken);
}

// 목적: folder scan, output validation과 item별 develop state를 실행 전 준비
// 입력: request: batch 설정, cancellationToken: 중단 상태, catalogAccessMutex: shared catalog lock
// 출력: immutable item 목록 또는 scan/output/cancellation 오류
[[nodiscard]] ExportPreparationResult prepareBatchRequest(const ExportBatchRequest& request,
                                                          const types::CancellationToken& cancellationToken,
                                                          QMutex& catalogAccessMutex)
{
    if (cancellationToken.isCancellationRequested())
    {
        return ExportPreparationResult::failure(makeCancelledError());
    }

    const catalog::CatalogScanResult scannedEntries = catalog::scanFolder(request.inputFolderPath);
    if (scannedEntries.hasError())
    {
        return ExportPreparationResult::failure(scannedEntries.error());
    }

    const QFileInfo outputFolderInfo(QDir::cleanPath(request.outputFolderPath));
    if (!outputFolderInfo.exists() || !outputFolderInfo.isDir() || !outputFolderInfo.isWritable())
    {
        return ExportPreparationResult::failure(
            makeError(types::ErrorCode::PermissionDenied,
                      QStringLiteral("Batch export output folder is unavailable or not writable.")));
    }

    const QVector<catalog::CatalogEntry>& entries = scannedEntries.value();
    if (entries.size() > static_cast<qsizetype>(std::numeric_limits<int>::max()))
    {
        return ExportPreparationResult::failure(makeError(
            types::ErrorCode::InvalidArgument, QStringLiteral("Batch export file count exceeds the supported range.")));
    }

    PreparedExport prepared;
    if (!entries.isEmpty())
    {
        const QDir outputFolder(outputFolderInfo.absoluteFilePath());
        prepared.items = prepareBatchItems(entries, request, outputFolder, catalogAccessMutex, cancellationToken);
    }
    if (cancellationToken.isCancellationRequested())
    {
        return ExportPreparationResult::failure(makeCancelledError());
    }
    return ExportPreparationResult::success(std::move(prepared));
}

// 목적: folder scan과 file 단위 bounded 병렬 처리로 batch export 수행
// 입력: request: batch 설정, cancellationToken: 중단 상태, progress: callback, catalogAccessMutex: catalog lock
// 출력: 모든 처리 item의 상세 report 또는 준비 단계/cancellation 오류
[[nodiscard]] ExportPipelineResult executeBatchRequest(const ExportBatchRequest& request,
                                                       const types::CancellationToken& cancellationToken,
                                                       const ExportProgressCallback& progress,
                                                       QMutex& catalogAccessMutex)
{
    const ExportPreparationResult preparation = prepareBatchRequest(request, cancellationToken, catalogAccessMutex);
    if (preparation.hasError())
    {
        return ExportPipelineResult::failure(preparation.error());
    }

    const QVector<PreparedExportItem>& preparedItems = preparation.value().items;
    const int totalCount = static_cast<int>(preparedItems.size());
    if (progress)
    {
        progress({0, totalCount, 0, 0, {}});
    }
    if (preparedItems.isEmpty())
    {
        return ExportPipelineResult::success({});
    }

    if (cancellationToken.isCancellationRequested())
    {
        return makeCancelledResult();
    }

    std::vector<int> indices(static_cast<std::size_t>(totalCount));
    std::iota(indices.begin(), indices.end(), 0);
    std::vector<ExportItemResult> itemResults(static_cast<std::size_t>(totalCount));
    int completedCount = 0;
    int succeededCount = 0;
    int failedCount = 0;
    QMutex resultMutex;
    QThreadPool workerPool;
    workerPool.setMaxThreadCount(resolveBatchWorkerCount(request.workerCount, totalCount));

    QtConcurrent::blockingMap(&workerPool, indices, [&](const int index) {
        if (cancellationToken.isCancellationRequested())
        {
            return;
        }

        const PreparedExportItem& preparedItem = preparedItems.at(index);
        ExportItemResult item = executePreparedItem(preparedItem, cancellationToken);
        if (cancellationToken.isCancellationRequested())
        {
            return;
        }

        {
            QMutexLocker lock(&resultMutex);
            itemResults[static_cast<std::size_t>(index)] = std::move(item);
            ++completedCount;
            if (itemResults[static_cast<std::size_t>(index)].succeeded)
            {
                ++succeededCount;
            }
            else
            {
                ++failedCount;
            }
            if (progress)
            {
                progress({completedCount, totalCount, succeededCount, failedCount, preparedItem.request.source.path});
            }
        }
    });

    if (cancellationToken.isCancellationRequested())
    {
        return makeCancelledResult();
    }

    ExportReport report;
    report.totalCount = totalCount;
    report.succeededCount = succeededCount;
    report.failedCount = failedCount;
    report.items.reserve(totalCount);
    for (ExportItemResult& item : itemResults)
    {
        report.items.push_back(std::move(item));
    }
    return ExportPipelineResult::success(std::move(report));
}

}  // namespace

// 목적: legacy injected pipeline의 단일 file request를 기본 prepared item으로 변환
// 입력: request: file 또는 batch 요청, cancellationToken: cooperative cancellation 상태
// 출력: 단일 file item 또는 지원되지 않는 batch/cancellation 오류
ExportPreparationResult IExportPipeline::prepare(const ExportRequest& request,
                                                 const types::CancellationToken& cancellationToken) const
{
    if (cancellationToken.isCancellationRequested())
    {
        return ExportPreparationResult::failure(makeCancelledError());
    }
    if (!std::holds_alternative<ExportFileRequest>(request))
    {
        return ExportPreparationResult::failure(makeError(
            types::ErrorCode::InvalidArgument,
            QStringLiteral("The injected export pipeline does not support item preparation for batch requests.")));
    }

    PreparedExport prepared;
    prepared.items.push_back({std::get<ExportFileRequest>(request), {}});
    return ExportPreparationResult::success(std::move(prepared));
}

// 목적: legacy pipeline execute 결과를 단일 item 결과로 호환 변환
// 입력: item: 준비된 단일 file 값, cancellationToken: cooperative cancellation 상태
// 출력: 첫 report item 또는 pipeline-level 오류를 item failure로 변환한 값
ExportItemResult IExportPipeline::executeItem(const PreparedExportItem& item,
                                              const types::CancellationToken& cancellationToken) const
{
    if (item.preparationError.has_value())
    {
        return makeFailedItem(item.request.source.path, item.request.outputPath, *item.preparationError);
    }
    const ExportPipelineResult result =
        execute(ExportRequest{item.request}, cancellationToken, [](const ExportProgressSnapshot&) {});
    if (result.hasError())
    {
        return makeFailedItem(item.request.source.path, item.request.outputPath, result.error());
    }
    if (result.value().items.isEmpty())
    {
        return makeFailedItem(
            item.request.source.path,
            item.request.outputPath,
            makeError(types::ErrorCode::Unknown, QStringLiteral("The export pipeline returned no item result.")));
    }
    return result.value().items.constFirst();
}

// 목적: file 또는 folder request를 resolved immutable item 목록으로 준비
// 입력: request: file 또는 batch 요청, cancellationToken: cooperative cancellation 상태
// 출력: item 목록 또는 scan/output/catalog 준비 오류
ExportPreparationResult FileExportPipeline::prepare(const ExportRequest& request,
                                                    const types::CancellationToken& cancellationToken) const
{
    return std::visit(
        [&](const auto& typedRequest) -> ExportPreparationResult {
            using RequestType = std::decay_t<decltype(typedRequest)>;
            if constexpr (std::is_same_v<RequestType, ExportFileRequest>)
            {
                return prepareFileRequest(typedRequest, m_catalogAccessMutex, cancellationToken);
            }
            else if constexpr (std::is_same_v<RequestType, ExportBatchRequest>)
            {
                return prepareBatchRequest(typedRequest, cancellationToken, m_catalogAccessMutex);
            }
            else
            {
                return prepareItemListRequest(typedRequest, cancellationToken, m_catalogAccessMutex);
            }
        },
        request);
}

// 목적: 준비된 file item 하나를 RAW/raster processing leaf로 실행
// 입력: item: resolved source/output/develop 값, cancellationToken: cooperative cancellation 상태
// 출력: item 단위 성공 또는 실패 상세 결과
ExportItemResult FileExportPipeline::executeItem(const PreparedExportItem& item,
                                                 const types::CancellationToken& cancellationToken) const
{
    return executePreparedItem(item, cancellationToken);
}

// 목적: Core domain module을 조합해 file-backed export request 처리
// 입력: request: file 또는 batch 요청, cancellationToken: cooperative cancellation 상태, progress: 진행 callback
// 출력: item별 상세 report 또는 scan, catalog, output 준비 오류
ExportPipelineResult FileExportPipeline::execute(const ExportRequest& request,
                                                 const types::CancellationToken& cancellationToken,
                                                 const ExportProgressCallback& progress) const
{
    return std::visit(
        [&](const auto& typedRequest) -> ExportPipelineResult {
            using RequestType = std::decay_t<decltype(typedRequest)>;
            if constexpr (std::is_same_v<RequestType, ExportFileRequest>)
            {
                return executeFileRequest(typedRequest, cancellationToken, progress, m_catalogAccessMutex);
            }
            else if constexpr (std::is_same_v<RequestType, ExportBatchRequest>)
            {
                return executeBatchRequest(typedRequest, cancellationToken, progress, m_catalogAccessMutex);
            }
            else
            {
                return executeItemListRequest(typedRequest, cancellationToken, progress, m_catalogAccessMutex);
            }
        },
        request);
}

}  // namespace flexraw::core::orchestration
