#pragma once

#include <optional>

#include <QDialog>
#include <QElapsedTimer>
#include <QVector>

#include "editor_contracts.h"
#include "export_client.h"
#include "export_defaults_client.h"
#include "export_options.h"
#include "worker_profile_client.h"

class QCheckBox;
class QCloseEvent;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTableWidget;
class QTimer;
class QToolButton;

namespace flexraw::ui::export_
{

struct ExportDialogSource
{
    core::types::FileDescriptor source;
    QString catalogPath;
    std::optional<core::types::DevelopParams> developParams;
    bool useDefaultDevelopParamsWhenCatalogPhotoMissing{false};
};

class ExportDialog final : public QDialog
{
    Q_OBJECT

public:
    // 목적: 현재 Editor snapshot을 통합 Local/Remote/Auto export 실행 경계에 연결
    // 입력: Export command/event contract, 현재 Editor state, Worker/profile default client와 Qt parent
    // 출력: 세 placement mode를 제출할 수 있는 graphical dialog 객체
    explicit ExportDialog(core::client::IExportClient& exportClient,
                          core::client::IExportEventSource& exportEventSource,
                          const core::orchestration::EditorState& editorState,
                          core::client::IWorkerProfileClient& workerProfileClient,
                          core::client::IExportDefaultsClient& exportDefaultsClient,
                          QWidget* parent = nullptr);

    // 목적: 선택된 여러 photo source를 editable output 목록과 통합 placement 경계에 연결
    // 입력: Export command/event contract, source별 develop resolution 정보, Worker/profile default client와 Qt parent
    // parent 출력: item 단위 Local/Remote/Auto scheduling을 제출할 graphical dialog 객체
    explicit ExportDialog(core::client::IExportClient& exportClient,
                          core::client::IExportEventSource& exportEventSource,
                          QVector<ExportDialogSource> sources,
                          core::client::IWorkerProfileClient& workerProfileClient,
                          core::client::IExportDefaultsClient& exportDefaultsClient,
                          QWidget* parent = nullptr);

    // 목적: dialog 종료 중 active export가 남아 있으면 cancellation 요청
    // 입력: 없음
    // 출력: dialog가 소유한 request publish 중단
    ~ExportDialog() override;

protected:
    // 목적: window close를 active export cancellation과 함께 처리
    // 입력: event: Qt close event
    // 출력: active request cancellation 후 dialog 닫힘
    void closeEvent(QCloseEvent* event) override;

private:
    // 목적: current widget 값으로 placement-aware single-photo ExportRequest 제출
    // 입력: 없음
    // 출력: accepted request 추적 또는 validation 오류 표시
    void startExport();

    // 목적: output 표의 여러 행을 하나의 item-list ExportRequest로 제출
    // 입력: 없음
    // 출력: accepted request 추적 또는 첫 validation 오류 표시
    void startItemListExport();

    // 목적: active export를 취소하거나 terminal dialog를 닫음
    // 입력: 없음
    // 출력: cancellation 요청 또는 dialog reject
    void cancelOrClose();

    // 목적: 단일 file 또는 다중 item 공통 output folder 선택 dialog 표시
    // 입력: 없음
    // 출력: 선택한 output path를 field에 반영
    void browseOutputPath();

    // 목적: 선택 format에 해당하는 quality/compression control만 표시
    // 입력: 없음
    // 출력: format별 row visibility와 output 확장자 갱신
    void updateFormatControls();

    // 목적: 선택 실행 mode에 따라 manual Remote endpoint control 표시
    // 입력: 없음
    // 출력: Remote mode에서만 endpoint/storage profile editor 표시
    void updateExecutionControls();

    // 목적: Qt-free Export event source를 current dialog request presentation에 연결
    // 입력: eventSource: application-scoped Export lifecycle source
    // 출력: RAII subscription 보관 또는 구조화된 오류 logging
    void subscribeToExportEvents(core::client::IExportEventSource& eventSource);

    // 목적: immutable Export event에서 current request progress와 exact terminal만 처리
    // 입력: event: initial snapshot 또는 accepted/progress/completed/failed/cancelled transition
    // 출력: matching request presentation 갱신
    void handleExportEvent(const core::client::ExportEvent& event);

    // 목적: widget 값을 Core raster export option으로 조립
    // 입력: 없음
    // 출력: Export client command로 투영할 RasterExportOptions
    [[nodiscard]] core::export_::RasterExportOptions collectOptions() const;

    // 목적: widget 값을 저장 가능한 placement mode와 preferred Worker identity로 조립
    // 입력: 없음
    // 출력: 현재 ExportExecutionDefaults snapshot
    [[nodiscard]] core::client::ExportExecutionDefaults collectExecutionDefaults() const;

    // 목적: 비어 있는 profile storage binding을 현재 source/output marker로 best-effort 보완
    // 입력: profile: 선택된 Worker snapshot, outputPath: Desktop absolute output path
    // 출력: 두 marker를 찾으면 profile 저장소와 현재 snapshot 갱신
    void discoverMissingStorageBinding(core::client::WorkerProfileSnapshot& profile, const QString& outputPath) const;

    // 목적: UI 실행 mode와 preferred Worker identity를 Qt-free placement option으로 변환
    // 입력: defaults: 저장 가능한 mode와 optional profile identity
    // 출력: endpoint detail을 포함하지 않는 LocalOnly, RemoteOnly 또는 Auto placement 계약
    [[nodiscard]] core::client::ExportPlacementOptions collectPlacementOptions(
        const core::client::ExportExecutionDefaults& defaults) const;

