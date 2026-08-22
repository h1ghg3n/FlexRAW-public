#include "console_command_controller.h"

#include <utility>

#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QtConcurrentRun>

#include "catalog_orchestrator.h"
#include "export_orchestrator.h"
#include "export_settings.h"
#include "folder_scanner.h"

namespace flexraw::ui::cli
{
namespace
{

// 목적: console path와 command 종류를 Core file descriptor로 변환
// 입력: path: 사용자가 입력한 source 경로, kind: command가 확정한 RAW 또는 raster 종류
// 출력: 정규화된 절대 경로와 file 이름을 포함한 descriptor
[[nodiscard]] core::types::FileDescriptor makeSourceDescriptor(const QString& path, core::types::SupportedFileKind kind)
{
    const QFileInfo fileInfo(QDir::cleanPath(path));
    return core::types::makeFileDescriptor(fileInfo, kind);
}

// 목적: export report에서 첫 item failure technical message 조회
// 입력: report: item별 성공과 오류를 포함한 완료 report
// 출력: 첫 failure message 또는 모든 item이 성공했으면 빈 문자열
[[nodiscard]] QString firstExportError(const core::orchestration::ExportReport& report)
{
    for (const core::orchestration::ExportItemResult& item : report.items)
    {
        if (!item.succeeded)
        {
            return item.error.message;
        }
    }
    return {};
}

// 목적: command target과 active catalog가 동일한 file을 가리키는지 확인
// 입력: left/right: 비교할 catalog locator
// 출력: 두 locator가 같은 file이면 true
[[nodiscard]] bool isSameCatalogFile(const QString& left, const QString& right)
{
    return !left.trimmed().isEmpty() && !right.trimmed().isEmpty() && QFileInfo(left) == QFileInfo(right);
}

}  // namespace

// 목적: 기본 application settings를 사용하는 console command controller 초기화
// 입력: catalogOrchestrator: active catalog use case, exportOrchestrator: shared export use case, parent: Qt 부모 객체
// 출력: 초기화된 ConsoleCommandController 객체
ConsoleCommandController::ConsoleCommandController(core::orchestration::CatalogOrchestrator& catalogOrchestrator,
                                                   core::orchestration::ExportOrchestrator& exportOrchestrator,
                                                   QObject* parent)
    : QObject(parent),
      m_ownedSettings(std::make_unique<QSettings>()),
      m_catalogOrchestrator(&catalogOrchestrator),
      m_exportOrchestrator(&exportOrchestrator),
      m_settings(m_ownedSettings.get())
{
    connectExportOrchestrator();
}

// 목적: 외부 settings 저장소를 사용하는 console command controller 초기화
// 입력: catalogOrchestrator: active catalog use case, exportOrchestrator: export use case, settings: 외부 settings
// 출력: 초기화된 ConsoleCommandController 객체
ConsoleCommandController::ConsoleCommandController(core::orchestration::CatalogOrchestrator& catalogOrchestrator,
                                                   core::orchestration::ExportOrchestrator& exportOrchestrator,
                                                   QSettings& settings,
                                                   QObject* parent)
    : QObject(parent),
      m_catalogOrchestrator(&catalogOrchestrator),
      m_exportOrchestrator(&exportOrchestrator),
      m_settings(&settings)
{
    connectExportOrchestrator();
}

// 목적: shared ExportOrchestrator terminal event를 current console request handler에 연결
// 입력: 없음
// 출력: QObject signal 연결 생성
void ConsoleCommandController::connectExportOrchestrator()
{
    connect(m_exportOrchestrator,
            &core::orchestration::ExportOrchestrator::exportCompleted,
            this,
            &ConsoleCommandController::handleExportCompleted);
    connect(m_exportOrchestrator,
            &core::orchestration::ExportOrchestrator::exportFailed,
            this,
            &ConsoleCommandController::handleExportFailed);
    connect(m_exportOrchestrator,
            &core::orchestration::ExportOrchestrator::exportCancelled,
            this,
            &ConsoleCommandController::handleExportCancelled);
}

bool ConsoleCommandController::tryExecute(const QString& commandLine)
{
    const RawDiagnosticsCommandParseResult rawDiagnosticsParsed = RawDiagnosticService::parseRawCommand(commandLine);
    if (rawDiagnosticsParsed.recognized)
    {
        return tryExecuteRawDiagnostics(rawDiagnosticsParsed);
    }

    const core::export_::RasterExportOptions defaultOptions =
        settings::ExportSettings(*m_settings).loadRasterDefaults();
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

    const core::orchestration::CatalogSessionState catalogState = m_catalogOrchestrator->state();
    if (!catalogState.isOpen)
    {
        emit outputReady(tr("Catalog import failed: No catalog is open."));
        return true;
    }
    if (!isSameCatalogFile(catalogState.catalogPath, parsed.command.catalogPath))
    {
        emit outputReady(tr("Catalog import failed: Open the requested catalog before importing."));
        return true;
    }

    m_busy = true;
    emit busyChanged(true);

    auto* watcher = new QFutureWatcher<core::catalog::CatalogScanResult>(this);
    connect(watcher,
            &QFutureWatcher<core::catalog::CatalogScanResult>::finished,
            this,
            [this, watcher, expectedCatalogPath = catalogState.catalogPath] {
                finishCatalogImport(expectedCatalogPath, watcher->result());
                watcher->deleteLater();
            });
    watcher->setFuture(
        QtConcurrent::run([folderPath = parsed.command.folderPath] { return core::catalog::scanFolder(folderPath); }));
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

    const core::orchestration::ExportFileRequest request{
        makeSourceDescriptor(parsed.command.inputPath, core::types::SupportedFileKind::RasterImage),
        parsed.command.outputPath,
        parsed.command.catalogPath,
        {},
        parsed.command.options,
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

    const core::orchestration::ExportFileRequest request{
        makeSourceDescriptor(parsed.command.inputPath, core::types::SupportedFileKind::Raw),
        parsed.command.outputPath,
        parsed.command.catalogPath,
        {},
        parsed.command.options,
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

    const core::orchestration::ExportBatchRequest request{
        parsed.command.inputFolderPath,
        parsed.command.outputFolderPath,
        parsed.command.catalogPath,
        {},
        parsed.command.workerCount,
        parsed.command.options,
    };
    return submitExport(request, parsed.command.options, PendingExportKind::Batch);
}

// 목적: Core export request를 submit하고 accepted request state를 console adapter에 연결
// 입력: request: Core file/batch request, options: 성공 시 저장할 기본값, kind: 결과 formatting 종류
// 출력: command 처리 완료를 나타내는 true
bool ConsoleCommandController::submitExport(core::orchestration::ExportRequest request,
                                            const core::export_::RasterExportOptions& options,
                                            PendingExportKind kind)
{
    const core::orchestration::ExportSubmissionResult submitted =
        m_exportOrchestrator->submitExport(std::move(request));
    if (submitted.hasError())
    {
        if (kind == PendingExportKind::Batch)
        {
            emit outputReady(formatBatchExportResult({false, 0, 0, 0, submitted.error().message}));
        }
        else
        {
            emit outputReady(formatRasterExportResult({false, {}, submitted.error().message}));
        }
        return true;
    }

    m_pendingExport = PendingExport{submitted.value(), kind, options};
    m_busy = true;
    emit busyChanged(true);
    return true;
}

// 목적: 현재 console export와 일치하는 완료 report를 표시하고 settings 갱신
// 입력: result: request identity와 item별 상세 결과
// 출력: outputReady와 busyChanged signal 발생 가능
void ConsoleCommandController::handleExportCompleted(const core::orchestration::ExportResult& result)
{
    if (!m_pendingExport.has_value() || m_pendingExport->requestId != result.requestId)
    {
        return;
    }

    if (m_pendingExport->kind == PendingExportKind::Batch)
    {
        settings::ExportSettings(*m_settings).saveRasterDefaults(m_pendingExport->options);
        emit outputReady(formatBatchExportResult({true,
                                                  result.report.totalCount,
                                                  result.report.succeededCount,
                                                  result.report.failedCount,
                                                  firstExportError(result.report)}));
    }
    else
    {
        RasterExportCommandExecutionResult executionResult{
            false,
            {},
            tr("Export pipeline returned no item result."),
        };
        if (!result.report.items.isEmpty())
        {
            const core::orchestration::ExportItemResult& item = result.report.items.constFirst();
            executionResult = {item.succeeded, item.outputPath, item.error.message};
        }
        if (executionResult.succeeded)
        {
            settings::ExportSettings(*m_settings).saveRasterDefaults(m_pendingExport->options);
        }
        emit outputReady(formatRasterExportResult(executionResult));
    }
    finishExport();
}

// 목적: 현재 console export의 terminal pipeline 실패를 사용자 출력으로 변환
// 입력: issue: request identity와 technical error
// 출력: outputReady와 busyChanged signal 발생 가능
void ConsoleCommandController::handleExportFailed(const core::orchestration::ExportIssue& issue)
{
    if (!m_pendingExport.has_value() || m_pendingExport->requestId != issue.requestId)
    {
        return;
    }

    if (m_pendingExport->kind == PendingExportKind::Batch)
    {
        emit outputReady(formatBatchExportResult({false, 0, 0, 0, issue.error.message}));
    }
    else
    {
        emit outputReady(formatRasterExportResult({false, {}, issue.error.message}));
    }
    finishExport();
}

// 목적: 현재 console export cancellation을 표시하고 입력 상태 복원
// 입력: requestId: 취소된 export request 식별자
// 출력: outputReady와 busyChanged signal 발생 가능
void ConsoleCommandController::handleExportCancelled(core::types::RequestId requestId)
{
    if (!m_pendingExport.has_value() || m_pendingExport->requestId != requestId)
    {
        return;
    }

    emit outputReady(tr("Export cancelled."));
    finishExport();
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

// 목적: worker scan 완료 후 active catalog를 재검증하고 owner thread에서 import
// 입력: expectedCatalogPath: scan 시작 session, scanResult: worker의 immutable scan 결과
// 출력: outputReady와 busyChanged signal 발생
void ConsoleCommandController::finishCatalogImport(const QString& expectedCatalogPath,
                                                   const core::catalog::CatalogScanResult& scanResult)
{
    QString output;
    if (scanResult.hasError())
    {
        output = tr("Catalog import failed: %1").arg(scanResult.error().message);
    }
    else if (!isSameCatalogFile(m_catalogOrchestrator->state().catalogPath, expectedCatalogPath))
    {
        output = tr("Catalog import failed: The active catalog changed while the folder was being scanned.");
    }
    else
    {
        output = formatImportResult(m_catalogOrchestrator->importScannedEntries(scanResult.value()));
    }

    emit outputReady(output);
    m_busy = false;
    emit busyChanged(false);
}

// 목적: CatalogOrchestrator import 결과를 사용자 표시 문자열로 변환
// 입력: result: active session에 적용한 import 결과
// 출력: console output 문자열
QString ConsoleCommandController::formatImportResult(const core::orchestration::CatalogImportResult& result) const
{
    if (result.hasError())
    {
        return tr("Catalog import failed: %1").arg(result.error().message);
    }

    return tr("Catalog import complete: scanned %1, stored %2.")
        .arg(result.value().scannedCount)
        .arg(result.value().storedCount);
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
