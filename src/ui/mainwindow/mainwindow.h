#pragma once

#include <optional>

#include <QHash>
#include <QMainWindow>

#include "activity_client.h"
#include "catalog_entry.h"
#include "catalog_folder_client.h"
#include "catalog_photo_client.h"
#include "catalog_project.h"
#include "catalog_project_client.h"
#include "catalog_session_client.h"
#include "catalog_startup_settings_client.h"
#include "catalog_thumbnail_client.h"
#include "develop_params.h"
#include "editor_client.h"
#include "editor_event_client.h"
#include "export_client.h"
#include "export_defaults_client.h"
#include "folder_import_client.h"
#include "preview_presentation_client.h"
#include "source_resolution_client.h"
#include "worker_health_client.h"
#include "worker_profile_client.h"

namespace flexraw::ui::catalog
{
class CatalogListWidget;
class SourceResolutionWidget;
}  // namespace flexraw::ui::catalog

namespace flexraw::ui::editor
{
class DevelopPanel;
class PreviewWidget;
}  // namespace flexraw::ui::editor

namespace flexraw::ui::cli
{
class ConsoleModeWidget;
}

namespace flexraw::core::orchestration
{
struct EditorState;
}  // namespace flexraw::core::orchestration

namespace flexraw::ui::facade
{
class CatalogEditorFacade;
}

namespace flexraw::ui::mainwindow
{
class QtActivityAdapter;
}  // namespace flexraw::ui::mainwindow

class QAction;
class QCloseEvent;
class QComboBox;
class QLabel;
class QProgressBar;
class QSettings;
class QStackedWidget;
class QTimer;
class QToolButton;

namespace flexraw::ui::mainwindow
{

class MainWindow : public QMainWindow
{
public:
    // 목적: Flexraw 첫 catalog-to-preview window를 editor session adapter로 초기화
    // 입력: catalogEditorFacade: GUI와 Folder lifecycle boundary,
    //       Export command/event/default contract, applicationSettings/profile client, optional Worker health와
    //       Catalog startup settings capability, parent: Qt 부모 widget
    // 출력: 초기화된 MainWindow 객체
    explicit MainWindow(facade::CatalogEditorFacade& catalogEditorFacade,
                        core::client::IExportClient& exportClient,
                        core::client::IExportEventSource& exportEventSource,
                        core::client::IExportDefaultsClient& exportDefaultsClient,
                        QSettings& applicationSettings,
                        core::client::IWorkerProfileClient& workerProfileClient,
                        core::client::IWorkerHealthClient* workerHealthClient = nullptr,
                        core::client::IWorkerHealthEventSource* workerHealthEventSource = nullptr,
                        core::client::ICatalogStartupSettingsClient* catalogStartupSettingsClient = nullptr,
                        QWidget* parent = nullptr);

protected:
    // 목적: explicit save policy에서 dirty catalog edit의 종료 전 저장 여부 확인
    // 입력: event: Qt window close event
    // 출력: 저장·폐기 선택 시 종료, 취소·저장 실패 시 종료 중단
    void closeEvent(QCloseEvent* event) override;

private:
    // 목적: status bar의 compact Activity label, indeterminate progress와 cancel control 구성
    // 입력: 없음
    // 출력: 초기에는 숨겨진 Activity presentation widget 생성
    void initializeActivityStatus();

    // 목적: Qt-free Activity client를 실제 MainWindow presentation consumer에 연결
    // 입력: 없음
    // 출력: RAII subscription 보관 또는 구조화된 오류 logging
    void subscribeToActivityEvents();

    // 목적: Qt-free Folder operation event source를 MainWindow presentation에 연결
    // 입력: 없음
    // 출력: RAII subscription 보관 또는 구조화된 오류 logging
    void subscribeToFolderOperationEvents();

    // 목적: Qt-free Preview presentation event source를 MainWindow view에 연결
    // 입력: 없음
    // 출력: RAII subscription 보관 또는 구조화된 오류 logging
    void subscribeToPreviewPresentationEvents();

    // 목적: Qt-free Catalog thumbnail event source를 MainWindow view에 연결
    // 입력: 없음
    // 출력: RAII subscription 보관 또는 구조화된 오류 logging
    void subscribeToCatalogThumbnailEvents();

    // 목적: Qt-free Source Resolution event source를 MainWindow presentation에 연결
    // 입력: 없음
    // 출력: RAII subscription 보관 또는 구조화된 오류 logging
    void subscribeToSourceResolutionEvents();

