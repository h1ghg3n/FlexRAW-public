#include "catalog_editor_facade.h"

#include <utility>

#include "catalog_orchestrator.h"
#include "catalog_thumbnail_orchestrator.h"
#include "display_frame_qt_adapter.h"
#include "editor_client_projection.h"
#include "editor_orchestrator.h"
#include "qt_editor_event_adapter.h"

namespace flexraw::ui::facade
{
namespace
{

// 목적: Qt-free Editor result를 기존 Qt GUI adapter result로 변환
// 입력: result: client contract command 결과
// 출력: 같은 state 또는 오류 의미를 가진 transitional result
[[nodiscard]] core::orchestration::EditorStateResult toQtEditorResult(const core::client::EditorResult& result)
{
    return result.hasError()
               ? core::orchestration::EditorStateResult::failure(core::orchestration::fromClientError(result.error()))
               : core::orchestration::EditorStateResult::success(
                     core::orchestration::fromClientEditorSnapshot(result.value()));
}

}  // namespace

// 목적: GUI adapter가 사용할 Catalog/Editor command와 event 경계 구성
// 입력: catalogOrchestrator: catalog use case, catalogThumbnailOrchestrator: list thumbnail use case,
//       editorOrchestrator: editor session use case, parent: Qt 부모 object
// 출력: 세 Orchestrator를 감싼 facade 객체
CatalogEditorFacade::CatalogEditorFacade(
    core::orchestration::CatalogOrchestrator& catalogOrchestrator,
    core::orchestration::CatalogThumbnailOrchestrator& catalogThumbnailOrchestrator,
    core::orchestration::EditorOrchestrator& editorOrchestrator,
    QObject* parent)
    : QObject(parent),
      m_catalogOrchestrator(&catalogOrchestrator),
      m_catalogThumbnailOrchestrator(&catalogThumbnailOrchestrator),
      m_editorOrchestrator(&editorOrchestrator),
      m_editorEventAdapter(new QtEditorEventAdapter(editorOrchestrator, this))
{
    qRegisterMetaType<core::client::DisplayFrame>();
    connect(m_catalogThumbnailOrchestrator,
            &core::orchestration::CatalogThumbnailOrchestrator::thumbnailReady,
            this,
            &CatalogEditorFacade::catalogThumbnailReady);
    connect(m_catalogThumbnailOrchestrator,
            &core::orchestration::CatalogThumbnailOrchestrator::thumbnailFailed,
            this,
            &CatalogEditorFacade::catalogThumbnailFailed);
    connect(m_editorOrchestrator,
            &core::orchestration::EditorOrchestrator::stateChanged,
            this,
            &CatalogEditorFacade::editorStateChanged);
    connect(m_editorOrchestrator,
            &core::orchestration::EditorOrchestrator::previewStarted,
            this,
            &CatalogEditorFacade::previewStarted);
    connect(m_editorOrchestrator,
            &core::orchestration::EditorOrchestrator::previewUpdated,
            this,
            [this](const core::orchestration::PreviewResult& result) {
                const std::optional<core::client::DisplayFrame> frame =
                    toDisplayFrame(result.image, result.previewSequence);
                if (frame.has_value())
                {
                    emit displayFrameUpdated(*frame);
                }
                emit previewUpdated(result);
            });
    connect(m_editorOrchestrator,
            &core::orchestration::EditorOrchestrator::previewWarning,
            this,
            &CatalogEditorFacade::previewWarning);
    connect(m_editorOrchestrator,
            &core::orchestration::EditorOrchestrator::previewCompleted,
            this,
            &CatalogEditorFacade::previewCompleted);
    connect(m_editorOrchestrator,
            &core::orchestration::EditorOrchestrator::previewFailed,
            this,
            &CatalogEditorFacade::previewFailed);
    connect(m_editorOrchestrator,
            &core::orchestration::EditorOrchestrator::previewCancelled,
            this,
            &CatalogEditorFacade::previewCancelled);
    connect(m_catalogOrchestrator,
            &core::orchestration::CatalogOrchestrator::sourceBindingStarted,
            this,
            &CatalogEditorFacade::sourceBindingStarted);
    connect(m_catalogOrchestrator,
            &core::orchestration::CatalogOrchestrator::sourceBindingUpdated,
            this,
            &CatalogEditorFacade::sourceBindingUpdated);
    connect(m_catalogOrchestrator,
            &core::orchestration::CatalogOrchestrator::sourceBindingFailed,
            this,
            &CatalogEditorFacade::sourceBindingFailed);
    connect(m_catalogOrchestrator,
            &core::orchestration::CatalogOrchestrator::sourceBindingCancelled,
            this,
            &CatalogEditorFacade::sourceBindingCancelled);
}

// 목적: 현재 catalog session state 반환
// 입력: 없음
// 출력: catalog open 여부와 canonical path
core::orchestration::CatalogSessionState CatalogEditorFacade::catalogState() const
{
    return m_catalogOrchestrator->state();
}

// 목적: 현재 editor session state 반환
// 입력: 없음
// 출력: selection, params, dirty와 history 상태
core::orchestration::EditorState CatalogEditorFacade::editorState() const
{
    return core::orchestration::fromClientEditorSnapshot(editorSnapshot());
}

// 목적: 현재 Editor session의 immutable Qt-free state 조회
// 입력: 없음
// 출력: selection, Develop state, source capability와 history snapshot
core::client::EditorSnapshot CatalogEditorFacade::editorSnapshot() const
{
    return m_editorOrchestrator->editorSnapshot();
}

// 목적: Qt GUI delivery context에서 initial snapshot과 이후 Editor state event 구독
// 입력: callback: immutable Qt-free event consumer
// 출력: RAII unsubscribe handle 또는 callback·thread 오류
core::client::EditorStateSubscriptionResult CatalogEditorFacade::subscribeToEditorState(
    core::client::EditorStateCallback callback)
{
    return m_editorEventAdapter->subscribeToEditorState(std::move(callback));
}

// 목적: active Catalog의 stable PhotoId를 현재 Editor session으로 선택
// 입력: command: 선택할 fixed-width Photo identity
// 출력: 선택 후 snapshot 또는 validation·Catalog·Develop load 오류
core::client::EditorResult CatalogEditorFacade::selectPhoto(const core::client::SelectEditorPhotoCommand& command)
{
    return m_editorOrchestrator->selectPhoto(command);
}

// 목적: 현재 Editor selection과 진행 중 Adjustment를 정리
// 입력: 없음
// 출력: 선택되지 않은 snapshot
core::client::EditorResult CatalogEditorFacade::clearEditorSelection()
{
    return m_editorOrchestrator->clearEditorSelection();
}

// 목적: 현재 Photo의 Develop parameter를 검증하고 session state에 반영
// 입력: command: Qt-free Develop parameter 전체 값
// 출력: 변경 후 snapshot 또는 selection·source·validation 오류
core::client::EditorResult CatalogEditorFacade::updateDevelopParams(
    const core::client::UpdateDevelopParamsCommand& command)
{
    return m_editorOrchestrator->updateDevelopParams(command);
}

// 목적: 연속 Develop parameter 조작을 하나의 undo 단위로 시작
// 입력: 없음
// 출력: Adjustment가 활성화된 snapshot 또는 현재 state 오류
core::client::EditorResult CatalogEditorFacade::beginAdjustment()
{
    return m_editorOrchestrator->beginAdjustment();
}

// 목적: 현재 연속 Adjustment를 종료하고 undo 단위를 확정
// 입력: 없음
// 출력: Adjustment가 종료된 snapshot 또는 현재 state 오류
core::client::EditorResult CatalogEditorFacade::endAdjustment()
{
    return m_editorOrchestrator->endAdjustment();
}

// 목적: 현재 Photo의 마지막 Develop Adjustment를 되돌림
// 입력: 없음
// 출력: 되돌린 snapshot 또는 selection·history 상태 오류
core::client::EditorResult CatalogEditorFacade::undoDevelop()
{
    return m_editorOrchestrator->undoDevelop();
}

// 목적: 현재 Photo에서 마지막으로 되돌린 Develop Adjustment를 다시 적용
// 입력: 없음
// 출력: 다시 적용한 snapshot 또는 selection·history 상태 오류
core::client::EditorResult CatalogEditorFacade::redoDevelop()
{
    return m_editorOrchestrator->redoDevelop();
}

// 목적: dirty Develop state를 optimistic persisted revision으로 저장
// 입력: 없음
// 출력: 저장된 baseline과 revision snapshot 또는 conflict·database 오류
core::client::EditorResult CatalogEditorFacade::saveDevelopState()
{
    return m_editorOrchestrator->saveDevelopState();
}

// 목적: 지정 catalog를 열어 facade의 active catalog session으로 설정
// 입력: catalogPath: 생성하거나 열 catalog file 경로
// 출력: 열린 session state 또는 오류
core::orchestration::CatalogSessionResult CatalogEditorFacade::openCatalog(const QString& catalogPath)
{
    return m_catalogOrchestrator->openCatalog(catalogPath);
}

// 목적: active catalog session과 background source 작업 정리
// 입력: 없음
// 출력: 닫힌 session state
core::orchestration::CatalogSessionState CatalogEditorFacade::closeCatalog()
{
    return m_catalogOrchestrator->closeCatalog();
}

// 목적: active catalog에서 optional exact-folder scope와 stable cursor 기반 bounded photo page 반환
// 입력: request: page 크기, 방향, optional exclusive cursor와 folder scope
// 출력: catalog photo page 또는 오류
core::orchestration::CatalogPhotoPageResult CatalogEditorFacade::queryPhotos(
    const core::catalog::CatalogPhotoPageRequest& request) const
{
    return m_catalogOrchestrator->queryPhotos(request);
}

// 목적: optional Catalog/Folder/Project scope의 bounded page를 Qt-free snapshot으로 조회
// 입력: request: fixed-width page 크기, 방향, optional cursor와 UTF-8 scope
// 출력: stable order photo snapshot과 양방향 cursor 또는 구조화된 client 오류
core::client::CatalogPhotoPageResult CatalogEditorFacade::queryPhotoPage(
    const core::client::CatalogPhotoPageRequest& request) const
{
    return m_catalogOrchestrator->queryPhotoPage(request);
}

// 목적: active Catalog navigation에 사용할 distinct Folder summary 반환
// 입력: 없음
// 출력: path 순서의 Folder와 photo 수 또는 오류
core::orchestration::CatalogFolderListResult CatalogEditorFacade::queryFolders() const
{
    return m_catalogOrchestrator->queryFolders();
}

// 목적: active Catalog Project를 Qt-free client snapshot으로 조회
// 입력: 없음
// 출력: UTF-8 이름과 fixed-width identity 목록 또는 구조화된 client 오류
core::client::CatalogProjectListResult CatalogEditorFacade::listProjects() const
{
    return m_catalogOrchestrator->listProjects();
}

// 목적: Qt-free Project command를 active Catalog use case로 전달
// 입력: command: UTF-8 Project 표시 이름
// 출력: 생성된 Project snapshot 또는 구조화된 client 오류
core::client::CatalogProjectResult CatalogEditorFacade::createProject(const core::client::CreateProjectCommand& command)
{
    return m_catalogOrchestrator->createProject(command);
}

// 목적: Qt-free command로 active Catalog Project 이름 변경
// 입력: command: fixed-width identity와 UTF-8 새 표시 이름
// 출력: 변경된 Project snapshot 또는 구조화된 client 오류
core::client::CatalogProjectResult CatalogEditorFacade::renameProject(const core::client::RenameProjectCommand& command)
{
    return m_catalogOrchestrator->renameProject(command);
}

// 목적: Qt-free command로 active Catalog Project와 membership 삭제
// 입력: command: 삭제할 Project identity
// 출력: Photo 보존을 전제로 한 mutation receipt 또는 구조화된 오류
core::client::CatalogProjectDeleteResult CatalogEditorFacade::deleteProject(
    const core::client::DeleteProjectCommand& command)
{
    return m_catalogOrchestrator->deleteProject(command);
}

// 목적: Qt-free command로 선택 Photo를 Project membership에 추가
// 입력: command: Project와 Photo identity 한 쌍
// 출력: 처리한 identity receipt 또는 구조화된 오류
core::client::CatalogProjectMembershipResult CatalogEditorFacade::addPhotoToProject(
    const core::client::ProjectPhotoMembershipCommand& command)
{
    return m_catalogOrchestrator->addPhotoToProject(command);
}

// 목적: Qt-free command로 선택 Photo를 Project membership에서 제거
// 입력: command: Project와 Photo identity 한 쌍
// 출력: 처리한 identity receipt 또는 구조화된 오류
core::client::CatalogProjectMembershipResult CatalogEditorFacade::removePhotoFromProject(
    const core::client::ProjectPhotoMembershipCommand& command)
{
    return m_catalogOrchestrator->removePhotoFromProject(command);
}

// 목적: Catalog list viewport와 인접 범위의 bounded thumbnail source set 교체
// 입력: sources: 현재 materialize할 file descriptor, targetSize: icon 최대 크기
// 출력: 수락 성공 또는 validation·shutdown 오류
core::orchestration::CatalogThumbnailWindowResult CatalogEditorFacade::updateThumbnailWindow(
    QVector<core::types::FileDescriptor> sources, const QSize& targetSize)
{
    return m_catalogThumbnailOrchestrator->updateWindow({std::move(sources), targetSize});
}

// 목적: worker에서 scan한 photo를 active catalog에 등록
// 입력: entries: 분류가 완료된 supported photo 목록
// 출력: 저장 수와 PhotoId 목록 또는 오류
core::orchestration::CatalogImportResult CatalogEditorFacade::importScannedEntries(
    const QVector<core::catalog::CatalogEntry>& entries)
{
    return m_catalogOrchestrator->importScannedEntries(entries);
}

// 목적: Folder photo를 active Catalog에 등록·resolve하고 stable Editor session 시작
// 입력: entry: scan된 supported photo, targetSize: preview viewport 크기
// 출력: 선택 후 editor state 또는 registration·catalog/source 오류
core::orchestration::EditorStateResult CatalogEditorFacade::activatePhoto(const core::catalog::CatalogEntry& entry,
                                                                          const QSize& targetSize)
{
    (void)updatePreviewTargetSize(targetSize);
    const core::orchestration::CatalogPhotoRegistrationResult registered = m_catalogOrchestrator->registerPhoto(entry);
    if (registered.hasError())
    {
        return core::orchestration::EditorStateResult::failure(registered.error());
    }

    return toQtEditorResult(selectPhoto({core::client::ClientPhotoId{registered.value().value}}));
}

// 목적: stable PhotoId photo를 persisted develop state와 함께 선택
// 입력: photoId: catalog-local identity, targetSize: preview viewport 크기
// 출력: 선택 후 editor state 또는 오류
core::orchestration::EditorStateResult CatalogEditorFacade::selectCatalogPhoto(core::types::PhotoId photoId,
                                                                               const QSize& targetSize)
{
    (void)updatePreviewTargetSize(targetSize);
    return toQtEditorResult(selectPhoto({core::client::ClientPhotoId{photoId.value}}));
}

// 목적: 현재 catalog-backed photo의 develop state 저장
// 입력: 없음
// 출력: 저장 후 editor state 또는 오류
core::orchestration::EditorStateResult CatalogEditorFacade::saveCurrentPhoto()
{
    return toQtEditorResult(saveDevelopState());
}

// 목적: current source를 기존 PhotoId의 새 baseline으로 수용
// 입력: photoId: develop state를 유지할 catalog identity
// 출력: accepted background request ID 또는 state 오류
core::orchestration::CatalogSourceSubmissionResult CatalogEditorFacade::acceptReplacement(core::types::PhotoId photoId)
{
    return m_catalogOrchestrator->acceptReplacement(photoId);
}

// 목적: current replacement source를 새 PhotoId로 등록
// 입력: photoId: source binding을 해제할 기존 identity
// 출력: accepted background request ID 또는 state 오류
core::orchestration::CatalogSourceSubmissionResult CatalogEditorFacade::registerReplacementAsNew(
    core::types::PhotoId photoId)
{
    return m_catalogOrchestrator->registerReplacementAsNew(photoId);
}

// 목적: 기존 PhotoId를 검증된 다른 source locator에 재연결
// 입력: photoId: 유지할 identity, locator: 검증할 새 위치
// 출력: accepted background request ID 또는 state 오류
core::orchestration::CatalogSourceSubmissionResult CatalogEditorFacade::relinkSource(
    core::types::PhotoId photoId, const core::types::SourceLocator& locator)
{
    return m_catalogOrchestrator->relinkSource(photoId, locator);
}

// 목적: Activity adapter가 current Editor preview cancellation을 owner에 전달
// 입력: requestId: accepted preview request identity
// 출력: current request를 취소했으면 true
bool CatalogEditorFacade::cancelPreviewActivity(core::types::RequestId requestId)
{
    return m_editorOrchestrator->cancelPreviewRequest(requestId);
}

// 목적: Activity adapter가 source verification cancellation을 owner에 전달
// 입력: requestId: accepted source request identity
// 출력: active source request를 취소했으면 true
bool CatalogEditorFacade::cancelSourceActivity(core::types::RequestId requestId)
{
    return m_catalogOrchestrator->cancelSourceRequest(requestId);
}

// 목적: 현재 editor selection과 active preview 정리
// 입력: 없음
// 출력: 없음
void CatalogEditorFacade::clearSelection()
{
    (void)clearEditorSelection();
}

// 목적: 현재 editor params를 갱신하고 preview 예약
// 입력: params: 새 develop 값, targetSize: preview viewport 크기
// 출력: 실제 state가 변경되면 true
bool CatalogEditorFacade::updateDevelopParams(const core::types::DevelopParams& params, const QSize& targetSize)
{
    (void)updatePreviewTargetSize(targetSize);
    const core::client::EditorDevelopParams previous = editorSnapshot().params;
    const core::client::EditorResult updated =
        updateDevelopParams({core::orchestration::toClientDevelopParams(params)});
    return updated.hasValue() && updated.value().params != previous;
}

// 목적: GUI viewport resize를 Editor preview target 변경으로 전달
// 입력: targetSize: layout 적용 후 preview viewport 크기
// 출력: 실제 target 크기 변경이 예약됐으면 true
bool CatalogEditorFacade::updatePreviewTargetSize(const QSize& targetSize)
{
    return m_editorOrchestrator->updatePreviewTargetSize(targetSize);
}

// 목적: 연속 develop 조작을 하나의 undo 단계로 시작
// 입력: 없음
// 출력: 없음
void CatalogEditorFacade::beginEdit()
{
    (void)beginAdjustment();
}

// 목적: 현재 연속 develop 조작 종료
// 입력: 없음
// 출력: 없음
void CatalogEditorFacade::endEdit()
{
    (void)endAdjustment();
}

// 목적: 현재 photo의 마지막 develop 변경 되돌리기
// 입력: targetSize: preview viewport 크기
// 출력: 변경된 state 또는 undo 불가 시 빈 값
std::optional<core::orchestration::EditorState> CatalogEditorFacade::undo(const QSize& targetSize)
{
    (void)updatePreviewTargetSize(targetSize);
    const core::client::EditorResult result = undoDevelop();
    return result.hasValue()
               ? std::optional<core::orchestration::EditorState>{core::orchestration::fromClientEditorSnapshot(
                     result.value())}
               : std::nullopt;
}

// 목적: 현재 photo의 마지막 undo 변경 다시 적용
// 입력: targetSize: preview viewport 크기
// 출력: 변경된 state 또는 redo 불가 시 빈 값
std::optional<core::orchestration::EditorState> CatalogEditorFacade::redo(const QSize& targetSize)
{
    (void)updatePreviewTargetSize(targetSize);
    const core::client::EditorResult result = redoDevelop();
    return result.hasValue()
               ? std::optional<core::orchestration::EditorState>{core::orchestration::fromClientEditorSnapshot(
                     result.value())}
               : std::nullopt;
}

}  // namespace flexraw::ui::facade
