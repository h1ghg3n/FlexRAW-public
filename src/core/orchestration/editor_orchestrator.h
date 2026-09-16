#pragma once

#include <optional>

#include <QHash>
#include <QObject>
#include <QSize>
#include <QTimer>

#include "develop_history.h"
#include "editor_client.h"
#include "editor_contracts.h"

namespace flexraw::core::orchestration
{

class CatalogOrchestrator;
class PreviewOrchestrator;
struct CatalogSourceUpdate;

class EditorOrchestrator final : public QObject, public client::IEditorClient
{
    Q_OBJECT

public:
    // 목적: editor session을 catalog-backed persistence와 preview use case에 연결
    // 입력: previewOrchestrator: preview 실행자, catalogOrchestrator: catalog session, parent: Qt 부모 object
    // 출력: persisted develop state를 load/save할 수 있는 EditorOrchestrator 객체
    EditorOrchestrator(PreviewOrchestrator& previewOrchestrator,
                       CatalogOrchestrator& catalogOrchestrator,
                       QObject* parent = nullptr);

    // 목적: editor session의 debounce와 active preview를 수명 종료 전에 취소
    // 입력: 없음
    // 출력: session-owned request가 남지 않음
    ~EditorOrchestrator() override;

    // 목적: 현재 Editor session의 immutable Qt-free state 조회
    // 입력: 없음
    // 출력: selection, Develop state, source capability와 history snapshot
    [[nodiscard]] client::EditorSnapshot editorSnapshot() const override;

    // 목적: active Catalog의 stable PhotoId를 현재 Editor session으로 선택
    // 입력: command: 선택할 fixed-width Photo identity
    // 출력: 선택 후 snapshot 또는 validation·Catalog·Develop load 오류
    [[nodiscard]] client::EditorResult selectPhoto(const client::SelectEditorPhotoCommand& command) override;

    // 목적: 현재 Editor selection과 진행 중 Adjustment를 정리
    // 입력: 없음
    // 출력: 선택되지 않은 snapshot
    [[nodiscard]] client::EditorResult clearEditorSelection() override;

    // 목적: 현재 Photo의 Develop parameter를 검증하고 session state에 반영
    // 입력: command: Qt-free Develop parameter 전체 값
    // 출력: 변경 후 snapshot 또는 selection·source·validation 오류
    [[nodiscard]] client::EditorResult updateDevelopParams(const client::UpdateDevelopParamsCommand& command) override;

    // 목적: 연속 Develop parameter 조작을 하나의 undo 단위로 시작
    // 입력: 없음
    // 출력: Adjustment가 활성화된 snapshot 또는 현재 state 오류
    [[nodiscard]] client::EditorResult beginAdjustment() override;

    // 목적: 현재 연속 Adjustment를 종료하고 undo 단위를 확정
    // 입력: 없음
    // 출력: Adjustment가 종료된 snapshot 또는 현재 state 오류
    [[nodiscard]] client::EditorResult endAdjustment() override;

    // 목적: 현재 Photo의 마지막 Develop Adjustment를 되돌림
    // 입력: 없음
    // 출력: 되돌린 snapshot 또는 selection·history 상태 오류
    [[nodiscard]] client::EditorResult undoDevelop() override;

    // 목적: 현재 Photo에서 마지막으로 되돌린 Develop Adjustment를 다시 적용
    // 입력: 없음
    // 출력: 다시 적용한 snapshot 또는 selection·history 상태 오류
    [[nodiscard]] client::EditorResult redoDevelop() override;

    // 목적: dirty Develop state를 optimistic persisted revision으로 저장
    // 입력: 없음
    // 출력: 저장된 baseline과 revision snapshot 또는 conflict·database 오류
    [[nodiscard]] client::EditorResult saveDevelopState() override;

    // 목적: 현재 editor session의 immutable state snapshot 반환
    // 입력: 없음
    // 출력: 선택 사진, params, revision과 history 상태
    [[nodiscard]] EditorState state() const;

    // 목적: Folder photo를 active Catalog에 등록·resolve하고 stable Editor session 시작
    // 입력: entry: scan된 supported photo, targetSize: 현재 preview viewport 크기
    // 출력: 선택 state 또는 registration·catalog/source validation 오류
    [[nodiscard]] EditorStateResult activatePhoto(const catalog::CatalogEntry& entry, const QSize& targetSize);

