#pragma once

#include <memory>
#include <optional>

#include <QObject>
#include <QSettings>
#include <QString>

#include "catalog_command_service.h"
#include "catalog_contracts.h"
#include "export_command_service.h"
#include "export_contracts.h"
#include "folder_scanner.h"
#include "raw_diagnostic_service.h"

namespace flexraw::core::orchestration
{
class CatalogOrchestrator;
class ExportOrchestrator;
}  // namespace flexraw::core::orchestration

namespace flexraw::ui::cli
{

class ConsoleCommandController final : public QObject
{
    Q_OBJECT

public:
    // 목적: console backend command의 비동기 실행과 결과 전달을 관리
    // 입력: catalogOrchestrator: active catalog use case, exportOrchestrator: shared export use case, parent: Qt 부모
    // 객체 출력: 초기화된 ConsoleCommandController 객체
    explicit ConsoleCommandController(core::orchestration::CatalogOrchestrator& catalogOrchestrator,
                                      core::orchestration::ExportOrchestrator& exportOrchestrator,
                                      QObject* parent = nullptr);

    // 목적: 외부 settings 저장소를 사용하는 console command controller 초기화
    // 입력: catalogOrchestrator: active catalog use case, exportOrchestrator: export use case, settings: 외부 settings
    // 출력: 초기화된 ConsoleCommandController 객체
    explicit ConsoleCommandController(core::orchestration::CatalogOrchestrator& catalogOrchestrator,
                                      core::orchestration::ExportOrchestrator& exportOrchestrator,
                                      QSettings& settings,
                                      QObject* parent = nullptr);

    // 목적: 인식된 console command를 검증 후 worker thread에서 실행
    // 입력: commandLine: 사용자가 입력한 전체 command 문자열
    // 출력: 지원 command로 처리했는지 여부
    [[nodiscard]] bool tryExecute(const QString& commandLine);

    // 목적: console background command가 실행 중인지 확인
    // 입력: 없음
    // 출력: worker task 실행 여부
    [[nodiscard]] bool isBusy() const noexcept;

signals:
    // 목적: console에 표시할 command 결과 한 줄 전달
    // 입력: line: 번역과 formatting이 완료된 output
    // 출력: 없음
    void outputReady(const QString& line);

    // 목적: command 입력 활성화 상태 갱신 요청 전달
    // 입력: busy: worker task 실행 여부
    // 출력: 없음
    void busyChanged(bool busy);

private:
    enum class PendingExportKind
    {
        File,
        Batch,
    };

    struct PendingExport
    {
        core::types::RequestId requestId{0};
        PendingExportKind kind{PendingExportKind::File};
        core::export_::RasterExportOptions options;
    };

    // 목적: shared ExportOrchestrator terminal event를 current console request handler에 연결
    // 입력: 없음
    // 출력: QObject signal 연결 생성
    void connectExportOrchestrator();

    // 목적: worker scan 완료 후 active catalog를 재검증하고 owner thread에서 import
    // 입력: expectedCatalogPath: scan 시작 session, scanResult: worker의 immutable scan 결과
    // 출력: outputReady와 busyChanged signal 발생
    void finishCatalogImport(const QString& expectedCatalogPath, const core::catalog::CatalogScanResult& scanResult);

    // 목적: CatalogOrchestrator import 결과를 사용자 표시 문자열로 변환
    // 입력: result: active session에 적용한 import 결과
    // 출력: console output 문자열
    [[nodiscard]] QString formatImportResult(const core::orchestration::CatalogImportResult& result) const;

    // 목적: raster export command를 검증하고 worker thread에서 실행
    // 입력: parsed: parse가 완료된 raster export command 결과
    // 출력: export command로 처리했는지 여부
    [[nodiscard]] bool tryExecuteRasterExport(const RasterExportCommandParseResult& parsed);

    // 목적: RAW export command를 검증하고 worker thread에서 실행
    // 입력: parsed: parse가 완료된 RAW export command 결과
    // 출력: RAW export command로 처리했는지 여부
    [[nodiscard]] bool tryExecuteRawExport(const RasterExportCommandParseResult& parsed);

    // 목적: RAW diagnostics command를 검증하고 worker thread에서 실행
    // 입력: parsed: parse가 완료된 RAW diagnostics command 결과
    // 출력: command로 처리했는지 여부
    [[nodiscard]] bool tryExecuteRawDiagnostics(const RawDiagnosticsCommandParseResult& parsed);

    // 목적: batch export command를 검증하고 worker thread에서 실행
    // 입력: parsed: parse가 완료된 batch export command 결과
    // 출력: batch export command로 처리했는지 여부
    [[nodiscard]] bool tryExecuteBatchExport(const BatchExportCommandParseResult& parsed);

    // 목적: Core export request를 submit하고 accepted request state를 console adapter에 연결
    // 입력: request: Core file/batch request, options: 성공 시 저장할 기본값, kind: 결과 formatting 종류
    // 출력: command 처리 완료를 나타내는 true
    [[nodiscard]] bool submitExport(core::orchestration::ExportRequest request,
                                    const core::export_::RasterExportOptions& options,
                                    PendingExportKind kind);

    // 목적: 현재 console export와 일치하는 완료 report를 표시하고 settings 갱신
    // 입력: result: request identity와 item별 상세 결과
    // 출력: outputReady와 busyChanged signal 발생 가능
    void handleExportCompleted(const core::orchestration::ExportResult& result);

    // 목적: 현재 console export의 terminal pipeline 실패를 사용자 출력으로 변환
    // 입력: issue: request identity와 technical error
    // 출력: outputReady와 busyChanged signal 발생 가능
    void handleExportFailed(const core::orchestration::ExportIssue& issue);

    // 목적: 현재 console export cancellation을 표시하고 입력 상태 복원
    // 입력: requestId: 취소된 export request 식별자
    // 출력: outputReady와 busyChanged signal 발생 가능
    void handleExportCancelled(core::types::RequestId requestId);

    // 목적: current export adapter state를 정리하고 console 입력 재활성화
    // 입력: 없음
    // 출력: busyChanged(false) signal 발생
    void finishExport();

    // 목적: console output 영역에 표시할 raster export 결과 formatting
    // 입력: result: export service가 반환한 실행 결과
    // 출력: console output 문자열
    [[nodiscard]] QString formatRasterExportResult(const RasterExportCommandExecutionResult& result) const;

    // 목적: console output 영역에 표시할 batch export 결과 formatting
    // 입력: result: batch export service가 반환한 실행 결과
    // 출력: console output 문자열
    [[nodiscard]] QString formatBatchExportResult(const BatchExportCommandExecutionResult& result) const;

    // 목적: console output 영역에 표시할 RAW diagnostics 결과 formatting
    // 입력: result: RAW diagnostics service가 반환한 실행 결과
    // 출력: console output 문자열
    [[nodiscard]] QString formatRawDiagnosticsResult(const RawDiagnosticsExecutionResult& result) const;

    std::unique_ptr<QSettings> m_ownedSettings;
    core::orchestration::CatalogOrchestrator* m_catalogOrchestrator{nullptr};
    core::orchestration::ExportOrchestrator* m_exportOrchestrator{nullptr};
    QSettings* m_settings{nullptr};
    std::optional<PendingExport> m_pendingExport;
    bool m_busy{false};
};

}  // namespace flexraw::ui::cli