    // 목적: immutable Preview snapshot/event를 image·analysis·status presentation에 반영
    // 입력: event: initial state, frame update, warning 또는 terminal
    // 출력: PreviewWidget과 DevelopPanel 표시 갱신
    void handlePreviewPresentationEvent(const core::client::PreviewPresentationEvent& event);

    // 목적: immutable Catalog thumbnail frame/issue/terminal을 list presentation에 반영
    // 입력: event: initial state 또는 window lifecycle transition
    // 출력: current generation row icon/terminal 상태 갱신
    void handleCatalogThumbnailEvent(const core::client::CatalogThumbnailEvent& event);

    // 목적: initial/active/terminal Folder lifecycle을 MainWindow 상태로 투영
    // 입력: event: immutable Folder operation event
    // 출력: action enablement와 scan/import presentation 갱신
    void handleFolderOperationEvent(const core::client::FolderOperationEvent& event);

    // 목적: accepted Folder scan/import 시작 상태를 action과 preview presentation에 반영
    // 입력: receipt: operation kind와 normalized folder path
    // 출력: 중복 command 차단과 scan 종류별 status 표시
    void handleFolderOperationStarted(const core::client::FolderOperationReceipt& receipt);

    // 목적: Folder scan/import exact terminal을 list 또는 Catalog navigation에 적용
    // 입력: terminal: completion snapshot 또는 구조화된 실패
    // 출력: action 복원과 scan/import 결과 표시
    void handleFolderOperationTerminal(const core::client::FolderOperationTerminal& terminal);

    // 목적: immutable active 목록을 status bar progress와 owner cancel target에 투영
    // 입력: event: initial 또는 lifecycle transition 뒤 Activity snapshot
    // 출력: active 여부와 cancellability에 맞는 compact status UI
    void updateActivityUi(const core::client::ActivityEvent& event);

    // 목적: status bar에 선택된 cancellable Activity를 실제 owner에서 취소
    // 입력: 없음
    // 출력: cancellation command 전달 또는 non-modal 실패 안내
    void cancelDisplayedActivity();

    // 목적: 새 catalog file 경로를 선택하고 migration된 빈 catalog 생성
    // 입력: 없음
    // 출력: 없음
    void createCatalog();

    // 목적: catalog file 선택 dialog를 열고 stable PhotoId 목록을 현재 window에 연결
    // 입력: 없음
    // 출력: 없음
    void openCatalog();

    // 목적: 지정 catalog를 단일 active session으로 열고 stable PhotoId 목록 표시
    // 입력: catalogPath: 생성하거나 열 catalog file 경로, openMode: explicit create/open 의도
    // 출력: catalog와 photo list를 모두 열었으면 true
    [[nodiscard]] bool openCatalogPath(const QString& catalogPath, core::client::CatalogOpenMode openMode);

    // 목적: 이미 열린 Catalog의 photo 목록과 관련 action을 현재 window에 반영
    // 입력: 없음
    // 출력: photo 목록을 정상 조회해 표시했으면 true
    [[nodiscard]] bool refreshCatalogPhotos();

    // 목적: active Catalog의 distinct Folder scope를 navigation control에 반영
    // 입력: 없음
    // 출력: Folder summary 조회와 control 갱신에 성공하면 true
    [[nodiscard]] bool refreshCatalogFolders();

    // 목적: active Catalog Project 목록을 navigation control에 반영
    // 입력: preferredProjectId: refresh 뒤 유지할 optional Project identity
    // 출력: Project 조회와 control 갱신에 성공하면 true
    [[nodiscard]] bool refreshCatalogProjects(
        std::optional<core::catalog::ProjectId> preferredProjectId = std::nullopt);

    // 목적: 사용자가 선택한 전체 Catalog 또는 exact Folder scope로 photo page 전환
    // 입력: index: Folder scope combo box의 선택 index
    // 출력: active adjustment와 dirty state 정리 후 선택 scope의 첫 page 표시
    void changeCatalogFolderScope(int index);

    // 목적: 사용자가 선택한 Catalog/Folder 또는 Project scope로 photo page 전환
    // 입력: index: Project scope combo box의 선택 index
    // 출력: active adjustment와 dirty state 정리 후 선택 scope의 첫 page 표시
    void changeCatalogProjectScope(int index);

    // 목적: 이름 입력을 받아 active Catalog에 Project 생성
    // 입력: 없음
    // 출력: 생성 성공 시 새 Project scope로 이동
    void createProject();

