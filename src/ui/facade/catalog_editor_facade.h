#pragma once

#include <optional>

#include <QObject>
#include <QSize>

#include "catalog_contracts.h"
#include "catalog_photo_client.h"
#include "catalog_project_client.h"
#include "catalog_thumbnail_contracts.h"
#include "display_frame.h"
#include "editor_client.h"
#include "editor_contracts.h"
#include "editor_event_client.h"
#include "preview_contracts.h"

namespace flexraw::core::orchestration
{
class CatalogOrchestrator;
class CatalogThumbnailOrchestrator;
class EditorOrchestrator;
}  // namespace flexraw::core::orchestration

namespace flexraw::ui::facade
{

class QtEditorEventAdapter;

class CatalogEditorFacade final : public QObject,
                                  public core::client::ICatalogProjectClient,
                                  public core::client::ICatalogPhotoClient,
                                  public core::client::IEditorClient,
                                  public core::client::IEditorStateEventSource
{
    Q_OBJECT

public:
    // 목적: GUI adapter가 사용할 Catalog/Editor command와 event 경계 구성
    // 입력: catalogOrchestrator: catalog use case, catalogThumbnailOrchestrator: list thumbnail use case,
    //       editorOrchestrator: editor session use case, parent: Qt 부모 object
    // 출력: 세 Orchestrator를 감싼 facade 객체
    CatalogEditorFacade(core::orchestration::CatalogOrchestrator& catalogOrchestrator,
                        core::orchestration::CatalogThumbnailOrchestrator& catalogThumbnailOrchestrator,
                        core::orchestration::EditorOrchestrator& editorOrchestrator,
                        QObject* parent = nullptr);

    // 목적: 현재 catalog session state 반환
    // 입력: 없음
    // 출력: catalog open 여부와 canonical path
    [[nodiscard]] core::orchestration::CatalogSessionState catalogState() const;

    // 목적: 현재 editor session state 반환
    // 입력: 없음
    // 출력: selection, params, dirty와 history 상태
    [[nodiscard]] core::orchestration::EditorState editorState() const;

    // 목적: 현재 Editor session의 immutable Qt-free state 조회
    // 입력: 없음
    // 출력: selection, Develop state, source capability와 history snapshot
    [[nodiscard]] core::client::EditorSnapshot editorSnapshot() const override;

    // 목적: Qt GUI delivery context에서 initial snapshot과 이후 Editor state event 구독
    // 입력: callback: immutable Qt-free event consumer
    // 출력: RAII unsubscribe handle 또는 callback·thread 오류
    [[nodiscard]] core::client::EditorStateSubscriptionResult subscribeToEditorState(
        core::client::EditorStateCallback callback) override;

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

    // 목적: 지정 catalog를 열어 facade의 active catalog session으로 설정
    // 입력: catalogPath: 생성하거나 열 catalog file 경로
    // 출력: 열린 session state 또는 오류
    [[nodiscard]] core::orchestration::CatalogSessionResult openCatalog(const QString& catalogPath);

    // 목적: active catalog session과 background source 작업 정리
    // 입력: 없음
    // 출력: 닫힌 session state
    [[nodiscard]] core::orchestration::CatalogSessionState closeCatalog();

    // 목적: active catalog에서 optional exact-folder scope와 stable cursor 기반 bounded photo page 반환
    // 입력: request: page 크기, 방향, optional exclusive cursor와 folder scope
    // 출력: catalog photo page 또는 오류
    [[nodiscard]] core::orchestration::CatalogPhotoPageResult queryPhotos(
        const core::catalog::CatalogPhotoPageRequest& request) const;

    // 목적: optional Catalog/Folder/Project scope의 bounded page를 Qt-free snapshot으로 조회
    // 입력: request: fixed-width page 크기, 방향, optional cursor와 UTF-8 scope
    // 출력: stable order photo snapshot과 양방향 cursor 또는 구조화된 client 오류
    [[nodiscard]] core::client::CatalogPhotoPageResult queryPhotoPage(
        const core::client::CatalogPhotoPageRequest& request) const override;

    // 목적: active Catalog navigation에 사용할 distinct Folder summary 반환
    // 입력: 없음
    // 출력: path 순서의 Folder와 photo 수 또는 오류
    [[nodiscard]] core::orchestration::CatalogFolderListResult queryFolders() const;

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

    // 목적: Catalog list viewport와 인접 범위의 bounded thumbnail source set 교체
    // 입력: sources: 현재 materialize할 file descriptor, targetSize: icon 최대 크기
    // 출력: 수락 성공 또는 validation·shutdown 오류
    [[nodiscard]] core::orchestration::CatalogThumbnailWindowResult updateThumbnailWindow(
        QVector<core::types::FileDescriptor> sources, const QSize& targetSize);

    // 목적: worker에서 scan한 photo를 active catalog에 등록
    // 입력: entries: 분류가 완료된 supported photo 목록
    // 출력: 저장 수와 PhotoId 목록 또는 오류
    [[nodiscard]] core::orchestration::CatalogImportResult importScannedEntries(
        const QVector<core::catalog::CatalogEntry>& entries);

    // 목적: Folder photo를 active Catalog에 등록·resolve하고 stable Editor session 시작
    // 입력: entry: scan된 supported photo, targetSize: preview viewport 크기
    // 출력: 선택 후 editor state 또는 registration·catalog/source 오류
    [[nodiscard]] core::orchestration::EditorStateResult activatePhoto(const core::catalog::CatalogEntry& entry,
                                                                       const QSize& targetSize);

    // 목적: stable PhotoId photo를 persisted develop state와 함께 선택
    // 입력: photoId: catalog-local identity, targetSize: preview viewport 크기
    // 출력: 선택 후 editor state 또는 오류
    [[nodiscard]] core::orchestration::EditorStateResult selectCatalogPhoto(core::types::PhotoId photoId,
                                                                            const QSize& targetSize);

    // 목적: 현재 catalog-backed photo의 develop state 저장
    // 입력: 없음
    // 출력: 저장 후 editor state 또는 오류
    [[nodiscard]] core::orchestration::EditorStateResult saveCurrentPhoto();

    // 목적: current source를 기존 PhotoId의 새 baseline으로 수용
    // 입력: photoId: develop state를 유지할 catalog identity
    // 출력: accepted background request ID 또는 state 오류
    [[nodiscard]] core::orchestration::CatalogSourceSubmissionResult acceptReplacement(core::types::PhotoId photoId);

    // 목적: current replacement source를 새 PhotoId로 등록
    // 입력: photoId: source binding을 해제할 기존 identity
    // 출력: accepted background request ID 또는 state 오류
    [[nodiscard]] core::orchestration::CatalogSourceSubmissionResult registerReplacementAsNew(
        core::types::PhotoId photoId);

    // 목적: 기존 PhotoId를 검증된 다른 source locator에 재연결
    // 입력: photoId: 유지할 identity, locator: 검증할 새 위치
    // 출력: accepted background request ID 또는 state 오류
    [[nodiscard]] core::orchestration::CatalogSourceSubmissionResult relinkSource(
        core::types::PhotoId photoId, const core::types::SourceLocator& locator);

    // 목적: Activity adapter가 current Editor preview cancellation을 owner에 전달
    // 입력: requestId: accepted preview request identity
    // 출력: current request를 취소했으면 true
    bool cancelPreviewActivity(core::types::RequestId requestId);

    // 목적: Activity adapter가 source verification cancellation을 owner에 전달
    // 입력: requestId: accepted source request identity
    // 출력: active source request를 취소했으면 true
    bool cancelSourceActivity(core::types::RequestId requestId);

    // 목적: 현재 editor selection과 active preview 정리
    // 입력: 없음
    // 출력: 없음
    void clearSelection();

    // 목적: 현재 editor params를 갱신하고 preview 예약
    // 입력: params: 새 develop 값, targetSize: preview viewport 크기
    // 출력: 실제 state가 변경되면 true
    [[nodiscard]] bool updateDevelopParams(const core::types::DevelopParams& params, const QSize& targetSize);

    // 목적: GUI viewport resize를 Editor preview target 변경으로 전달
    // 입력: targetSize: layout 적용 후 preview viewport 크기
    // 출력: 실제 target 크기 변경이 예약됐으면 true
    [[nodiscard]] bool updatePreviewTargetSize(const QSize& targetSize);

    // 목적: 연속 develop 조작을 하나의 undo 단계로 시작
    // 입력: 없음
    // 출력: 없음
    void beginEdit();

    // 목적: 현재 연속 develop 조작 종료
    // 입력: 없음
    // 출력: 없음
    void endEdit();

    // 목적: 현재 photo의 마지막 develop 변경 되돌리기
    // 입력: targetSize: preview viewport 크기
    // 출력: 변경된 state 또는 undo 불가 시 빈 값
    [[nodiscard]] std::optional<core::orchestration::EditorState> undo(const QSize& targetSize);

    // 목적: 현재 photo의 마지막 undo 변경 다시 적용
    // 입력: targetSize: preview viewport 크기
    // 출력: 변경된 state 또는 redo 불가 시 빈 값
    [[nodiscard]] std::optional<core::orchestration::EditorState> redo(const QSize& targetSize);

signals:
    // 목적: Qt-free local presentation contract로 변환한 current preview frame 전달
    // 입력: frame: immutable BGRA8/top-down/sRGB display frame
    // 출력: 없음
    void displayFrameUpdated(const core::client::DisplayFrame& frame);

    // 목적: current Catalog thumbnail window의 decoded frame 전달
    // 입력: frame: source path와 thumbnail image
    // 출력: 없음
    void catalogThumbnailReady(const core::orchestration::CatalogThumbnailFrame& frame);

    // 목적: current Catalog thumbnail window의 terminal decode 실패 전달
    // 입력: issue: source path와 구조화된 오류
    // 출력: 없음
    void catalogThumbnailFailed(const core::orchestration::CatalogThumbnailIssue& issue);

    // 목적: facade consumer에 editor state 변경 전달
    // 입력: state: 변경 후 immutable editor state
    // 출력: 없음
    void editorStateChanged(const core::orchestration::EditorState& state);

    // 목적: facade consumer에 preview request accepted lifecycle 전달
    // 입력: requestId: accepted preview request identity
    // 출력: 없음
    void previewStarted(core::types::RequestId requestId);

    // 목적: facade consumer에 current preview frame 전달
    // 입력: result: stale filtering이 끝난 preview 결과
    // 출력: 없음
    void previewUpdated(const core::orchestration::PreviewResult& result);

    // 목적: facade consumer에 current preview warning 전달
    // 입력: issue: 현재 request의 비치명적 문제
    // 출력: 없음
    void previewWarning(const core::orchestration::PreviewIssue& issue);

    // 목적: facade consumer에 preview 완료 전달
    // 입력: requestId: 완료된 request identity
    // 출력: 없음
    void previewCompleted(core::types::RequestId requestId);

    // 목적: facade consumer에 preview 실패 전달
    // 입력: issue: 현재 request의 구조화된 실패
    // 출력: 없음
    void previewFailed(const core::orchestration::PreviewIssue& issue);

    // 목적: facade consumer에 preview 취소 전달
    // 입력: requestId: 취소된 request identity
    // 출력: 없음
    void previewCancelled(core::types::RequestId requestId);

    // 목적: facade consumer에 source verification accepted lifecycle 전달
    // 입력: requestId: accepted source request identity, photoId: 대상 Photo identity
    // 출력: 없음
    void sourceBindingStarted(core::types::RequestId requestId, core::types::PhotoId photoId);

    // 목적: source binding transition 결과를 GUI consumer에 전달
    // 입력: update: 갱신된 기존 photo와 optional 신규 photo
    // 출력: 없음
    void sourceBindingUpdated(const core::orchestration::CatalogSourceUpdate& update);

    // 목적: source verification 또는 resolution 실패를 GUI consumer에 전달
    // 입력: issue: request·photo identity와 technical 오류
    // 출력: 없음
    void sourceBindingFailed(const core::orchestration::CatalogIssue& issue);

    // 목적: source request terminal cancellation을 GUI consumer에 전달
    // 입력: requestId: 취소된 request identity
    // 출력: 없음
    void sourceBindingCancelled(core::types::RequestId requestId);

private:
    core::orchestration::CatalogOrchestrator* m_catalogOrchestrator{nullptr};
    core::orchestration::CatalogThumbnailOrchestrator* m_catalogThumbnailOrchestrator{nullptr};
    core::orchestration::EditorOrchestrator* m_editorOrchestrator{nullptr};
    QtEditorEventAdapter* m_editorEventAdapter{nullptr};
};

}  // namespace flexraw::ui::facade
