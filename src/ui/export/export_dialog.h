#pragma once

#include <optional>

#include <QDialog>
#include <QElapsedTimer>
#include <QVector>

#include "editor_contracts.h"
#include "export_contracts.h"
#include "export_options.h"
#include "export_settings.h"

class QCheckBox;
class QCloseEvent;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSettings;
class QSpinBox;
class QTableWidget;
class QTimer;
class QToolButton;

namespace flexraw::core::orchestration
{
class ExportOrchestrator;
}

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
    // 입력: placement authority, 현재 Editor state, settings와 Qt parent
    // 출력: 세 placement mode를 제출할 수 있는 graphical dialog 객체
    explicit ExportDialog(core::orchestration::ExportOrchestrator& exportOrchestrator,
                          const core::orchestration::EditorState& editorState,
                          QSettings& settings,
                          QWidget* parent = nullptr);

    // 목적: 선택된 여러 photo source를 editable output 목록과 통합 placement 경계에 연결
    // 입력: placement authority, source별 develop resolution 정보, settings와 Qt parent
    // 출력: item 단위 Local/Remote/Auto scheduling을 제출할 graphical dialog 객체
    explicit ExportDialog(core::orchestration::ExportOrchestrator& exportOrchestrator,
                          QVector<ExportDialogSource> sources,
                          QSettings& settings,
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

    // 목적: widget 값을 Core raster export option으로 조립
    // 입력: 없음
    // 출력: ExportOrchestrator validation에 전달할 RasterExportOptions
    [[nodiscard]] core::export_::RasterExportOptions collectOptions() const;

    // 목적: widget 값을 저장 가능한 placement mode와 manual Remote 기본값으로 조립
    // 입력: 없음
    // 출력: 현재 ExportExecutionDefaults snapshot
    [[nodiscard]] settings::ExportExecutionDefaults collectExecutionDefaults() const;

    // 목적: 비어 있는 endpoint storage binding을 현재 source/output marker로 best-effort 보완
    // 입력: defaults: endpoint 결합 설정, outputPath: Desktop absolute output path
    // 출력: 두 marker를 모두 찾은 경우에만 expected storage ID 갱신
    void discoverMissingStorageBinding(settings::ExportExecutionDefaults& defaults, const QString& outputPath) const;

    // 목적: UI 실행 mode와 endpoint 설정을 Core placement option으로 변환
    // 입력: defaults: 저장 가능한 mode/endpoint/storage snapshot
    // 출력: LocalOnly, RemoteOnly 또는 Auto placement 계약
    [[nodiscard]] core::orchestration::ExportPlacementOptions collectPlacementOptions(
        const settings::ExportExecutionDefaults& defaults) const;

    // 목적: execution combo의 현재 선택을 typed mode로 변환
    // 입력: 없음
    // 출력: Local, Remote 또는 Auto 실행 mode
    [[nodiscard]] settings::ExportExecutionMode selectedExecutionMode() const;

    // 목적: export 실행 여부에 따라 output option control과 action 상태 갱신
    // 입력: enabled: option을 편집할 수 있으면 true
    // 출력: 관련 widget enabled 상태 변경
    void setOptionControlsEnabled(bool enabled);

    // 목적: accepted request의 elapsed timer와 running presentation 시작
    // 입력: requestId: 선택 executor가 발급한 identity, mode/options/outputPath: terminal 처리용 snapshot
    // 출력: active request 추적과 Running 상태 표시
    void beginRequest(core::types::RequestId requestId,
                      settings::ExportExecutionMode mode,
                      core::export_::RasterExportOptions options,
                      QString outputPath);

    // 목적: terminal request 추적과 elapsed timer 종료
    // 입력: allowRetry: option 수정 후 재제출을 허용하면 true
    // 출력: active identity 제거와 button 상태 갱신
    void finishRequest(bool allowRetry);

    // 목적: current request progress를 사용자용 running 상태에 반영
    // 입력: progress: request identity와 item 누적 상태
    // 출력: 일치하는 request의 status text 갱신
    void handleProgress(const core::orchestration::ExportProgress& progress);

    // 목적: current request item report를 성공 또는 실패 presentation으로 변환
    // 입력: result: request identity와 item별 terminal report
    // 출력: 성공 설정 저장 또는 실패 상세 표시
    void handleCompleted(const core::orchestration::ExportResult& result);

    // 목적: pipeline-level current request 실패를 사용자에게 표시
    // 입력: issue: request identity와 structured error
    // 출력: retry 가능한 Failed 상태 표시
    void handleFailed(const core::orchestration::ExportIssue& issue);

    // 목적: current request cancellation terminal event를 사용자에게 표시
    // 입력: requestId: 취소된 request identity
    // 출력: retry 가능한 Cancelled 상태 표시
    void handleCancelled(core::types::RequestId requestId);

    // 목적: active request 경과시간 label 갱신
    // 입력: 없음
    // 출력: 0.1초 단위 elapsed text 표시
    void updateElapsedTime();

    // 목적: 현재 format에 맞는 QFileDialog filter 생성
    // 입력: format: 선택한 raster format
    // 출력: 번역된 file filter 문자열
    [[nodiscard]] QString outputFilter(core::export_::RasterExportFormat format) const;

    core::orchestration::ExportOrchestrator* m_exportOrchestrator{nullptr};
    QVector<ExportDialogSource> m_sources;
    core::types::FileDescriptor m_source;
    core::types::DevelopParams m_developParams;
    settings::ExportSettings m_exportSettings;
    settings::ExportExecutionDefaults m_executionDefaults;
    QLabel* m_sourceLabel{nullptr};
    QTableWidget* m_outputTable{nullptr};
    QVector<QLineEdit*> m_outputPathEdits;
    QLineEdit* m_outputPathEdit{nullptr};
    QToolButton* m_browseButton{nullptr};
    QComboBox* m_formatCombo{nullptr};
    QComboBox* m_executionCombo{nullptr};
    QWidget* m_remoteSettingsWidget{nullptr};
    QLineEdit* m_remoteHostEdit{nullptr};
    QSpinBox* m_remotePortSpinBox{nullptr};
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
    std::optional<core::types::RequestId> m_activeRequestId;
    core::export_::RasterExportOptions m_submittedOptions;
    QString m_submittedOutputPath;
};

}  // namespace flexraw::ui::export_