    // 목적: 현재 Project의 표시 이름 변경
    // 입력: 없음
    // 출력: 변경 성공 시 Project navigation 갱신
    void renameCurrentProject();

    // 목적: 현재 Project와 membership 삭제
    // 입력: 없음
    // 출력: 사용자 확인 후 Photo를 보존하고 Catalog scope로 이동
    void removeCurrentProject();

    // 목적: 현재 multi-selection Photo를 사용자가 고른 Project에 추가
    // 입력: 없음
    // 출력: photo별 idempotent membership command 결과를 status에 표시
    void addSelectedPhotosToProject();

    // 목적: 현재 multi-selection Photo를 active Project에서 제거
    // 입력: 없음
    // 출력: membership 제거 후 active Project 첫 page reload
    void removeSelectedPhotosFromProject();

    // 목적: Catalog session, active Project와 selected Photo에 맞춰 Project action 상태 갱신
    // 입력: 없음
    // 출력: 유효한 Project command만 활성화
    void updateProjectActions();

    // 목적: cursor 요청에 해당하는 bounded Catalog photo page를 현재 목록에 적용
    // 입력: request: page 크기, 이동 방향과 exclusive cursor
    // 출력: 조회와 UI 적용에 성공하면 true
    [[nodiscard]] bool loadCatalogPhotoPage(const core::client::CatalogPhotoPageRequest& request);

    // 목적: 현재 첫 record 이전의 Catalog photo page로 이동
    // 입력: 없음
    // 출력: dirty state 저장 후 이전 page 적용
    void showPreviousCatalogPage();

    // 목적: 현재 마지막 record 다음의 Catalog photo page로 이동
    // 입력: 없음
    // 출력: dirty state 저장 후 다음 page 적용
    void showNextCatalogPage();

    // 목적: Catalog page cursor state와 navigation control 초기화
    // 입력: 없음
    // 출력: 현재 page가 제거되고 navigation button 비활성화
    void resetCatalogPhotoPage();

    // 목적: 현재 page의 이전·다음 cursor 존재 여부를 button 상태에 반영
    // 입력: 없음
    // 출력: 유효한 방향의 navigation button만 활성화
    void updateCatalogPageActions();

    // 목적: 현재 열린 catalog에 추가할 photo folder를 선택하고 background scan 시작
    // 입력: 없음
    // 출력: 없음
    void importFolder();

    // 목적: 현재 Catalog snapshot을 baseline으로 Folder import command 제출
    // 입력: folderPath: file dialog에서 선택한 folder
    // 출력: accepted lifecycle 또는 non-modal submission 실패 표시
    void submitFolderImport(const QString& folderPath);

    // 목적: folder 선택 dialog를 열고 선택된 folder scan 시작
    // 입력: 없음
    // 출력: 없음
    void openFolder();

    // 목적: UI thread를 차단하지 않고 지정 folder의 catalog scan 시작
    // 입력: folderPath: scan할 folder 경로
    // 출력: 비동기 scan 시작
    void loadFolder(const QString& folderPath);

    // 목적: 선택된 Folder photo를 active Catalog에 등록·resolve하고 Editor에 연결
    // 입력: entry: 사용자가 선택한 CatalogEntry 값
    // 출력: stable PhotoId activation 성공 시 preview 예약, 실패 시 이전 선택 복원
    void showSelectedEntry(const core::catalog::CatalogEntry& entry);

    // 목적: 선택된 catalog-backed photo를 persisted develop state와 함께 editor에 연결
    // 입력: photo: stable PhotoId와 source binding을 포함한 catalog record
    // 출력: 없음
    void showSelectedCatalogPhoto(const core::catalog::CatalogPhotoRecord& photo);

    // 목적: 선택 photo의 replacement source를 기존 identity의 새 baseline으로 수용
    // 입력: 없음
    // 출력: accepted source request를 inline pending 상태로 표시
    void acceptReplacement();

    // 목적: 선택 photo의 replacement source를 새 identity로 등록
    // 입력: 없음
    // 출력: 기존 dirty state 저장 후 accepted source request 표시
    void registerReplacementAsNew();

    // 목적: 선택 photo와 동일한 content의 다른 source locator 선택
    // 입력: 없음
    // 출력: file 선택 후 accepted relink request 표시
    void relinkSource();

