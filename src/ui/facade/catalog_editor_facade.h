#pragma once

#include <optional>

#include <QObject>
#include <QSize>

#include "catalog_contracts.h"
#include "editor_contracts.h"
#include "preview_contracts.h"

namespace flexraw::core::orchestration
{
class CatalogOrchestrator;
class EditorOrchestrator;
}  // namespace flexraw::core::orchestration

namespace flexraw::ui::facade
{

class CatalogEditorFacade final : public QObject
{
    Q_OBJECT

public:
    // 목적: GUI adapter가 사용할 Catalog/Editor command와 event 경계 구성
    // 입력: catalogOrchestrator: catalog use case, editorOrchestrator: editor session use case, parent: Qt 부모 object
    // 출력: 두 Orchestrator를 감싼 facade 객체
    CatalogEditorFacade(core::orchestration::CatalogOrchestrator& catalogOrchestrator,
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
    // 목적: facade consumer에 editor state 변경 전달
    // 입력: state: 변경 후 immutable editor state
    // 출력: 없음
    void editorStateChanged(const core::orchestration::EditorState& state);

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
    core::orchestration::EditorOrchestrator* m_editorOrchestrator{nullptr};
};

}  // namespace flexraw::ui::facade