    // 목적: catalog PhotoId를 resolve하고 persisted develop state로 editor selection 구성
    // 입력: photoId: catalog-local identity, targetSize: 현재 preview viewport 크기
    // 출력: 선택 state 또는 catalog/source validation 오류
    [[nodiscard]] EditorStateResult selectCatalogPhoto(types::PhotoId photoId, const QSize& targetSize);

    // 목적: 현재 catalog-backed 사진의 develop state를 optimistic revision으로 저장
    // 입력: 없음
    // 출력: persisted baseline이 갱신된 state 또는 conflict·session 오류
    [[nodiscard]] EditorStateResult saveCurrentPhoto();

    // 목적: 현재 선택과 진행 중 preview를 정리하되 사진별 in-memory history 유지
    // 입력: 없음
    // 출력: 선택되지 않은 editor state
    void clearSelection();

    // 목적: 현재 사진의 develop params를 갱신하고 debounce된 preview 예약
    // 입력: params: 새 develop 값, targetSize: 현재 preview viewport 크기
    // 출력: 실제 state가 변경되면 true
    [[nodiscard]] bool updateDevelopParams(const types::DevelopParams& params, const QSize& targetSize);

    // 목적: 변경된 preview viewport 크기를 반영해 resize가 끝난 뒤 final preview 예약
    // 입력: targetSize: layout 적용 후 preview viewport 크기
    // 출력: 유효한 선택에서 실제 target 크기가 변경됐으면 true
    [[nodiscard]] bool updatePreviewTargetSize(const QSize& targetSize);

    // 목적: Activity client가 지정한 현재 preview request를 실제 owner에서 취소
    // 입력: requestId: 현재 Editor session의 accepted preview identity
    // 출력: current request를 취소했으면 true
    bool cancelPreviewRequest(types::RequestId requestId);

    // 목적: 연속 parameter 조작을 하나의 undo 단계로 시작
    // 입력: 없음
    // 출력: 없음
    void beginEdit();

    // 목적: 현재 연속 parameter 조작을 종료
    // 입력: 없음
    // 출력: 없음
    void endEdit();

    // 목적: 현재 사진의 마지막 develop 변경을 되돌리고 preview 예약
    // 입력: targetSize: 현재 preview viewport 크기
    // 출력: 변경된 editor state 또는 가능한 undo가 없으면 빈 값
    [[nodiscard]] std::optional<EditorState> undo(const QSize& targetSize);

    // 목적: 현재 사진에서 되돌린 develop 변경을 다시 적용하고 preview 예약
    // 입력: targetSize: 현재 preview viewport 크기
    // 출력: 변경된 editor state 또는 가능한 redo가 없으면 빈 값
    [[nodiscard]] std::optional<EditorState> redo(const QSize& targetSize);

signals:
    // 목적: 선택, params 또는 history 상태 변경을 adapter에 전달
    // 입력: state: 변경 후 immutable editor state
    // 출력: 없음
    void stateChanged(const EditorState& state);

    // 목적: Qt-free Editor snapshot 의미가 변경됐음을 event adapter에 알림
    // 입력: 없음; adapter가 owner context에서 immutable snapshot을 즉시 capture
    // 출력: 없음
    void editorSnapshotChanged();

    // 목적: 현재 Editor preview request가 owner에 accepted됐음을 adapter에 전달
    // 입력: requestId: accepted preview request identity
    // 출력: 없음
    void previewStarted(types::RequestId requestId);

    // 목적: 현재 editor state와 일치하는 progressive preview 전달
    // 입력: result: stale result가 제거된 preview frame
    // 출력: 없음
    void previewUpdated(const PreviewResult& result);

    // 목적: 현재 preview의 비치명적 pipeline 경고 전달
    // 입력: issue: 현재 request identity와 technical error
    // 출력: 없음
    void previewWarning(const PreviewIssue& issue);

    // 목적: 현재 preview request 정상 완료 전달
    // 입력: requestId: 완료된 request 식별자
    // 출력: 없음
    void previewCompleted(types::RequestId requestId);

    // 목적: 현재 preview 제출 또는 pipeline 실패 전달
    // 입력: issue: 현재 editor snapshot과 technical error
    // 출력: 없음
    void previewFailed(const PreviewIssue& issue);

    // 목적: 외부 원인으로 현재 preview request가 취소됐음을 전달
    // 입력: requestId: 취소된 request 식별자
    // 출력: 없음
    void previewCancelled(types::RequestId requestId);

private:
    // 목적: 공통 timer와 preview event 연결을 먼저 구성하는 delegating constructor
    // 입력: previewOrchestrator: preview request 실행자, parent: Qt 부모 object
    // 출력: public catalog-backed constructor가 완성할 내부 EditorOrchestrator 객체
    explicit EditorOrchestrator(PreviewOrchestrator& previewOrchestrator, QObject* parent);