    // 목적: source resolution command 제출 결과를 공통 inline 상태로 변환
    // 입력: result: request ID 또는 오류, failureMessage: 사용자용 실패 안내
    // 출력: request가 accepted되면 true
    [[nodiscard]] bool beginSourceResolution(const core::client::SourceRequestResult& result,
                                             const QString& failureMessage);

    // 목적: immutable source request lifecycle을 inline control과 Catalog presentation에 반영
    // 입력: event: initial active requests 또는 accepted·update·issue·terminal transition
    // 출력: pending 상태와 source transition 결과 갱신
    void handleSourceResolutionEvent(const core::client::SourceResolutionEvent& event);

    // 목적: source transition을 현재 Editor selection과 catalog list에 반영
    // 입력: update: 갱신된 기존 photo와 optional 신규 photo
    // 출력: 현재 selection의 Register as New이면 신규 PhotoId로 전환
    void handleSourceBindingUpdated(const core::client::SourceResolutionUpdate& update);

    // 목적: current photo의 source verification/resolution 실패를 inline 상태로 표시
    // 입력: issue: request·photo identity와 technical 오류
    // 출력: pending 해제와 사용자용 실패 안내
    void handleSourceBindingFailed(const core::client::SourceResolutionIssue& issue);

    // 목적: source request terminal cancellation을 inline 상태에 반영
    // 입력: requestId: 취소된 request identity
    // 출력: pending 상태 해제
    void handleSourceBindingCancelled(const core::client::SourceResolutionTerminal& terminal);

    // 목적: 현재 catalog-backed photo의 develop state를 explicit user command로 저장
    // 입력: 없음
    // 출력: 성공 시 persisted baseline 갱신, 실패 시 non-modal 상태 표시
    void saveCurrentPhoto();

    // 목적: 현재 Editor snapshot을 Local graphical export dialog에 연결
    // 입력: 없음
    // 출력: 선택 photo가 처리 가능하면 modal export workflow 실행
    void exportCurrentPhoto();

    // 목적: 현재 catalog-backed photo 저장 command를 실행하고 결과를 UI에 표시
    // 입력: 없음
    // 출력: 저장 성공 또는 저장할 dirty state가 없으면 true
    [[nodiscard]] bool persistCurrentPhoto();

    // 목적: catalog 전환 또는 window 종료 전에 dirty edit의 저장·폐기·취소 의사 확인
    // 입력: 없음
    // 출력: 호출자가 전환을 계속해도 되면 true
    [[nodiscard]] bool confirmPendingSave();

    // 목적: 중앙 UI를 graphical 또는 console mode로 전환
    // 입력: enabled: console mode 활성화 여부
    // 출력: 없음
    void setConsoleMode(bool enabled);

    // 목적: application UI preference와 product setting을 편집하는 Settings dialog 표시
    // 입력: 없음
    // 출력: accept 시 Export 기본값 저장과 adjustment style 즉시 반영
    void openSettings();

    // 목적: 현재 선택 사진에 대한 develop parameter 변경을 preview와 history에 반영
    // 입력: params: panel에서 변경된 develop parameter 값
    // 출력: 없음
    void applyDevelopParams(const core::types::DevelopParams& params);

    // 목적: 현재 사진의 연속 parameter 조작을 하나의 undo 단계로 시작
    // 입력: 없음
    // 출력: 없음
    void beginDevelopAdjustment();

    // 목적: 현재 사진의 연속 parameter 조작을 완료하고 undo action 상태 갱신
    // 입력: 없음
    // 출력: 없음
    void finishDevelopAdjustment();

    // 목적: 현재 사진의 마지막 develop parameter 변경을 undo
    // 입력: 없음
    // 출력: 없음
    void undoDevelopAdjustment();

    // 목적: 현재 사진의 마지막 undo된 develop parameter 변경을 redo
    // 입력: 없음
    // 출력: 없음
    void redoDevelopAdjustment();

    // 목적: Editor state 변경을 action 활성화와 source-blocked 안내에 반영
    // 입력: state: Qt-free Editor event snapshot을 Qt presentation model로 변환한 immutable state
    // 출력: 없음
    void updateEditorStateUi(const core::orchestration::EditorState& state);

    // 목적: 현재 사진 state에 맞춰 Save, undo와 redo action 활성화 갱신
    // 입력: 없음
    // 출력: 없음
    void updateEditorActions();