    // 목적: combo에서 현재 선택된 stable identity를 Worker profile snapshot으로 해석
    // 입력: 없음
    // 출력: 저장된 profile과 일치하면 snapshot, placeholder·missing이면 nullopt
    [[nodiscard]] std::optional<core::client::WorkerProfileSnapshot> selectedWorkerProfile() const;

    // 목적: execution combo의 현재 선택을 typed mode로 변환
    // 입력: 없음
    // 출력: Local, Remote 또는 Auto 실행 mode
    [[nodiscard]] core::client::ExportPlacementPolicy selectedPlacementPolicy() const;

    // 목적: export 실행 여부에 따라 output option control과 action 상태 갱신
    // 입력: enabled: option을 편집할 수 있으면 true
    // 출력: 관련 widget enabled 상태 변경
    void setOptionControlsEnabled(bool enabled);

    // 목적: accepted request의 elapsed timer와 running presentation 시작
    // 입력: requestId: 선택 executor가 발급한 identity, mode/options/outputPath: terminal 처리용 snapshot
    // 출력: active request 추적과 Running 상태 표시
    void beginRequest(core::client::ExportRequestId requestId,
                      core::client::ExportPlacementPolicy placementPolicy,
                      core::export_::RasterExportOptions options,
                      QString outputPath);

    // 목적: terminal request 추적과 elapsed timer 종료
    // 입력: allowRetry: option 수정 후 재제출을 허용하면 true
    // 출력: active identity 제거와 button 상태 갱신
    void finishRequest(bool allowRetry);

    // 목적: current request progress를 사용자용 running 상태에 반영
    // 입력: progress: request identity와 item 누적 상태
    // 출력: 일치하는 request의 status text 갱신
    void handleProgress(const core::client::ExportProgress& progress);

    // 목적: current request item report를 성공 또는 실패 presentation으로 변환
    // 입력: result: request identity와 item별 terminal report
    // 출력: 성공 설정 저장 또는 실패 상세 표시
    void handleCompleted(const core::client::ExportResult& result);

    // 목적: pipeline-level current request 실패를 사용자에게 표시
    // 입력: issue: request identity와 structured error
    // 출력: retry 가능한 Failed 상태 표시
    void handleFailed(const core::client::ExportIssue& issue);

    // 목적: current request cancellation terminal event를 사용자에게 표시
    // 입력: requestId: 취소된 request identity
    // 출력: retry 가능한 Cancelled 상태 표시
    void handleCancelled(core::client::ExportRequestId requestId);

    // 목적: common ClientError와 Export domain failure를 localized presentation으로 변환
    // 입력: error: diagnostic-only detail을 가진 common error, failureKind: optional Export domain 의미
    // 출력: technicalMessage를 직접 노출하지 않는 사용자용 문자열
    [[nodiscard]] QString exportErrorText(
        const core::client::ClientError& error,
        core::client::ExportFailureKind failureKind = core::client::ExportFailureKind::None) const;

    // 목적: active request 경과시간 label 갱신
    // 입력: 없음
    // 출력: 0.1초 단위 elapsed text 표시
    void updateElapsedTime();

    // 목적: 현재 format에 맞는 QFileDialog filter 생성
    // 입력: format: 선택한 raster format
    // 출력: 번역된 file filter 문자열
    [[nodiscard]] QString outputFilter(core::export_::RasterExportFormat format) const;

    core::client::IExportClient* m_exportClient{nullptr};
    core::client::ExportSubscriptionHandle m_exportSubscription;
    core::client::IWorkerProfileClient* m_workerProfileClient{nullptr};
    core::client::IExportDefaultsClient* m_exportDefaultsClient{nullptr};
    std::vector<core::client::WorkerProfileSnapshot> m_workerProfiles;
    QVector<ExportDialogSource> m_sources;
    core::types::FileDescriptor m_source;
    core::types::DevelopParams m_developParams;
    core::client::ExportExecutionDefaults m_executionDefaults;
    QLabel* m_sourceLabel{nullptr};
    QTableWidget* m_outputTable{nullptr};
    QVector<QLineEdit*> m_outputPathEdits;
    QLineEdit* m_outputPathEdit{nullptr};
    QToolButton* m_browseButton{nullptr};
    QComboBox* m_formatCombo{nullptr};
    QComboBox* m_executionCombo{nullptr};
    QWidget* m_remoteSettingsWidget{nullptr};
    QComboBox* m_workerProfileCombo{nullptr};
    QLabel* m_jpegQualityLabel{nullptr};
    QSpinBox* m_jpegQualitySpinBox{nullptr};
    QLabel* m_pngCompressionLabel{nullptr};
    QSpinBox* m_pngCompressionSpinBox{nullptr};
    QLabel* m_tiffCompressionLabel{nullptr};
    QComboBox* m_tiffCompressionCombo{nullptr};
    QSpinBox* m_maximumDimensionSpinBox{nullptr};
    QComboBox* m_outputColorSpaceCombo{nullptr};
    QCheckBox* m_includeMetadataCheckBox{nullptr};
    QLabel* m_statusLabel{nullptr};
    QLabel* m_elapsedLabel{nullptr};
    QPushButton* m_exportButton{nullptr};
    QPushButton* m_cancelButton{nullptr};
    QTimer* m_elapsedTimer{nullptr};
    QElapsedTimer m_elapsedClock;
    std::optional<core::client::ExportRequestId> m_activeRequestId;
    core::export_::RasterExportOptions m_submittedOptions;
    QString m_submittedOutputPath;
};

}  // namespace flexraw::ui::export_