    enum class PreviewTiming
    {
        Debounced,
        Immediate,
    };

    // 목적: transitional Qt state와 Qt-free snapshot invalidation을 한 state transition에서 publish
    // 입력: currentState: 변경이 끝난 현재 Editor state
    // 출력: 기존 GUI signal과 client event adapter 알림
    void publishStateChanged(const EditorState& currentState);

    // 목적: 현재 state를 full-quality final preview로 예약
    // 입력: targetSize: preview viewport 크기, progression: source tier 정책, timing: 제출 시점
    // 출력: 기존 request 취소와 final preview sequence 증가
    void scheduleFinalPreview(const QSize& targetSize, PreviewProgression progression, PreviewTiming timing);

    // 목적: 연속 입력의 최신 state를 최대 frame rate로 제한해 interactive preview 예약
    // 입력: targetSize: preview viewport 크기
    // 출력: leading request 즉시 제출 또는 trailing request coalescing
    void scheduleInteractivePreview(const QSize& targetSize);

    // 목적: throttle 구간에 coalescing된 최신 interactive preview 제출
    // 입력: 없음
    // 출력: pending request 제출과 다음 throttle 구간 시작
    void submitPendingInteractivePreview();

    // 목적: 현재 edit transaction과 interactive scheduling 상태 종료
    // 입력: submitFinalPreview: 변경된 state의 final preview 즉시 제출 여부
    // 출력: history transaction 종료와 선택적 final request 제출
    void finishEdit(bool submitFinalPreview);

    // 목적: 마지막 editor 입력을 immutable preview request로 제출
    // 입력: 없음
    // 출력: accepted request 저장 또는 previewFailed signal
    void submitPreview();

    // 목적: 현재 accepted preview request의 향후 결과 publish 취소
    // 입력: 없음
    // 출력: active owner request를 취소했으면 true
    bool cancelActivePreview();

    // 목적: preview 결과가 현재 editor selection과 revision에 일치하는지 확인
    // 입력: result: PreviewOrchestrator가 전달한 결과
    // 출력: adapter에 전달 가능한 현재 결과이면 true
    [[nodiscard]] bool isCurrentPreview(const PreviewResult& result) const;

    // 목적: 현재 선택 PhotoId의 background source binding transition을 editor state에 반영
    // 입력: update: 완료된 fingerprint request와 갱신된 catalog photo
    // 출력: processing availability와 필요 시 progressive preview 갱신
    void handleSourceBindingUpdated(const CatalogSourceUpdate& update);

    // 목적: 현재 선택 사진의 persisted 또는 session baseline 대비 dirty 여부 계산
    // 입력: 없음
    // 출력: baseline과 현재 params가 다르면 true
    [[nodiscard]] bool isDirty() const;

    // 목적: stable catalog identity로 editor-session history key 구성
    // 입력: 없음
    // 출력: 현재 catalog selection의 session history key 또는 선택이 없으면 빈 문자열
    [[nodiscard]] QString currentHistoryKey() const;

    PreviewOrchestrator* m_previewOrchestrator{nullptr};
    CatalogOrchestrator* m_catalogOrchestrator{nullptr};
    history::DevelopHistory m_developHistory;
    QHash<QString, types::DevelopParams> m_sessionBaselines;
    QHash<QString, types::DevelopRevision> m_persistedRevisions;
    QTimer m_previewDebounceTimer;
    QTimer m_previewResizeDebounceTimer;
    QTimer m_interactiveThrottleTimer;
    std::optional<types::PhotoId> m_currentCatalogPhotoId;
    QString m_currentCatalogIdentity;
    std::optional<catalog::SourceBindingState> m_currentSourceState;
    types::FileDescriptor m_currentSource;
    types::DevelopParams m_currentParams;
    QSize m_previewTargetSize;
    PreviewProgression m_previewProgression{PreviewProgression::Progressive};
    PreviewRenderMode m_previewRenderMode{PreviewRenderMode::Final};
    types::RequestId m_activePreviewRequestId{0};
    types::PreviewSequence m_previewSequence{0};
    bool m_editInProgress{false};
    bool m_editPreviewChanged{false};
    bool m_interactivePreviewPending{false};
    bool m_sourceProcessingAllowed{false};
    SourceResolutionCapabilities m_sourceResolution;
};

}  // namespace flexraw::core::orchestration
