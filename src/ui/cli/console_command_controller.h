#pragma once

#include <optional>

#include <QObject>
#include <QString>

#include "catalog_command_service.h"
#include "export_client.h"
#include "export_command_service.h"
#include "export_defaults_client.h"
#include "folder_import_client.h"
#include "raw_diagnostic_service.h"

namespace flexraw::ui::cli
{

class ConsoleCommandController final : public QObject
{
    Q_OBJECT

public:
    // 목적: console backend command의 비동기 실행과 결과 전달을 관리
    // 입력: Folder/Export command/event와 application-wide Export default contract, parent: Qt 부모 객체
    // 출력: 초기화된 ConsoleCommandController 객체
    explicit ConsoleCommandController(core::client::IFolderImportClient& folderImportClient,
                                      core::client::IFolderImportEventSource& folderImportEventSource,
                                      core::client::IExportClient& exportClient,
                                      core::client::IExportEventSource& exportEventSource,
                                      core::client::IExportDefaultsClient& exportDefaultsClient,
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
        core::client::ExportRequestId requestId;
        PendingExportKind kind{PendingExportKind::File};
        core::export_::RasterExportOptions options;
    };

    // 목적: Qt-free Export event source를 current console request handler에 연결
    // 입력: eventSource: application-scoped Export lifecycle source
    // 출력: RAII subscription 저장 또는 diagnostic logging
    void connectExportEvents(core::client::IExportEventSource& eventSource);

    // 목적: immutable Export event에서 current console request의 exact terminal만 처리
    // 입력: event: initial snapshot 또는 accepted/progress/completed/failed/cancelled transition
    // 출력: matching terminal이면 outputReady와 busyChanged(false) 발생
    void handleExportEvent(const core::client::ExportEvent& event);

    // 목적: Folder operation event source를 current console request handler에 연결
    // 입력: eventSource: MainWindow와 공유하는 Qt delivery adapter
    // 출력: RAII subscription 저장 또는 diagnostic logging
    void connectFolderOperationEvents(core::client::IFolderImportEventSource& eventSource);

    // 목적: current console Folder import와 일치하는 terminal을 사용자 출력으로 변환
    // 입력: event: initial/active/terminal Folder operation event
    // 출력: matching terminal이면 outputReady와 busyChanged(false) 발생
    void handleFolderOperationEvent(const core::client::FolderOperationEvent& event);

    // 목적: Folder import completion을 사용자 표시 문자열로 변환
    // 입력: completion: discovered/applied count와 persisted Photo identity
    // 출력: console output 문자열
    [[nodiscard]] QString formatImportResult(const core::client::FolderImportCompletion& completion) const;

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
    [[nodiscard]] bool submitExport(core::client::ExportRequest request,
                                    const core::export_::RasterExportOptions& options,
                                    PendingExportKind kind);

    // 목적: 현재 console export와 일치하는 완료 report를 표시하고 settings 갱신
    // 입력: result: request identity와 item별 상세 결과
    // 출력: outputReady와 busyChanged signal 발생 가능
    void handleExportCompleted(const core::client::ExportResult& result);

    // 목적: 현재 console export의 terminal pipeline 실패를 사용자 출력으로 변환
    // 입력: issue: request identity와 technical error
    // 출력: outputReady와 busyChanged signal 발생 가능
    void handleExportFailed(const core::client::ExportIssue& issue);

    // 목적: 현재 console export cancellation을 표시하고 입력 상태 복원
    // 입력: requestId: 취소된 export request 식별자
    // 출력: outputReady와 busyChanged signal 발생 가능
    void handleExportCancelled(core::client::ExportRequestId requestId);

    // 목적: common ClientError와 Export domain failure를 console user-facing text로 변환
    // 입력: error: diagnostic-only detail, failureKind: optional Export domain 의미
    // 출력: technicalMessage를 직접 노출하지 않는 localized 문자열
    [[nodiscard]] QString exportErrorText(
        const core::client::ClientError& error,
        core::client::ExportFailureKind failureKind = core::client::ExportFailureKind::None) const;

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

    core::client::IFolderImportClient* m_folderImportClient{nullptr};
    core::client::IExportClient* m_exportClient{nullptr};
    core::client::IExportDefaultsClient* m_exportDefaultsClient{nullptr};
    core::client::FolderOperationSubscriptionHandle m_folderOperationSubscription;
    core::client::ExportSubscriptionHandle m_exportSubscription;
    std::optional<core::client::FolderOperationId> m_pendingFolderOperation;
    std::optional<PendingExport> m_pendingExport;
    bool m_busy{false};
};

}  // namespace flexraw::ui::cli
