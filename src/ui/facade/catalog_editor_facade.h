#pragma once

#include <QObject>

#include "catalog_folder_client.h"
#include "catalog_photo_client.h"
#include "catalog_project_client.h"
#include "catalog_session_client.h"
#include "catalog_thumbnail_client.h"
#include "editor_client.h"
#include "editor_event_client.h"
#include "folder_import_client.h"
#include "preview_presentation_client.h"
#include "source_resolution_client.h"

namespace flexraw::core::orchestration
{
class CatalogOrchestrator;
class CatalogSessionOrchestrator;
class CatalogThumbnailOrchestrator;
class EditorOrchestrator;
}  // namespace flexraw::core::orchestration

namespace flexraw::ui::facade
{

class QtEditorEventAdapter;
class QtCatalogThumbnailEventAdapter;
class QtFolderImportEventAdapter;
class QtPreviewPresentationEventAdapter;

class CatalogEditorFacade final : public QObject,
                                  public core::client::ICatalogFolderClient,
                                  public core::client::ICatalogProjectClient,
                                  public core::client::ICatalogPhotoClient,
                                  public core::client::ICatalogSessionClient,
                                  public core::client::IEditorClient,
                                  public core::client::IEditorStateEventSource,
                                  public core::client::IFolderImportClient,
                                  public core::client::IFolderImportEventSource,
                                  public core::client::ICatalogThumbnailClient,
                                  public core::client::ICatalogThumbnailEventSource,
                                  public core::client::IPreviewPresentationClient,
                                  public core::client::IPreviewPresentationEventSource,
                                  public core::client::ISourceResolutionClient,
                                  public core::client::ISourceResolutionEventSource
{
    Q_OBJECT

public:
    // 목적: GUI adapter가 사용할 Catalog/Editor command와 event 경계 구성
    // 입력: catalogOrchestrator: catalog data use case, catalogSessionOrchestrator: session transition use case,
    //       catalogThumbnailOrchestrator: list thumbnail use case, editorOrchestrator: editor session use case,
    //       sourceResolutionEventSource: Runtime-owned source lifecycle, parent: Qt 부모 object
    // 출력: 주입된 use case와 Source Resolution event를 감싼 facade 객체
    CatalogEditorFacade(core::orchestration::CatalogOrchestrator& catalogOrchestrator,
                        core::orchestration::CatalogSessionOrchestrator& catalogSessionOrchestrator,
                        core::orchestration::CatalogThumbnailOrchestrator& catalogThumbnailOrchestrator,
                        core::orchestration::EditorOrchestrator& editorOrchestrator,
                        core::client::ISourceResolutionEventSource& sourceResolutionEventSource,
                        QObject* parent = nullptr);

    // 목적: 현재 active Catalog session을 immutable Qt-free snapshot으로 조회
    // 입력: 없음
    // 출력: open 여부와 open 상태에서만 채워지는 normalized absolute UTF-8 catalog path
    [[nodiscard]] core::client::CatalogSessionSnapshot catalogSnapshot() const override;

    // 목적: 현재 Editor session의 immutable Qt-free state 조회
    // 입력: 없음
    // 출력: selection, Develop state, source capability와 history snapshot
    [[nodiscard]] core::client::EditorSnapshot editorSnapshot() const override;

    // 목적: Folder scan source를 active Catalog에 등록·resolve하고 Editor session으로 선택
    // 입력: command: normalized absolute UTF-8 source와 표시 metadata
    // 출력: stable Photo identity가 발급된 snapshot 또는 validation·Catalog 오류
    [[nodiscard]] core::client::EditorResult activateSource(
        const core::client::ActivateEditorSourceCommand& command) override;

    // 목적: Qt GUI delivery context에서 initial snapshot과 이후 Editor state event 구독
    // 입력: callback: immutable Qt-free event consumer
    // 출력: RAII unsubscribe handle 또는 callback·thread 오류
    [[nodiscard]] core::client::EditorStateSubscriptionResult subscribeToEditorState(
        core::client::EditorStateCallback callback) override;

    // 목적: Qt-free viewport command를 authoritative Editor Preview owner에 전달
    // 입력: command: 양수 fixed-width pixel 크기
    // 출력: 적용된 viewport 또는 validation 오류
    [[nodiscard]] core::client::PreviewViewportResult setPreviewViewport(
        const core::client::SetPreviewViewportCommand& command) override;

    // 목적: 현재 accepted Preview request의 향후 frame publication과 processing 취소
    // 입력: requestId: owner가 발급해 presentation snapshot에 공개한 identity
    // 출력: 취소된 identity 또는 stale·validation 오류
    [[nodiscard]] core::client::PreviewRequestCancelResult cancelPreviewRequest(
        core::client::PreviewRequestId requestId) override;

    // 목적: Qt GUI delivery context에서 initial Preview snapshot과 이후 lifecycle 구독
    // 입력: callback: immutable frame·analysis·warning·terminal consumer
    // 출력: RAII unsubscribe handle 또는 callback·thread 오류
    [[nodiscard]] core::client::PreviewPresentationSubscriptionResult subscribeToPreviewPresentation(
        core::client::PreviewPresentationCallback callback) override;

    // 목적: Qt-free command로 replacement source를 기존 Photo identity의 새 baseline으로 수용
    // 입력: command: develop state를 유지할 stable Photo identity
    // 출력: accepted request context 또는 capability·session 오류
    [[nodiscard]] core::client::SourceRequestResult acceptReplacement(
        const core::client::AcceptReplacementCommand& command) override;

    // 목적: Qt-free command로 replacement source에 새 Photo identity 발급
    // 입력: command: source binding을 해제할 기존 stable Photo identity
    // 출력: accepted request context 또는 capability·session 오류
    [[nodiscard]] core::client::SourceRequestResult registerReplacementAsNew(
        const core::client::RegisterReplacementAsNewCommand& command) override;

    // 목적: Qt-free command로 기존 Photo를 normalized absolute source locator에 재연결
    // 입력: command: stable Photo identity와 UTF-8 lexical locator
    // 출력: accepted request context 또는 validation·capability·session 오류
    [[nodiscard]] core::client::SourceRequestResult relinkSource(
        const core::client::RelinkSourceCommand& command) override;

    // 목적: accepted source fingerprint request의 향후 mutation과 event publication 취소
    // 입력: requestId: owner가 발급한 source request identity
    // 출력: 취소된 identity 또는 stale·validation 오류
    [[nodiscard]] core::client::SourceRequestCancelResult cancelSourceRequest(
        core::client::SourceRequestId requestId) override;

    // 목적: Qt GUI delivery context에서 initial source requests와 이후 lifecycle 구독
    // 입력: callback: immutable Source Resolution event consumer
    // 출력: RAII unsubscribe handle 또는 callback·thread 오류
    [[nodiscard]] core::client::SourceResolutionSubscriptionResult subscribeToSourceResolution(
        core::client::SourceResolutionCallback callback) override;

    // 목적: Qt-free tagged item 집합으로 visible/adjacent Catalog thumbnail window 교체
    // 입력: command: stable PhotoId 또는 transient locator identity와 target extent
    // 출력: owner generation과 accepted item 수 또는 validation·shutdown 오류
    [[nodiscard]] core::client::CatalogThumbnailWindowResult replaceThumbnailWindow(
        const core::client::ReplaceCatalogThumbnailWindowCommand& command) override;

    // 목적: active Catalog thumbnail window와 pending decode를 idempotent하게 정리
    // 입력: 없음
    // 출력: 기존 generation cancellation 완료 또는 shutdown 오류
    [[nodiscard]] core::client::CatalogThumbnailClearResult clearThumbnailWindow() override;

    // 목적: Qt GUI delivery context에서 initial Catalog thumbnail snapshot과 이후 lifecycle 구독
    // 입력: callback: immutable frame·issue·terminal consumer
    // 출력: RAII unsubscribe handle 또는 callback·thread 오류
    [[nodiscard]] core::client::CatalogThumbnailSubscriptionResult subscribeToCatalogThumbnails(
        core::client::CatalogThumbnailCallback callback) override;

    // 목적: Qt-free command로 Catalog와 독립적인 단일 folder scan 제출
    // 입력: command: UTF-8 folder path
    // 출력: accepted operation receipt 또는 validation·busy 오류
    [[nodiscard]] core::client::FolderOperationResult submitFolderScan(
        const core::client::ScanFolderCommand& command) override;

    // 목적: Qt-free command로 expected Catalog에 묶인 단일 folder import 제출
    // 입력: command: UTF-8 folder path와 submit 시점 Catalog path
    // 출력: accepted operation receipt 또는 validation·session·busy 오류
    [[nodiscard]] core::client::FolderOperationResult submitFolderImport(
        const core::client::ImportFolderCommand& command) override;

    // 목적: Qt GUI delivery context에서 initial Folder operation과 이후 lifecycle event 구독
    // 입력: callback: immutable Qt-free event consumer
    // 출력: RAII unsubscribe handle 또는 callback·thread 오류
    [[nodiscard]] core::client::FolderOperationSubscriptionResult subscribeToFolderOperations(
        core::client::FolderOperationCallback callback) override;

    // 목적: active Catalog의 stable PhotoId를 현재 Editor session으로 선택
    // 입력: command: 선택할 fixed-width Photo identity
    // 출력: 선택 후 snapshot 또는 validation·Catalog·Develop load 오류
    [[nodiscard]] core::client::EditorResult selectPhoto(
        const core::client::SelectEditorPhotoCommand& command) override;

    // 목적: 현재 Editor selection과 진행 중 Adjustment를 정리
    // 입력: 없음
    // 출력: 선택되지 않은 snapshot
    [[nodiscard]] core::client::EditorResult clearEditorSelection() override;

    // 목적: 현재 Photo의 Develop parameter를 검증하고 session state에 반영
    // 입력: command: Qt-free Develop parameter 전체 값
    // 출력: 변경 후 snapshot 또는 selection·source·validation 오류
    [[nodiscard]] core::client::EditorResult updateDevelopParams(
        const core::client::UpdateDevelopParamsCommand& command) override;

    // 목적: 연속 Develop parameter 조작을 하나의 undo 단위로 시작
    // 입력: 없음
    // 출력: Adjustment가 활성화된 snapshot 또는 현재 state 오류
    [[nodiscard]] core::client::EditorResult beginAdjustment() override;

    // 목적: 현재 연속 Adjustment를 종료하고 undo 단위를 확정
    // 입력: 없음
    // 출력: Adjustment가 종료된 snapshot 또는 현재 state 오류
    [[nodiscard]] core::client::EditorResult endAdjustment() override;

    // 목적: 현재 Photo의 마지막 Develop Adjustment를 되돌림
    // 입력: 없음
    // 출력: 되돌린 snapshot 또는 selection·history 상태 오류
    [[nodiscard]] core::client::EditorResult undoDevelop() override;

    // 목적: 현재 Photo에서 마지막으로 되돌린 Develop Adjustment를 다시 적용
    // 입력: 없음
    // 출력: 다시 적용한 snapshot 또는 selection·history 상태 오류
    [[nodiscard]] core::client::EditorResult redoDevelop() override;

    // 목적: dirty Develop state를 optimistic persisted revision으로 저장
    // 입력: 없음
    // 출력: 저장된 baseline과 revision snapshot 또는 conflict·database 오류
    [[nodiscard]] core::client::EditorResult saveDevelopState() override;

    // 목적: 명시적 create/open 및 optional clean-session 교체 정책으로 Catalog 활성화
    // 입력: command: UTF-8 path, create/open mode와 active session replacement 정책
    // 출력: 열린 normalized absolute snapshot 또는 validation·permission·database·conflict 오류
    [[nodiscard]] core::client::CatalogSessionResult openCatalog(
        const core::client::OpenCatalogCommand& command) override;

    // 목적: dirty Editor state를 보존하면서 clean Catalog session을 idempotent하게 종료
    // 입력: 없음
    // 출력: 닫힌 snapshot 또는 unsaved Editor conflict 오류
    [[nodiscard]] core::client::CatalogSessionResult closeCatalog() override;

    // 목적: optional Catalog/Folder/Project scope의 bounded page를 Qt-free snapshot으로 조회
    // 입력: request: fixed-width page 크기, 방향, optional cursor와 UTF-8 scope
    // 출력: stable order photo snapshot과 양방향 cursor 또는 구조화된 client 오류
    [[nodiscard]] core::client::CatalogPhotoPageResult queryPhotoPage(
        const core::client::CatalogPhotoPageRequest& request) const override;

    // 목적: active Catalog navigation에 사용할 distinct Folder snapshot 조회
    // 입력: 없음
    // 출력: normalized UTF-8 path 순서와 양수 photo 수 또는 구조화된 client 오류
    [[nodiscard]] core::client::CatalogFolderListResult listFolders() const override;

    // 목적: active Catalog Project를 Qt-free client snapshot으로 조회
    // 입력: 없음
    // 출력: UTF-8 이름과 fixed-width identity 목록 또는 구조화된 client 오류
    [[nodiscard]] core::client::CatalogProjectListResult listProjects() const override;

    // 목적: Qt-free Project command를 active Catalog use case로 전달
    // 입력: command: UTF-8 Project 표시 이름
    // 출력: 생성된 Project snapshot 또는 구조화된 client 오류
    [[nodiscard]] core::client::CatalogProjectResult createProject(
        const core::client::CreateProjectCommand& command) override;

    // 목적: Qt-free command로 active Catalog Project 이름 변경
    // 입력: command: fixed-width identity와 UTF-8 새 표시 이름
    // 출력: 변경된 Project snapshot 또는 구조화된 client 오류
    [[nodiscard]] core::client::CatalogProjectResult renameProject(
        const core::client::RenameProjectCommand& command) override;

    // 목적: Qt-free command로 active Catalog Project와 membership 삭제
    // 입력: command: 삭제할 Project identity
    // 출력: Photo 보존을 전제로 한 mutation receipt 또는 구조화된 오류
    [[nodiscard]] core::client::CatalogProjectDeleteResult deleteProject(
        const core::client::DeleteProjectCommand& command) override;

    // 목적: Qt-free command로 선택 Photo를 Project membership에 추가
    // 입력: command: Project와 Photo identity 한 쌍
    // 출력: 처리한 identity receipt 또는 구조화된 오류
    [[nodiscard]] core::client::CatalogProjectMembershipResult addPhotoToProject(
        const core::client::ProjectPhotoMembershipCommand& command) override;

    // 목적: Qt-free command로 선택 Photo를 Project membership에서 제거
    // 입력: command: Project와 Photo identity 한 쌍
    // 출력: 처리한 identity receipt 또는 구조화된 오류
    [[nodiscard]] core::client::CatalogProjectMembershipResult removePhotoFromProject(
        const core::client::ProjectPhotoMembershipCommand& command) override;

private:
    core::orchestration::CatalogOrchestrator* m_catalogOrchestrator{nullptr};
    core::orchestration::CatalogSessionOrchestrator* m_catalogSessionOrchestrator{nullptr};
    core::orchestration::CatalogThumbnailOrchestrator* m_catalogThumbnailOrchestrator{nullptr};
    core::orchestration::EditorOrchestrator* m_editorOrchestrator{nullptr};
    QtCatalogThumbnailEventAdapter* m_catalogThumbnailEventAdapter{nullptr};
    QtEditorEventAdapter* m_editorEventAdapter{nullptr};
    QtFolderImportEventAdapter* m_folderImportEventAdapter{nullptr};
    QtPreviewPresentationEventAdapter* m_previewPresentationEventAdapter{nullptr};
    core::client::ISourceResolutionEventSource* m_sourceResolutionEventSource{nullptr};
};

}  // namespace flexraw::ui::facade