    catalog::CatalogListWidget* m_catalogWidget{nullptr};
    editor::DevelopPanel* m_developPanel{nullptr};
    catalog::SourceResolutionWidget* m_sourceResolutionWidget{nullptr};
    editor::PreviewWidget* m_previewWidget{nullptr};
    cli::ConsoleModeWidget* m_consoleWidget{nullptr};
    core::client::ICatalogPhotoClient* m_catalogPhotoClient{nullptr};
    core::client::ICatalogProjectClient* m_catalogProjectClient{nullptr};
    core::client::ICatalogFolderClient* m_catalogFolderClient{nullptr};
    core::client::ICatalogSessionClient* m_catalogSessionClient{nullptr};
    core::client::IEditorClient* m_editorClient{nullptr};
    core::client::IEditorStateEventSource* m_editorStateEventSource{nullptr};
    core::client::IFolderImportClient* m_folderImportClient{nullptr};
    core::client::IFolderImportEventSource* m_folderImportEventSource{nullptr};
    core::client::ICatalogThumbnailClient* m_catalogThumbnailClient{nullptr};
    core::client::ICatalogThumbnailEventSource* m_catalogThumbnailEventSource{nullptr};
    core::client::IPreviewPresentationClient* m_previewPresentationClient{nullptr};
    core::client::IPreviewPresentationEventSource* m_previewPresentationEventSource{nullptr};
    core::client::ISourceResolutionClient* m_sourceResolutionClient{nullptr};
    core::client::ISourceResolutionEventSource* m_sourceResolutionEventSource{nullptr};
    QtActivityAdapter* m_activityAdapter{nullptr};
    core::client::EditorStateSubscriptionHandle m_editorStateSubscription;
    core::client::ActivitySubscriptionHandle m_activitySubscription;
    core::client::FolderOperationSubscriptionHandle m_folderOperationSubscription;
    core::client::CatalogThumbnailSubscriptionHandle m_catalogThumbnailSubscription;
    core::client::PreviewPresentationSubscriptionHandle m_previewPresentationSubscription;
    core::client::SourceResolutionSubscriptionHandle m_sourceResolutionSubscription;
    core::client::IExportClient* m_exportClient{nullptr};
    core::client::IExportEventSource* m_exportEventSource{nullptr};
    core::client::IExportDefaultsClient* m_exportDefaultsClient{nullptr};
    QSettings* m_applicationSettings{nullptr};
    core::client::IWorkerProfileClient* m_workerProfileClient{nullptr};
    core::client::IWorkerHealthClient* m_workerHealthClient{nullptr};
    core::client::IWorkerHealthEventSource* m_workerHealthEventSource{nullptr};
    core::client::ICatalogStartupSettingsClient* m_catalogStartupSettingsClient{nullptr};
    QStackedWidget* m_contentStack{nullptr};
    QLabel* m_activityLabel{nullptr};
    QProgressBar* m_activityProgressBar{nullptr};
    QToolButton* m_cancelActivityButton{nullptr};
    std::optional<core::client::ActivityId> m_cancelActivityId;
    QAction* m_newCatalogAction{nullptr};
    QAction* m_openCatalogAction{nullptr};
    QAction* m_importFolderAction{nullptr};
    QAction* m_openFolderAction{nullptr};
    QAction* m_saveDevelopAction{nullptr};
    QAction* m_exportAction{nullptr};
    QAction* m_consoleModeAction{nullptr};
    QAction* m_undoDevelopAction{nullptr};
    QAction* m_redoDevelopAction{nullptr};
    QAction* m_settingsAction{nullptr};
    QAction* m_createProjectAction{nullptr};
    QAction* m_renameProjectAction{nullptr};
    QAction* m_removeProjectAction{nullptr};
    QAction* m_addSelectedPhotosToProjectAction{nullptr};
    QAction* m_removeSelectedPhotosFromProjectAction{nullptr};
    QToolButton* m_previousCatalogPageButton{nullptr};
    QToolButton* m_nextCatalogPageButton{nullptr};
    QComboBox* m_catalogFolderScopeComboBox{nullptr};
    QComboBox* m_catalogProjectScopeComboBox{nullptr};
    QToolButton* m_catalogProjectMenuButton{nullptr};
    std::optional<core::client::CatalogPhotoPage> m_catalogPhotoPage;
    std::optional<QString> m_catalogFolderPath;
    std::optional<core::catalog::ProjectId> m_catalogProjectId;
    QHash<qint64, core::client::SourceRequestId> m_sourceResolutionRequests;
    bool m_folderOperationActive{false};
};

}  // namespace flexraw::ui::mainwindow
