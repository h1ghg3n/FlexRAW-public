#include "catalog_editor_facade.h"

#include "catalog_orchestrator.h"
#include "editor_orchestrator.h"

namespace flexraw::ui::facade
{

// 목적: GUI adapter가 사용할 Catalog/Editor command와 event 경계 구성
// 입력: catalogOrchestrator: catalog use case, editorOrchestrator: editor session use case, parent: Qt 부모 object
// 출력: 두 Orchestrator를 감싼 facade 객체
CatalogEditorFacade::CatalogEditorFacade(core::orchestration::CatalogOrchestrator& catalogOrchestrator,
                                         core::orchestration::EditorOrchestrator& editorOrchestrator,
                                         QObject* parent)
    : QObject(parent), m_catalogOrchestrator(&catalogOrchestrator), m_editorOrchestrator(&editorOrchestrator)
{
    connect(m_editorOrchestrator,
            &core::orchestration::EditorOrchestrator::stateChanged,
            this,
            &CatalogEditorFacade::editorStateChanged);
    connect(m_editorOrchestrator,
            &core::orchestration::EditorOrchestrator::previewUpdated,
            this,
            &CatalogEditorFacade::previewUpdated);
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
    return m_editorOrchestrator->state();
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
    return m_editorOrchestrator->activatePhoto(entry, targetSize);
}

// 목적: stable PhotoId photo를 persisted develop state와 함께 선택
// 입력: photoId: catalog-local identity, targetSize: preview viewport 크기
// 출력: 선택 후 editor state 또는 오류
core::orchestration::EditorStateResult CatalogEditorFacade::selectCatalogPhoto(core::types::PhotoId photoId,
                                                                               const QSize& targetSize)
{
    return m_editorOrchestrator->selectCatalogPhoto(photoId, targetSize);
}

// 목적: 현재 catalog-backed photo의 develop state 저장
// 입력: 없음
// 출력: 저장 후 editor state 또는 오류
core::orchestration::EditorStateResult CatalogEditorFacade::saveCurrentPhoto()
{
    return m_editorOrchestrator->saveCurrentPhoto();
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

// 목적: 현재 editor selection과 active preview 정리
// 입력: 없음
// 출력: 없음
void CatalogEditorFacade::clearSelection()
{
    m_editorOrchestrator->clearSelection();
}

// 목적: 현재 editor params를 갱신하고 preview 예약
// 입력: params: 새 develop 값, targetSize: preview viewport 크기
// 출력: 실제 state가 변경되면 true
bool CatalogEditorFacade::updateDevelopParams(const core::types::DevelopParams& params, const QSize& targetSize)
{
    return m_editorOrchestrator->updateDevelopParams(params, targetSize);
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
    m_editorOrchestrator->beginEdit();
}

// 목적: 현재 연속 develop 조작 종료
// 입력: 없음
// 출력: 없음
void CatalogEditorFacade::endEdit()
{
    m_editorOrchestrator->endEdit();
}

// 목적: 현재 photo의 마지막 develop 변경 되돌리기
// 입력: targetSize: preview viewport 크기
// 출력: 변경된 state 또는 undo 불가 시 빈 값
std::optional<core::orchestration::EditorState> CatalogEditorFacade::undo(const QSize& targetSize)
{
    return m_editorOrchestrator->undo(targetSize);
}

// 목적: 현재 photo의 마지막 undo 변경 다시 적용
// 입력: targetSize: preview viewport 크기
// 출력: 변경된 state 또는 redo 불가 시 빈 값
std::optional<core::orchestration::EditorState> CatalogEditorFacade::redo(const QSize& targetSize)
{
    return m_editorOrchestrator->redo(targetSize);
}

}  // namespace flexraw::ui::facade
