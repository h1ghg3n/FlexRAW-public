#include "console_command_controller.h"

#include <cstddef>
#include <optional>
#include <string>
#include <utility>

#include <QByteArray>
#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QtConcurrentRun>

#include "log.h"

namespace flexraw::ui::cli
{
namespace
{

// 목적: console path와 command 종류를 Qt-free Export source snapshot으로 변환
// 입력: path: 사용자가 입력한 source 경로, kind: command가 확정한 RAW 또는 raster 종류
// 출력: 정규화된 절대 UTF-8 locator와 file 이름을 포함한 source snapshot
[[nodiscard]] core::client::EditorSourceSnapshot makeSourceSnapshot(const QString& path,
                                                                    core::client::CatalogFileKind kind)
{
    const QFileInfo fileInfo(QDir::cleanPath(path));
    const QByteArray sourcePath = fileInfo.absoluteFilePath().toUtf8();
    const QByteArray extension = fileInfo.suffix().toLower().toUtf8();
    const QByteArray displayName = fileInfo.fileName().toUtf8();
    return {{sourcePath.constData(), static_cast<std::size_t>(sourcePath.size())},
            {extension.constData(), static_cast<std::size_t>(extension.size())},
            {displayName.constData(), static_cast<std::size_t>(displayName.size())},
            kind};
}

// 목적: export report에서 첫 item failure 조회
// 입력: report: item별 성공과 structured error를 포함한 완료 report
// 출력: 첫 failure item pointer 또는 모든 item이 성공했으면 nullptr
[[nodiscard]] const core::client::ExportItemResult* firstExportFailure(const core::client::ExportReport& report)
{
    for (const core::client::ExportItemResult& item : report.items)
    {
        if (!item.succeeded)
        {
            return &item;
        }
    }
    return nullptr;
}

// 목적: Qt command 문자열을 byte length가 보존된 UTF-8 client string으로 변환
// 입력: value: parser 또는 file dialog가 반환한 QString
// 출력: Qt-free contract용 UTF-8 string
[[nodiscard]] std::string toClientString(const QString& value)
{
    const QByteArray utf8 = value.toUtf8();
    return {utf8.constData(), static_cast<std::size_t>(utf8.size())};
}

// 목적: byte length가 보존된 UTF-8 client string을 console output text로 변환
// 입력: value: Export output locator
// 출력: 같은 Unicode text의 QString
[[nodiscard]] QString fromClientString(const std::string& value)
{
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

// 목적: command parser의 raster option을 Qt-free Export option으로 투영
// 입력: options: existing parser/settings option
// 출력: product contract format/quality/color snapshot
[[nodiscard]] core::client::ExportRasterOptions toClientOptions(const core::export_::RasterExportOptions& options)
{
    core::client::ExportRasterOptions projected;
    switch (options.format)
    {
    case core::export_::RasterExportFormat::Jpeg:
        projected.format = core::client::ExportRasterFormat::Jpeg;
        break;
    case core::export_::RasterExportFormat::Png:
        projected.format = core::client::ExportRasterFormat::Png;
        break;
    case core::export_::RasterExportFormat::Tiff:
        projected.format = core::client::ExportRasterFormat::Tiff;
        break;
    }
    projected.jpegQuality = options.jpegQuality;
    projected.pngCompression = options.pngCompression;
    projected.tiffCompression = options.tiffCompression == core::export_::TiffCompression::None
                                    ? core::client::ExportTiffCompression::None
                                    : core::client::ExportTiffCompression::Lzw;
    projected.maximumDimension = options.maximumDimension;
    switch (options.outputColorSpace)
    {
    case core::export_::RasterOutputColorSpace::Srgb:
        projected.outputColorSpace = core::client::ExportOutputColorSpace::Srgb;
        break;
    case core::export_::RasterOutputColorSpace::AdobeRgb:
        projected.outputColorSpace = core::client::ExportOutputColorSpace::AdobeRgb;
        break;
    case core::export_::RasterOutputColorSpace::DisplayP3:
        projected.outputColorSpace = core::client::ExportOutputColorSpace::DisplayP3;
        break;
    }
    projected.includeMetadata = options.includeMetadata;
    return projected;
}

// 목적: application-wide Qt-free Export default를 existing console parser option으로 투영
// 입력: options: product contract가 반환한 raster default snapshot
// 출력: command parser와 existing service가 사용하는 processing option
[[nodiscard]] core::export_::RasterExportOptions fromClientOptions(const core::client::ExportRasterOptions& options)
{
    core::export_::RasterExportOptions projected;
    switch (options.format)
    {
    case core::client::ExportRasterFormat::Jpeg:
        projected.format = core::export_::RasterExportFormat::Jpeg;
        break;
    case core::client::ExportRasterFormat::Png:
        projected.format = core::export_::RasterExportFormat::Png;
        break;
    case core::client::ExportRasterFormat::Tiff:
        projected.format = core::export_::RasterExportFormat::Tiff;
        break;
    }
    projected.jpegQuality = options.jpegQuality;
    projected.pngCompression = options.pngCompression;
    projected.tiffCompression = options.tiffCompression == core::client::ExportTiffCompression::None
                                    ? core::export_::TiffCompression::None
                                    : core::export_::TiffCompression::Lzw;
    projected.maximumDimension = options.maximumDimension;
    switch (options.outputColorSpace)
    {
    case core::client::ExportOutputColorSpace::Srgb:
        projected.outputColorSpace = core::export_::RasterOutputColorSpace::Srgb;
        break;
    case core::client::ExportOutputColorSpace::AdobeRgb:
        projected.outputColorSpace = core::export_::RasterOutputColorSpace::AdobeRgb;
        break;
    case core::client::ExportOutputColorSpace::DisplayP3:
        projected.outputColorSpace = core::export_::RasterOutputColorSpace::DisplayP3;
        break;
    }
    projected.includeMetadata = options.includeMetadata;
    return projected;
}

}  // namespace

// 목적: application-wide Export default contract를 사용하는 console command controller 초기화
// 입력: Folder/Export command/event와 Export default contract, parent: Qt 부모 객체
// 출력: 초기화된 ConsoleCommandController 객체
ConsoleCommandController::ConsoleCommandController(core::client::IFolderImportClient& folderImportClient,
                                                   core::client::IFolderImportEventSource& folderImportEventSource,
                                                   core::client::IExportClient& exportClient,
                                                   core::client::IExportEventSource& exportEventSource,
                                                   core::client::IExportDefaultsClient& exportDefaultsClient,
                                                   QObject* parent)
    : QObject(parent),
      m_folderImportClient(&folderImportClient),
      m_exportClient(&exportClient),
      m_exportDefaultsClient(&exportDefaultsClient)
{
    connectExportEvents(exportEventSource);
    connectFolderOperationEvents(folderImportEventSource);
}

// 목적: Qt-free Export event source를 current console request handler에 연결
// 입력: eventSource: application-scoped Export lifecycle source
// 출력: RAII subscription 저장 또는 diagnostic logging
void ConsoleCommandController::connectExportEvents(core::client::IExportEventSource& eventSource)
{
    const core::client::ExportSubscriptionResult subscribed =
        eventSource.subscribeToExports([this](const core::client::ExportEvent& event) { handleExportEvent(event); });
    if (subscribed.hasError())
    {
        LOG_ERROR("cli", "Unable to subscribe to Export operations: {}", subscribed.error().technicalMessage);
        return;
    }
    m_exportSubscription = subscribed.value();
}

// 목적: immutable Export event에서 current console request의 exact terminal만 처리
// 입력: event: initial snapshot 또는 accepted/progress/completed/failed/cancelled transition
// 출력: matching terminal이면 outputReady와 busyChanged(false) 발생
void ConsoleCommandController::handleExportEvent(const core::client::ExportEvent& event)
{
    if (event.completed.has_value())
    {
        handleExportCompleted(*event.completed);
    }
    if (event.failed.has_value())
    {
        handleExportFailed(*event.failed);
    }
    if (event.cancelled.has_value())
    {
        handleExportCancelled(event.cancelled->requestId);
    }
}

// 목적: Folder operation event source를 current console request handler에 연결
// 입력: eventSource: MainWindow와 공유하는 Qt delivery adapter
// 출력: RAII subscription 저장 또는 diagnostic logging
void ConsoleCommandController::connectFolderOperationEvents(core::client::IFolderImportEventSource& eventSource)
{
    const core::client::FolderOperationSubscriptionResult subscribed = eventSource.subscribeToFolderOperations(
        [this](const core::client::FolderOperationEvent& event) { handleFolderOperationEvent(event); });
    if (subscribed.hasError())
    {
        LOG_ERROR("cli", "Unable to subscribe to Folder operations: {}", subscribed.error().technicalMessage);
        return;
    }
    m_folderOperationSubscription = subscribed.value();
}

bool ConsoleCommandController::tryExecute(const QString& commandLine)
{
    const RawDiagnosticsCommandParseResult rawDiagnosticsParsed = RawDiagnosticService::parseRawCommand(commandLine);
    if (rawDiagnosticsParsed.recognized)
    {
        return tryExecuteRawDiagnostics(rawDiagnosticsParsed);
    }

    core::export_::RasterExportOptions defaultOptions;
    const core::client::ExportDefaultsResult defaults = m_exportDefaultsClient->exportDefaults();
    if (defaults.hasValue())
    {
        defaultOptions = fromClientOptions(defaults.value().rasterOptions);
    }
    else
    {
        LOG_WARN("cli", "Unable to load Export defaults: {}", defaults.error().technicalMessage);
    }
    const BatchExportCommandParseResult batchExportParsed =
        ExportCommandService::parseBatchCommand(commandLine, defaultOptions);
    if (batchExportParsed.recognized)
    {
        return tryExecuteBatchExport(batchExportParsed);
    }

    const RasterExportCommandParseResult rawExportParsed =
        ExportCommandService::parseRawCommand(commandLine, defaultOptions);
    if (rawExportParsed.recognized)
    {
        return tryExecuteRawExport(rawExportParsed);
    }

    const RasterExportCommandParseResult exportParsed =
        ExportCommandService::parseRasterCommand(commandLine, defaultOptions);
    if (exportParsed.recognized)
    {
        return tryExecuteRasterExport(exportParsed);
    }

    const CatalogImportCommandParseResult parsed = CatalogCommandService::parseImportCommand(commandLine);

    if (!parsed.recognized)
    {
        return false;
    }

    if (!parsed.valid)
    {
        emit outputReady(parsed.errorMessage);
        return true;
    }

    if (m_busy)
    {
        emit outputReady(tr("A catalog command is already running."));
        return true;
    }

    if (m_folderOperationSubscription == nullptr)
    {
        emit outputReady(tr("Catalog import is unavailable."));
        return true;
    }
    const core::client::FolderOperationResult submitted = m_folderImportClient->submitFolderImport(
        {toClientString(parsed.command.folderPath),
         toClientString(QFileInfo(parsed.command.catalogPath).absoluteFilePath())});
    if (submitted.hasError())
    {
        LOG_WARN("cli", "Catalog import submission failed: {}", submitted.error().technicalMessage);
        emit outputReady(
            submitted.error().code == core::client::ClientErrorCode::Conflict
                ? tr("Catalog import failed: Open the requested catalog or wait for the current folder operation.")
                : tr("Catalog import failed."));
        return true;
    }

    m_pendingFolderOperation = submitted.value().id;
    m_busy = true;
    emit busyChanged(true);
    return true;
}

// 목적: raster export command를 검증하고 worker thread에서 실행
// 입력: parsed: parse가 완료된 raster export command 결과
// 출력: export command로 처리했는지 여부
bool ConsoleCommandController::tryExecuteRasterExport(const RasterExportCommandParseResult& parsed)
{
    if (!parsed.valid)
    {
        emit outputReady(parsed.errorMessage);
        return true;
    }

    if (m_busy)
    {
        emit outputReady(tr("A console command is already running."));
        return true;
    }

    const core::client::ExportFileRequest request{
        makeSourceSnapshot(parsed.command.inputPath, core::client::CatalogFileKind::RasterImage),
        toClientString(parsed.command.outputPath),
        toClientString(parsed.command.catalogPath),
        {},
        toClientOptions(parsed.command.options),
    };
    return submitExport(request, parsed.command.options, PendingExportKind::File);
}

// 목적: RAW export command를 검증하고 worker thread에서 실행
// 입력: parsed: parse가 완료된 RAW export command 결과
// 출력: RAW export command로 처리했는지 여부
bool ConsoleCommandController::tryExecuteRawExport(const RasterExportCommandParseResult& parsed)
{
    if (!parsed.valid)
    {
        emit outputReady(parsed.errorMessage);
        return true;
    }

    if (m_busy)
    {
        emit outputReady(tr("A console command is already running."));
        return true;
    }

    const core::client::ExportFileRequest request{
        makeSourceSnapshot(parsed.command.inputPath, core::client::CatalogFileKind::Raw),
        toClientString(parsed.command.outputPath),
        toClientString(parsed.command.catalogPath),
        {},
        toClientOptions(parsed.command.options),
    };
    return submitExport(request, parsed.command.options, PendingExportKind::File);
}

// 목적: RAW diagnostics command를 검증하고 worker thread에서 실행
// 입력: parsed: parse가 완료된 RAW diagnostics command 결과
// 출력: command로 처리했는지 여부
bool ConsoleCommandController::tryExecuteRawDiagnostics(const RawDiagnosticsCommandParseResult& parsed)
{
    if (!parsed.valid)
    {
        emit outputReady(parsed.errorMessage);
        return true;
    }

    if (m_busy)
    {
        emit outputReady(tr("A console command is already running."));
        return true;
    }

    m_busy = true;
    emit busyChanged(true);

    auto* watcher = new QFutureWatcher<RawDiagnosticsExecutionResult>(this);
    connect(watcher, &QFutureWatcher<RawDiagnosticsExecutionResult>::finished, this, [this, watcher] {
        emit outputReady(formatRawDiagnosticsResult(watcher->result()));
        m_busy = false;
        emit busyChanged(false);
        watcher->deleteLater();
    });
    watcher->setFuture(
        QtConcurrent::run([command = parsed.command] { return RawDiagnosticService::inspectRaw(command); }));
    return true;
}

// 목적: batch export command를 검증하고 worker thread에서 실행
// 입력: parsed: parse가 완료된 batch export command 결과
// 출력: batch export command로 처리했는지 여부
bool ConsoleCommandController::tryExecuteBatchExport(const BatchExportCommandParseResult& parsed)
{
    if (!parsed.valid)
    {
        emit outputReady(parsed.errorMessage);
        return true;
    }

    if (m_busy)
    {
        emit outputReady(tr("A console command is already running."));
        return true;
    }

    const core::client::ExportBatchRequest request{
        toClientString(parsed.command.inputFolderPath),
        toClientString(parsed.command.outputFolderPath),
        toClientString(parsed.command.catalogPath),
        {},
        static_cast<std::uint32_t>(parsed.command.workerCount),
        toClientOptions(parsed.command.options),
    };
    return submitExport(request, parsed.command.options, PendingExportKind::Batch);
}

// 목적: Core export request를 submit하고 accepted request state를 console adapter에 연결
// 입력: request: Core file/batch request, options: 성공 시 저장할 기본값, kind: 결과 formatting 종류
// 출력: command 처리 완료를 나타내는 true
bool ConsoleCommandController::submitExport(core::client::ExportRequest request,
                                            const core::export_::RasterExportOptions& options,
                                            PendingExportKind kind)
{
    const core::client::SubmitExportCommand command{std::move(request),
                                                    {core::client::ExportPlacementPolicy::LocalOnly, std::nullopt}};
    const core::client::ExportSubmissionResult submitted = m_exportClient->submitExport(command);
    if (submitted.hasError())
    {
        LOG_WARN("cli", "Console Export submission failed: {}", submitted.error().technicalMessage);
        const QString message = exportErrorText(submitted.error());
        if (kind == PendingExportKind::Batch)
        {
            emit outputReady(formatBatchExportResult({false, 0, 0, 0, message}));
        }
        else
        {
            emit outputReady(formatRasterExportResult({false, {}, message}));
        }
        return true;
    }

    m_pendingExport = PendingExport{submitted.value().requestId, kind, options};
    m_busy = true;
    emit busyChanged(true);
    return true;
}

// 목적: 현재 console export와 일치하는 완료 report를 표시하고 settings 갱신
// 입력: result: request identity와 item별 상세 결과
// 출력: outputReady와 busyChanged signal 발생 가능
void ConsoleCommandController::handleExportCompleted(const core::client::ExportResult& result)
{
    if (!m_pendingExport.has_value() || m_pendingExport->requestId != result.requestId)
    {
        return;
    }

    if (m_pendingExport->kind == PendingExportKind::Batch)
    {
        if (result.report.failedCount == 0)
        {
            const core::client::ExportRasterDefaultsResult savedDefaults =
                m_exportDefaultsClient->saveRasterExportDefaults(toClientOptions(m_pendingExport->options));
            if (savedDefaults.hasError())
            {
                LOG_WARN("cli", "Unable to save raster Export defaults: {}", savedDefaults.error().technicalMessage);
            }
        }
        const core::client::ExportItemResult* firstFailure = firstExportFailure(result.report);
        const QString firstError = firstFailure == nullptr || !firstFailure->error.has_value()
                                       ? QString{}
                                       : exportErrorText(*firstFailure->error, firstFailure->failureKind);
        emit outputReady(formatBatchExportResult({true,
                                                  static_cast<int>(result.report.totalCount),
                                                  static_cast<int>(result.report.succeededCount),
                                                  static_cast<int>(result.report.failedCount),
                                                  firstError}));
    }
    else
    {
        RasterExportCommandExecutionResult executionResult{
            false,
            {},
            tr("Export pipeline returned no item result."),
        };
        if (!result.report.items.empty())
        {
            const core::client::ExportItemResult& item = result.report.items.front();
            executionResult = {item.succeeded,
                               fromClientString(item.outputLocator),
                               item.error.has_value() ? exportErrorText(*item.error, item.failureKind) : QString{}};
        }
        if (executionResult.succeeded)
        {
            const core::client::ExportRasterDefaultsResult savedDefaults =
                m_exportDefaultsClient->saveRasterExportDefaults(toClientOptions(m_pendingExport->options));
            if (savedDefaults.hasError())
            {
                LOG_WARN("cli", "Unable to save raster Export defaults: {}", savedDefaults.error().technicalMessage);
            }
        }
        emit outputReady(formatRasterExportResult(executionResult));
    }
    finishExport();
}

// 목적: 현재 console export의 terminal pipeline 실패를 사용자 출력으로 변환
// 입력: issue: request identity와 technical error
// 출력: outputReady와 busyChanged signal 발생 가능
void ConsoleCommandController::handleExportFailed(const core::client::ExportIssue& issue)
{
    if (!m_pendingExport.has_value() || m_pendingExport->requestId != issue.requestId)
    {
        return;
    }

    LOG_WARN("cli", "Console Export request failed: {}", issue.error.technicalMessage);
    const QString message = exportErrorText(issue.error);
    if (m_pendingExport->kind == PendingExportKind::Batch)
    {
        emit outputReady(formatBatchExportResult({false, 0, 0, 0, message}));
    }
    else
    {
        emit outputReady(formatRasterExportResult({false, {}, message}));
    }
    finishExport();
}

// 목적: 현재 console export cancellation을 표시하고 입력 상태 복원
// 입력: requestId: 취소된 export request 식별자
// 출력: outputReady와 busyChanged signal 발생 가능
void ConsoleCommandController::handleExportCancelled(const core::client::ExportRequestId requestId)
{
    if (!m_pendingExport.has_value() || m_pendingExport->requestId != requestId)
    {
        return;
    }

    emit outputReady(tr("Export cancelled."));
    finishExport();
}

// 목적: common ClientError와 Export domain failure를 console user-facing text로 변환
// 입력: error: diagnostic-only detail, failureKind: optional Export domain 의미
// 출력: technicalMessage를 직접 노출하지 않는 localized 문자열
QString ConsoleCommandController::exportErrorText(const core::client::ClientError& error,
                                                  const core::client::ExportFailureKind failureKind) const
{
    switch (failureKind)
    {
    case core::client::ExportFailureKind::Eligibility:
        return tr("The source or destination is not eligible for this export target.");
    case core::client::ExportFailureKind::DispatchExhausted:
        return tr("The Worker could not accept this export after the allowed attempts.");
    case core::client::ExportFailureKind::Ambiguous:
        return tr("The Worker result is uncertain. Verify the output before trying again.");
    case core::client::ExportFailureKind::Execution:
    case core::client::ExportFailureKind::None:
        break;
    }

    switch (error.code)
    {
    case core::client::ClientErrorCode::InvalidArgument:
        return tr("Check the export arguments and destination.");
    case core::client::ClientErrorCode::NotFound:
        return tr("The source, destination, or Worker profile is no longer available.");
    case core::client::ClientErrorCode::PermissionDenied:
        return tr("Flexraw cannot access the source or destination.");
    case core::client::ClientErrorCode::UnsupportedFormat:
        return tr("The input or output format is not supported.");
    case core::client::ClientErrorCode::DecodeFailed:
        return tr("The source image could not be decoded.");
    case core::client::ClientErrorCode::DatabaseError:
        return tr("The Catalog state required for export could not be read.");
    case core::client::ClientErrorCode::Conflict:
        return tr("The export state changed. Try the command again.");
    case core::client::ClientErrorCode::Cancelled:
        return tr("The export was cancelled.");
    case core::client::ClientErrorCode::ThumbnailUnavailable:
    case core::client::ClientErrorCode::Unknown:
        return tr("The export could not be completed.");
    }
    return tr("The export could not be completed.");
}

// 목적: current export adapter state를 정리하고 console 입력 재활성화
// 입력: 없음
// 출력: busyChanged(false) signal 발생
void ConsoleCommandController::finishExport()
{
    m_pendingExport.reset();
    m_busy = false;
    emit busyChanged(false);
}

bool ConsoleCommandController::isBusy() const noexcept
{
    return m_busy;
}

// 목적: current console Folder import와 일치하는 terminal을 사용자 출력으로 변환
// 입력: event: initial/active/terminal Folder operation event
// 출력: matching terminal이면 outputReady와 busyChanged(false) 발생
void ConsoleCommandController::handleFolderOperationEvent(const core::client::FolderOperationEvent& event)
{
    if (!event.terminal.has_value() || !m_pendingFolderOperation.has_value() ||
        event.terminal->receipt.id != *m_pendingFolderOperation)
    {
        return;
    }

    QString output = tr("Catalog import failed.");
    if (event.terminal->state == core::client::FolderOperationTerminalState::Completed &&
        event.terminal->completion.has_value() &&
        std::holds_alternative<core::client::FolderImportCompletion>(*event.terminal->completion))
    {
        output = formatImportResult(std::get<core::client::FolderImportCompletion>(*event.terminal->completion));
    }
    else if (event.terminal->state == core::client::FolderOperationTerminalState::Cancelled)
    {
        output = tr("Catalog import cancelled.");
    }
    else if (event.terminal->error.has_value())
    {
        LOG_WARN("cli", "Catalog import terminal failure: {}", event.terminal->error->technicalMessage);
        if (event.terminal->error->code == core::client::ClientErrorCode::Conflict)
        {
            output = tr("Catalog import failed: The active catalog changed while the folder was being scanned.");
        }
    }

    emit outputReady(output);
    m_pendingFolderOperation.reset();
    m_busy = false;
    emit busyChanged(false);
}

// 목적: Folder import completion을 사용자 표시 문자열로 변환
// 입력: completion: discovered/applied count와 persisted Photo identity
// 출력: console output 문자열
QString ConsoleCommandController::formatImportResult(const core::client::FolderImportCompletion& completion) const
{
    return tr("Catalog import complete: scanned %1, stored %2.")
        .arg(completion.discoveredCount)
        .arg(completion.appliedCount);
}

// 목적: console output 영역에 표시할 raster export 결과 formatting
// 입력: result: export service가 반환한 실행 결과
// 출력: console output 문자열
QString ConsoleCommandController::formatRasterExportResult(const RasterExportCommandExecutionResult& result) const
{
    if (!result.succeeded)
    {
        return tr("Raster export failed: %1").arg(result.errorMessage);
    }

    return tr("Raster export complete: %1").arg(result.outputPath);
}

// 목적: console output 영역에 표시할 batch export 결과 formatting
// 입력: result: batch export service가 반환한 실행 결과
// 출력: console output 문자열
QString ConsoleCommandController::formatBatchExportResult(const BatchExportCommandExecutionResult& result) const
{
    if (!result.completed)
    {
        return tr("Batch export failed: %1").arg(result.firstError);
    }

    QString output = tr("Batch export complete: total %1, succeeded %2, failed %3.")
                         .arg(result.totalCount)
                         .arg(result.succeededCount)
                         .arg(result.failedCount);
    if (!result.firstError.isEmpty())
    {
        output.append(tr(" First error: %1").arg(result.firstError));
    }
    return output;
}

// 목적: console output 영역에 표시할 RAW diagnostics 결과 formatting
// 입력: result: RAW diagnostics service가 반환한 실행 결과
// 출력: console output 문자열
QString ConsoleCommandController::formatRawDiagnosticsResult(const RawDiagnosticsExecutionResult& result) const
{
    if (!result.succeeded)
    {
        return tr("RAW diagnostics failed: %1").arg(result.errorMessage);
    }

    QString orientation;
    switch (result.diagnostics.appliedOrientation)
    {
    case core::raw::RawPreviewOrientation::AsDecoded:
        orientation = tr("as decoded");
        break;
    case core::raw::RawPreviewOrientation::Rotate90CounterClockwise:
        orientation = tr("rotate 90 degrees counterclockwise");
        break;
    case core::raw::RawPreviewOrientation::Rotate90Clockwise:
        orientation = tr("rotate 90 degrees clockwise");
        break;
    }

    return tr("RAW diagnostics: flip %1, decoded %2x%3, processed %4x%5, applied orientation %6.")
        .arg(result.diagnostics.sourceFlip)
        .arg(result.diagnostics.decodedSize.width())
        .arg(result.diagnostics.decodedSize.height())
        .arg(result.diagnostics.processedSize.width())
        .arg(result.diagnostics.processedSize.height())
        .arg(orientation);
}

}  // namespace flexraw::ui::cli
