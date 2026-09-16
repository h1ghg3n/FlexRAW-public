#include "editor_orchestrator.h"

#include <limits>

#include <QByteArray>
#include <QDir>
#include <QFileInfo>

#include "catalog_orchestrator.h"
#include "client_error_projection.h"
#include "develop.h"
#include "editor_client_projection.h"
#include "preview_orchestrator.h"

namespace flexraw::core::orchestration
{
namespace
{

constexpr int PreviewDebounceMs = 40;
constexpr int PreviewResizeDebounceMs = 150;
constexpr int InteractivePreviewIntervalMs = 33;

// 목적: Editor client command가 사용할 수 있는 양의 Preview target인지 확인
// 입력: targetSize: adapter가 마지막으로 전달한 viewport 크기
// 출력: width와 height가 모두 양수이면 true
[[nodiscard]] bool hasPreviewTarget(const QSize& targetSize) noexcept
{
    return targetSize.width() > 0 && targetSize.height() > 0;
}

// 목적: Qt-free Editor command의 현재 state 오류 result 생성
// 입력: code: 오류 분류, message: log와 diagnostics용 설명
// 출력: ClientError를 보유한 실패 result
[[nodiscard]] client::EditorResult makeClientFailure(types::ErrorCode code, const QString& message)
{
    return client::EditorResult::failure(toClientError({code, message}));
}

// 목적: Qt-free UTF-8 contract string을 손실 없이 internal QString으로 변환
// 입력: value: UTF-8로 선언된 byte string
// 출력: round-trip 가능한 Unicode text 또는 invalid UTF-8이면 빈 값
[[nodiscard]] std::optional<QString> decodeClientString(const std::string& value)
{
    const QByteArray bytes(value.data(), static_cast<qsizetype>(value.size()));
    const QString decoded = QString::fromUtf8(bytes);
    return decoded.toUtf8() == bytes ? std::optional<QString>{decoded} : std::nullopt;
}

// 목적: Qt-free Catalog file kind를 internal processing kind로 변환
// 입력: kind: Editor source command의 file 종류
// 출력: 같은 의미의 Core enum
[[nodiscard]] types::SupportedFileKind fromClientFileKind(client::CatalogFileKind kind) noexcept
{
    switch (kind)
    {
    case client::CatalogFileKind::Raw:
        return types::SupportedFileKind::Raw;
    case client::CatalogFileKind::RasterImage:
        return types::SupportedFileKind::RasterImage;
    case client::CatalogFileKind::Unknown:
        return types::SupportedFileKind::Unknown;
    }
    return types::SupportedFileKind::Unknown;
}

// 목적: catalog photo record의 current locator를 preview source descriptor로 변환
// 입력: photo: stable identity와 optional source binding을 포함한 catalog record
// 출력: source가 연결돼 있으면 descriptor, 아니면 빈 descriptor
[[nodiscard]] types::FileDescriptor makeSourceDescriptor(const catalog::CatalogPhotoRecord& photo)
{
    if (!photo.source.has_value())
    {
        return {};
    }

    return {photo.source->path, photo.extension, photo.displayName, photo.kind};
}

// 목적: persisted source record를 frontend가 직접 실행 가능한 resolution capability로 투영
// 입력: photo: source state, locator와 fingerprint baseline을 가진 catalog record
// 출력: 현재 state에서 유효한 명시적 source command 집합
[[nodiscard]] SourceResolutionCapabilities makeSourceResolutionCapabilities(
    const catalog::CatalogPhotoRecord& photo) noexcept
{
    const bool replacementResolution = photo.sourceState == catalog::SourceBindingState::IdentityUnverified ||
                                       photo.sourceState == catalog::SourceBindingState::ReplacementDetected;
    const bool hasSource = photo.source.has_value();
    return {
        hasSource && replacementResolution,
        hasSource && replacementResolution,
        catalog::requiresSourceResolution(photo.sourceState) && types::hasSourceContentHash(photo.fingerprint),
    };
}

}  // namespace

// 목적: 공통 timer와 preview event 연결을 먼저 구성하는 delegating constructor
// 입력: previewOrchestrator: preview request 실행자, parent: Qt 부모 object
// 출력: public catalog-backed constructor가 완성할 내부 EditorOrchestrator 객체
EditorOrchestrator::EditorOrchestrator(PreviewOrchestrator& previewOrchestrator, QObject* parent)
    : QObject(parent), m_previewOrchestrator(&previewOrchestrator)
{
    m_previewDebounceTimer.setInterval(PreviewDebounceMs);
    m_previewDebounceTimer.setSingleShot(true);
    m_previewResizeDebounceTimer.setInterval(PreviewResizeDebounceMs);
    m_previewResizeDebounceTimer.setSingleShot(true);
    m_interactiveThrottleTimer.setInterval(InteractivePreviewIntervalMs);
    m_interactiveThrottleTimer.setSingleShot(true);
    connect(&m_previewDebounceTimer, &QTimer::timeout, this, &EditorOrchestrator::submitPreview);
    connect(&m_previewResizeDebounceTimer, &QTimer::timeout, this, &EditorOrchestrator::submitPreview);
    connect(&m_interactiveThrottleTimer, &QTimer::timeout, this, &EditorOrchestrator::submitPendingInteractivePreview);
    connect(m_previewOrchestrator, &PreviewOrchestrator::previewUpdated, this, [this](const PreviewResult& result) {
        if (isCurrentPreview(result))
        {
            emit previewUpdated(result);
        }
    });
    connect(m_previewOrchestrator, &PreviewOrchestrator::previewWarning, this, [this](const PreviewIssue& issue) {
        if (issue.requestId == m_activePreviewRequestId)
        {
            emit previewWarning(issue);
        }
    });
    connect(m_previewOrchestrator, &PreviewOrchestrator::previewCompleted, this, [this](types::RequestId requestId) {
        if (requestId != m_activePreviewRequestId)
        {
            return;
        }

        m_activePreviewRequestId = 0;
        emit previewCompleted(requestId);
    });
    connect(m_previewOrchestrator, &PreviewOrchestrator::previewFailed, this, [this](const PreviewIssue& issue) {
        if (issue.requestId != m_activePreviewRequestId)
        {
            return;
        }

        m_activePreviewRequestId = 0;
        emit previewFailed(issue);
    });
    connect(m_previewOrchestrator, &PreviewOrchestrator::previewCancelled, this, [this](types::RequestId requestId) {
        if (requestId != m_activePreviewRequestId)
        {
            return;
        }

        m_activePreviewRequestId = 0;
        emit previewCancelled(requestId);
    });
}

// 목적: editor session을 catalog-backed persistence와 preview use case에 연결
// 입력: previewOrchestrator: preview 실행자, catalogOrchestrator: catalog session, parent: Qt 부모 object
// 출력: persisted develop state를 load/save할 수 있는 EditorOrchestrator 객체
EditorOrchestrator::EditorOrchestrator(PreviewOrchestrator& previewOrchestrator,
                                       CatalogOrchestrator& catalogOrchestrator,
                                       QObject* parent)
    : EditorOrchestrator(previewOrchestrator, parent)
{
    m_catalogOrchestrator = &catalogOrchestrator;
    connect(m_catalogOrchestrator,
            &CatalogOrchestrator::sourceBindingUpdated,
            this,
            &EditorOrchestrator::handleSourceBindingUpdated);
}

// 목적: editor session의 debounce와 active preview를 수명 종료 전에 취소
// 입력: 없음
// 출력: session-owned request가 남지 않음
EditorOrchestrator::~EditorOrchestrator()
{
    m_previewDebounceTimer.stop();
    m_previewResizeDebounceTimer.stop();
    m_interactiveThrottleTimer.stop();
    cancelActivePreview();
}

// 목적: 현재 Editor session의 immutable Qt-free state 조회
// 입력: 없음
// 출력: selection, Develop state, source capability와 history snapshot
client::EditorSnapshot EditorOrchestrator::editorSnapshot() const
{
    return toClientEditorSnapshot(state(), m_editInProgress);
}

// 목적: Qt-free viewport command를 기존 Preview target과 resize coalescing에 전달
// 입력: command: 양수 fixed-width pixel 크기
// 출력: 적용된 viewport 또는 validation 오류
client::PreviewViewportResult EditorOrchestrator::setPreviewViewport(const client::SetPreviewViewportCommand& command)
{
    constexpr std::uint32_t MaximumQtDimension = static_cast<std::uint32_t>(std::numeric_limits<int>::max());
    if (command.viewport.width == 0 || command.viewport.height == 0 || command.viewport.width > MaximumQtDimension ||
        command.viewport.height > MaximumQtDimension)
    {
        return client::PreviewViewportResult::failure(
            {client::ClientErrorCode::InvalidArgument,
             "Preview viewport dimensions must be positive Qt pixel values."});
    }

    (void)updatePreviewTargetSize(
        QSize{static_cast<int>(command.viewport.width), static_cast<int>(command.viewport.height)});
    return client::PreviewViewportResult::success(command.viewport);
}

// 목적: 현재 accepted Preview request의 향후 frame publication과 processing 취소
// 입력: requestId: owner가 발급해 presentation snapshot에 공개한 identity
// 출력: 취소된 identity 또는 stale·validation 오류
client::PreviewRequestCancelResult EditorOrchestrator::cancelPreviewRequest(client::PreviewRequestId requestId)
{
    if (requestId.value == 0)
    {
        return client::PreviewRequestCancelResult::failure(
            {client::ClientErrorCode::InvalidArgument, "Preview request identity must be positive."});
    }
    if (!cancelPreviewRequest(static_cast<types::RequestId>(requestId.value)))
    {
        return client::PreviewRequestCancelResult::failure(
            {client::ClientErrorCode::NotFound, "Preview request is no longer active."});
    }
    return client::PreviewRequestCancelResult::success(requestId);
}

// 목적: Preview presentation adapter가 현재 target 크기를 owner context에서 조회
// 입력: 없음
// 출력: 아직 설정되지 않았으면 빈 QSize, 아니면 양수 pixel 크기
QSize EditorOrchestrator::previewTargetSize() const noexcept
{
    return m_previewTargetSize;
}

// 목적: Preview presentation adapter가 현재 latest-wins sequence를 조회
// 입력: 없음
// 출력: 선택·편집·resize transition에 따라 증가한 sequence
types::PreviewSequence EditorOrchestrator::previewSequence() const noexcept
{
    return m_previewSequence;
}

// 목적: Preview presentation adapter가 현재 accepted request identity를 조회
// 입력: 없음
// 출력: active request가 없으면 0, 있으면 owner-issued identity
types::RequestId EditorOrchestrator::activePreviewRequestId() const noexcept
{
    return m_activePreviewRequestId;
}

// 목적: Folder scan source를 active Catalog에 등록·resolve하고 Editor session으로 선택
// 입력: command: normalized absolute UTF-8 source와 표시 metadata
// 출력: stable Photo identity가 발급된 snapshot 또는 validation·Catalog 오류
client::EditorResult EditorOrchestrator::activateSource(const client::ActivateEditorSourceCommand& command)
{
    const std::optional<QString> sourceLocator = decodeClientString(command.sourceLocator);
    const std::optional<QString> extension = decodeClientString(command.extension);
    const std::optional<QString> displayName = decodeClientString(command.displayName);
    if (!sourceLocator.has_value() || !extension.has_value() || !displayName.has_value() ||
        command.kind == client::CatalogFileKind::Unknown)
    {
        return makeClientFailure(types::ErrorCode::InvalidArgument,
                                 QStringLiteral("Editor source command contains invalid UTF-8 or file kind."));
    }

    const QString normalizedLocator = QDir::cleanPath(QDir::fromNativeSeparators(sourceLocator->trimmed()));
    if (normalizedLocator.isEmpty() || normalizedLocator == QStringLiteral(".") ||
        !QFileInfo(normalizedLocator).isAbsolute() || normalizedLocator != *sourceLocator ||
        extension->trimmed().isEmpty() || displayName->trimmed().isEmpty())
    {
        return makeClientFailure(types::ErrorCode::InvalidArgument,
                                 QStringLiteral("Editor source command metadata is invalid or not normalized."));
    }

    const catalog::CatalogEntry entry{
        {normalizedLocator, extension->trimmed(), displayName->trimmed(), fromClientFileKind(command.kind)},
        types::FileScanStatus::Ready};
    const EditorStateResult selected = activatePhoto(entry, m_previewTargetSize);
    return selected.hasError() ? client::EditorResult::failure(toClientError(selected.error()))
                               : client::EditorResult::success(toClientEditorSnapshot(selected.value(), false));
}

// 목적: active Catalog의 stable PhotoId를 현재 Editor session으로 선택
// 입력: command: 선택할 fixed-width Photo identity
// 출력: 선택 후 snapshot 또는 validation·Catalog·Develop load 오류
client::EditorResult EditorOrchestrator::selectPhoto(const client::SelectEditorPhotoCommand& command)
{
    if (command.photoId.value <= 0)
    {
        return makeClientFailure(types::ErrorCode::InvalidArgument, QStringLiteral("Editor PhotoId must be positive."));
    }

    const EditorStateResult selected = selectCatalogPhoto(types::PhotoId{command.photoId.value}, m_previewTargetSize);
    return selected.hasError() ? client::EditorResult::failure(toClientError(selected.error()))
                               : client::EditorResult::success(toClientEditorSnapshot(selected.value(), false));
}

// 목적: 현재 Editor selection과 해당 Photo의 session-local Develop state를 폐기
// 입력: 없음
// 출력: 다른 Photo history와 persisted data를 유지한 선택되지 않은 snapshot
client::EditorResult EditorOrchestrator::clearEditorSelection()
{
    clearSelection();
    return client::EditorResult::success(editorSnapshot());
}

// 목적: 현재 Photo의 Develop parameter를 검증하고 session state에 반영
// 입력: command: Qt-free Develop parameter 전체 값
// 출력: 변경 후 snapshot 또는 selection·source·validation 오류
client::EditorResult EditorOrchestrator::updateDevelopParams(const client::UpdateDevelopParamsCommand& command)
{
    const EditorState currentState = state();
    if (!currentState.hasSelection)
    {
        return makeClientFailure(types::ErrorCode::InvalidArgument, QStringLiteral("No Editor photo is selected."));
    }
    if (!currentState.sourceProcessingAllowed || currentState.source.path.isEmpty())
    {
        return makeClientFailure(types::ErrorCode::Conflict,
                                 QStringLiteral("The selected photo source does not allow Develop changes."));
    }

    const develop::DevelopParamsValidationResult validation =
        develop::validateDevelopParams(fromClientDevelopParams(command.params));
    if (validation.hasError())
    {
        return client::EditorResult::failure(toClientError(validation.error()));
    }
    if (validation.value() == m_currentParams)
    {
        return client::EditorResult::success(editorSnapshot());
    }
    if (!updateDevelopParams(validation.value(), m_previewTargetSize))
    {
        return makeClientFailure(types::ErrorCode::Unknown,
                                 QStringLiteral("Editor Develop state could not be updated."));
    }
    return client::EditorResult::success(editorSnapshot());
}

// 목적: 연속 Develop parameter 조작을 하나의 undo 단위로 시작
// 입력: 없음
// 출력: Adjustment가 활성화된 snapshot 또는 현재 state 오류
client::EditorResult EditorOrchestrator::beginAdjustment()
{
    const EditorState currentState = state();
    if (!currentState.hasSelection)
    {
        return makeClientFailure(types::ErrorCode::InvalidArgument, QStringLiteral("No Editor photo is selected."));
    }
    if (!currentState.sourceProcessingAllowed || currentState.source.path.isEmpty())
    {
        return makeClientFailure(types::ErrorCode::Conflict,
                                 QStringLiteral("The selected photo source does not allow an Adjustment."));
    }
    if (m_editInProgress)
    {
        return makeClientFailure(types::ErrorCode::Conflict, QStringLiteral("An Adjustment is already active."));
    }

    beginEdit();
    return client::EditorResult::success(editorSnapshot());
}

// 목적: 현재 연속 Adjustment를 종료하고 undo 단위를 확정
// 입력: 없음
// 출력: Adjustment가 종료된 snapshot 또는 현재 state 오류
client::EditorResult EditorOrchestrator::endAdjustment()
{
    if (!m_editInProgress)
    {
        return makeClientFailure(types::ErrorCode::Conflict, QStringLiteral("No Adjustment is active."));
    }

    endEdit();
    return client::EditorResult::success(editorSnapshot());
}

// 목적: 현재 Photo의 마지막 Develop Adjustment를 되돌림
// 입력: 없음
// 출력: 되돌린 snapshot 또는 selection·history 상태 오류
client::EditorResult EditorOrchestrator::undoDevelop()
{
    const EditorState currentState = state();
    if (!currentState.hasSelection)
    {
        return makeClientFailure(types::ErrorCode::InvalidArgument, QStringLiteral("No Editor photo is selected."));
    }
    if (m_editInProgress)
    {
        return makeClientFailure(types::ErrorCode::Conflict,
                                 QStringLiteral("The active Adjustment must finish before undo."));
    }
    if (!currentState.sourceProcessingAllowed || !currentState.canUndo)
    {
        return makeClientFailure(types::ErrorCode::Conflict, QStringLiteral("No Develop Adjustment can be undone."));
    }

    const std::optional<EditorState> undone = undo(m_previewTargetSize);
    return undone.has_value()
               ? client::EditorResult::success(toClientEditorSnapshot(*undone, false))
               : makeClientFailure(types::ErrorCode::Unknown, QStringLiteral("Develop undo did not produce a state."));
}

// 목적: 현재 Photo에서 마지막으로 되돌린 Develop Adjustment를 다시 적용
// 입력: 없음
// 출력: 다시 적용한 snapshot 또는 selection·history 상태 오류
client::EditorResult EditorOrchestrator::redoDevelop()
{
    const EditorState currentState = state();
    if (!currentState.hasSelection)
    {
        return makeClientFailure(types::ErrorCode::InvalidArgument, QStringLiteral("No Editor photo is selected."));
    }
    if (m_editInProgress)
    {
        return makeClientFailure(types::ErrorCode::Conflict,
                                 QStringLiteral("The active Adjustment must finish before redo."));
    }
    if (!currentState.sourceProcessingAllowed || !currentState.canRedo)
    {
        return makeClientFailure(types::ErrorCode::Conflict, QStringLiteral("No Develop Adjustment can be redone."));
    }

    const std::optional<EditorState> redone = redo(m_previewTargetSize);
    return redone.has_value()
               ? client::EditorResult::success(toClientEditorSnapshot(*redone, false))
               : makeClientFailure(types::ErrorCode::Unknown, QStringLiteral("Develop redo did not produce a state."));
}

// 목적: dirty Develop state를 optimistic persisted revision으로 저장
// 입력: 없음
// 출력: 저장된 baseline과 revision snapshot 또는 conflict·database 오류
client::EditorResult EditorOrchestrator::saveDevelopState()
{
    const bool adjustmentWasActive = m_editInProgress;
    const EditorStateResult saved = saveCurrentPhoto();
    if (saved.hasError() && adjustmentWasActive != m_editInProgress)
    {
        emit editorSnapshotChanged();
    }
    return saved.hasError() ? client::EditorResult::failure(toClientError(saved.error()))
                            : client::EditorResult::success(toClientEditorSnapshot(saved.value(), false));
}

// 목적: 현재 editor session의 immutable state snapshot 반환
// 입력: 없음
// 출력: 선택 사진, params, revision과 history 상태
EditorState EditorOrchestrator::state() const
{
    const QString photoKey = currentHistoryKey();
    const bool hasSelection = !photoKey.isEmpty();
    const types::DevelopRevision persistedRevision = m_persistedRevisions.value(photoKey, 0);
    const types::PhotoId photoId = m_currentCatalogPhotoId.value_or(types::PhotoId{});
    return {
        hasSelection,
        {photoId, {}, m_developHistory.revision(photoKey)},
        persistedRevision,
        m_currentSource,
        m_currentParams,
        hasSelection && m_sourceProcessingAllowed,
        m_currentSourceState,
        hasSelection ? m_sourceResolution : SourceResolutionCapabilities{},
        hasSelection && isDirty(),
        hasSelection && m_developHistory.canUndo(photoKey),
        hasSelection && m_developHistory.canRedo(photoKey),
    };
}

// 목적: Folder photo를 active Catalog에 등록·resolve하고 stable Editor session 시작
// 입력: entry: scan된 supported photo, targetSize: 현재 preview viewport 크기
// 출력: 선택 state 또는 registration·catalog/source validation 오류
EditorStateResult EditorOrchestrator::activatePhoto(const catalog::CatalogEntry& entry, const QSize& targetSize)
{
    if (m_catalogOrchestrator == nullptr)
    {
        return EditorStateResult::failure(
            {types::ErrorCode::InvalidArgument, QStringLiteral("Editor has no catalog persistence dependency.")});
    }

    const CatalogPhotoRegistrationResult registered = m_catalogOrchestrator->registerPhoto(entry);
    if (registered.hasError())
    {
        return EditorStateResult::failure(registered.error());
    }

    return selectCatalogPhoto(registered.value(), targetSize);
}

// 목적: catalog PhotoId를 resolve하고 persisted develop state로 editor selection 구성
// 입력: photoId: catalog-local identity, targetSize: 현재 preview viewport 크기
// 출력: 선택 state 또는 catalog/source validation 오류
EditorStateResult EditorOrchestrator::selectCatalogPhoto(types::PhotoId photoId, const QSize& targetSize)
{
    if (m_catalogOrchestrator == nullptr)
    {
        return EditorStateResult::failure(
            {types::ErrorCode::InvalidArgument, QStringLiteral("Editor has no catalog persistence dependency.")});
    }

    const CatalogPhotoStateResult resolved = m_catalogOrchestrator->resolvePhoto(photoId);
    if (resolved.hasError())
    {
        return EditorStateResult::failure(resolved.error());
    }

    finishEdit(false);
    m_previewDebounceTimer.stop();
    m_previewResizeDebounceTimer.stop();
    cancelActivePreview();
    m_currentCatalogPhotoId = photoId;
    m_currentCatalogIdentity = m_catalogOrchestrator->state().catalogPath;
    m_currentSourceState = resolved.value().photo.sourceState;
    m_currentSource = makeSourceDescriptor(resolved.value().photo);
    m_sourceProcessingAllowed = resolved.value().sourceProcessingAllowed;
    m_sourceResolution = makeSourceResolutionCapabilities(resolved.value().photo);
    const QString photoKey = currentHistoryKey();
    m_currentParams = m_developHistory.selectPhoto(photoKey, resolved.value().developParams);

    if (!m_sessionBaselines.contains(photoKey))
    {
        m_sessionBaselines.insert(photoKey, resolved.value().developParams);
        m_persistedRevisions.insert(photoKey, resolved.value().persistedRevision);
    }

    if (m_sourceProcessingAllowed && !m_currentSource.path.isEmpty() && hasPreviewTarget(targetSize))
    {
        scheduleFinalPreview(targetSize, PreviewProgression::Progressive, PreviewTiming::Debounced);
    }
    else
    {
        ++m_previewSequence;
        m_previewTargetSize = targetSize;
    }

    const EditorState currentState = state();
    publishStateChanged();
    return EditorStateResult::success(currentState);
}

// 목적: 현재 catalog-backed 사진의 develop state를 optimistic revision으로 저장
// 입력: 없음
// 출력: persisted baseline이 갱신된 state 또는 conflict·session 오류
EditorStateResult EditorOrchestrator::saveCurrentPhoto()
{
    if (m_catalogOrchestrator == nullptr || !m_currentCatalogPhotoId.has_value())
    {
        return EditorStateResult::failure(
            {types::ErrorCode::InvalidArgument, QStringLiteral("No catalog-backed photo is selected.")});
    }

    const CatalogSessionState catalogState = m_catalogOrchestrator->state();
    if (!catalogState.isOpen || catalogState.catalogPath != m_currentCatalogIdentity)
    {
        return EditorStateResult::failure(
            {types::ErrorCode::Conflict, QStringLiteral("Editor catalog session changed before save.")});
    }

    finishEdit(false);
    const types::PhotoId photoId = *m_currentCatalogPhotoId;
    const QString photoKey = currentHistoryKey();
    const types::DevelopRevision expectedRevision = m_persistedRevisions.value(photoKey, 0);
    const CatalogPhotoStateResult saved =
        m_catalogOrchestrator->saveDevelopState(photoId, m_currentParams, expectedRevision);

    if (saved.hasError())
    {
        return EditorStateResult::failure(saved.error());
    }

    m_sessionBaselines.insert(photoKey, saved.value().developParams);
    m_persistedRevisions.insert(photoKey, saved.value().persistedRevision);
    m_sourceProcessingAllowed = saved.value().sourceProcessingAllowed;
    m_currentSourceState = saved.value().photo.sourceState;
    m_currentSource = makeSourceDescriptor(saved.value().photo);
    m_sourceResolution = makeSourceResolutionCapabilities(saved.value().photo);
    const EditorState currentState = state();
    publishStateChanged();
    return EditorStateResult::success(currentState);
}

// 목적: 현재 선택과 해당 Photo의 session-local Develop state 및 진행 중 preview 정리
// 입력: 없음
// 출력: 다른 Photo history와 persisted data를 유지한 선택되지 않은 editor state
void EditorOrchestrator::clearSelection()
{
    const QString photoKey = currentHistoryKey();
    finishEdit(false);
    m_previewDebounceTimer.stop();
    m_previewResizeDebounceTimer.stop();
    cancelActivePreview();
    if (!photoKey.isEmpty())
    {
        m_developHistory.discardPhoto(photoKey);
        m_sessionBaselines.remove(photoKey);
        m_persistedRevisions.remove(photoKey);
    }
    ++m_previewSequence;
    m_currentCatalogPhotoId.reset();
    m_currentCatalogIdentity.clear();
    m_currentSourceState.reset();
    m_currentSource = {};
    m_currentParams = {};
    m_previewTargetSize = {};
    m_sourceProcessingAllowed = false;
    m_sourceResolution = {};
    publishStateChanged();
}

// 목적: 현재 사진의 develop params를 갱신하고 debounce된 preview 예약
// 입력: params: 새 develop 값, targetSize: 현재 preview viewport 크기
// 출력: 실제 state가 변경되면 true
bool EditorOrchestrator::updateDevelopParams(const types::DevelopParams& params, const QSize& targetSize)
{
    if (m_currentSource.path.isEmpty() || !m_sourceProcessingAllowed)
    {
        return false;
    }

    const QString photoKey = currentHistoryKey();
    const develop::DevelopParamsValidationResult validation = develop::validateDevelopParams(params);
    if (validation.hasError() || !m_developHistory.update(photoKey, validation.value()))
    {
        return false;
    }

    m_currentParams = validation.value();

    if (m_editInProgress)
    {
        m_editPreviewChanged = true;
        if (hasPreviewTarget(targetSize))
        {
            scheduleInteractivePreview(targetSize);
        }
    }
    else if (hasPreviewTarget(targetSize))
    {
        scheduleFinalPreview(targetSize, PreviewProgression::FinalOnly, PreviewTiming::Debounced);
    }

    publishStateChanged();
    return true;
}

// 목적: 변경된 preview viewport 크기를 반영해 resize가 끝난 뒤 final preview 예약
// 입력: targetSize: layout 적용 후 preview viewport 크기
// 출력: 실제 target 크기가 저장됐으면 true, 유효한 선택이 있으면 preview도 예약
bool EditorOrchestrator::updatePreviewTargetSize(const QSize& targetSize)
{
    if (!hasPreviewTarget(targetSize) || targetSize == m_previewTargetSize)
    {
        return false;
    }

    m_previewTargetSize = targetSize;
    if (m_currentSource.path.isEmpty() || !m_sourceProcessingAllowed)
    {
        emit previewPresentationStateChanged();
        return true;
    }

    if (m_previewDebounceTimer.isActive())
    {
        emit previewPresentationStateChanged();
        return true;
    }

    m_interactiveThrottleTimer.stop();
    m_interactivePreviewPending = false;
    m_previewProgression = PreviewProgression::FinalOnly;
    m_previewRenderMode = m_editInProgress ? PreviewRenderMode::Interactive : PreviewRenderMode::Final;
    m_editPreviewChanged = m_editPreviewChanged || m_editInProgress;
    ++m_previewSequence;
    cancelActivePreview();
    m_previewResizeDebounceTimer.start();
    emit previewPresentationStateChanged();
    return true;
}

// 목적: Activity client가 지정한 현재 preview request를 실제 owner에서 취소
// 입력: requestId: 현재 Editor session의 accepted preview identity
// 출력: current request를 취소했으면 true
bool EditorOrchestrator::cancelPreviewRequest(types::RequestId requestId)
{
    return requestId != 0 && requestId == m_activePreviewRequestId && cancelActivePreview();
}

// 목적: 연속 parameter 조작을 하나의 undo 단계로 시작
// 입력: 없음
// 출력: 없음
void EditorOrchestrator::beginEdit()
{
    if (!m_currentSource.path.isEmpty() && m_sourceProcessingAllowed && !m_editInProgress)
    {
        m_editInProgress = true;
        m_editPreviewChanged = false;
        m_developHistory.beginEdit(currentHistoryKey());
        emit editorSnapshotChanged();
    }
}

// 목적: 현재 연속 parameter 조작을 종료
// 입력: 없음
// 출력: 없음
void EditorOrchestrator::endEdit()
{
    if (!m_editInProgress)
    {
        return;
    }

    finishEdit(true);
    emit editorSnapshotChanged();
}

// 목적: 현재 사진의 마지막 develop 변경을 되돌리고 preview 예약
// 입력: targetSize: 현재 preview viewport 크기
// 출력: 변경된 editor state 또는 가능한 undo가 없으면 빈 값
std::optional<EditorState> EditorOrchestrator::undo(const QSize& targetSize)
{
    if (m_currentSource.path.isEmpty() || !m_sourceProcessingAllowed)
    {
        return std::nullopt;
    }

    finishEdit(false);
    const std::optional<types::DevelopParams> params = m_developHistory.undo(currentHistoryKey());

    if (!params.has_value())
    {
        return std::nullopt;
    }

    m_currentParams = *params;
    if (hasPreviewTarget(targetSize))
    {
        scheduleFinalPreview(targetSize, PreviewProgression::FinalOnly, PreviewTiming::Debounced);
    }
    const EditorState currentState = state();
    publishStateChanged();
    return currentState;
}

// 목적: 현재 사진에서 되돌린 develop 변경을 다시 적용하고 preview 예약
// 입력: targetSize: 현재 preview viewport 크기
// 출력: 변경된 editor state 또는 가능한 redo가 없으면 빈 값
std::optional<EditorState> EditorOrchestrator::redo(const QSize& targetSize)
{
    if (m_currentSource.path.isEmpty() || !m_sourceProcessingAllowed)
    {
        return std::nullopt;
    }

    finishEdit(false);
    const std::optional<types::DevelopParams> params = m_developHistory.redo(currentHistoryKey());

    if (!params.has_value())
    {
        return std::nullopt;
    }

    m_currentParams = *params;
    if (hasPreviewTarget(targetSize))
    {
        scheduleFinalPreview(targetSize, PreviewProgression::FinalOnly, PreviewTiming::Debounced);
    }
    const EditorState currentState = state();
    publishStateChanged();
    return currentState;
}

// 목적: 현재 edit transaction과 interactive scheduling 상태 종료
// 입력: submitFinalPreview: 변경된 state의 final preview 즉시 제출 여부
// 출력: history transaction 종료와 선택적 final request 제출
void EditorOrchestrator::finishEdit(bool submitFinalPreview)
{
    if (!m_editInProgress)
    {
        return;
    }

    m_developHistory.endEdit(currentHistoryKey());
    m_editInProgress = false;
    m_interactiveThrottleTimer.stop();
    m_interactivePreviewPending = false;
    const bool shouldSubmitFinal = submitFinalPreview && m_editPreviewChanged && !m_currentSource.path.isEmpty() &&
                                   hasPreviewTarget(m_previewTargetSize);
    m_editPreviewChanged = false;

    if (shouldSubmitFinal)
    {
        scheduleFinalPreview(m_previewTargetSize, PreviewProgression::FinalOnly, PreviewTiming::Immediate);
    }
}

// 목적: 현재 state를 full-quality final preview로 예약
// 입력: targetSize: preview viewport 크기, progression: source tier 정책, timing: 제출 시점
// 출력: 기존 request 취소와 final preview sequence 증가
void EditorOrchestrator::scheduleFinalPreview(const QSize& targetSize,
                                              PreviewProgression progression,
                                              PreviewTiming timing)
{
    m_previewResizeDebounceTimer.stop();
    m_interactiveThrottleTimer.stop();
    m_interactivePreviewPending = false;
    m_previewTargetSize = targetSize;
    m_previewProgression = progression;
    m_previewRenderMode = PreviewRenderMode::Final;
    ++m_previewSequence;
    cancelActivePreview();

    if (timing == PreviewTiming::Immediate)
    {
        m_previewDebounceTimer.stop();
        submitPreview();
    }
    else
    {
        m_previewDebounceTimer.start();
    }
}

// 목적: 연속 입력의 최신 state를 최대 frame rate로 제한해 interactive preview 예약
// 입력: targetSize: preview viewport 크기
// 출력: leading request 즉시 제출 또는 trailing request coalescing
void EditorOrchestrator::scheduleInteractivePreview(const QSize& targetSize)
{
    m_previewDebounceTimer.stop();
    m_previewResizeDebounceTimer.stop();
    m_previewTargetSize = targetSize;
    m_previewProgression = PreviewProgression::FinalOnly;
    m_previewRenderMode = PreviewRenderMode::Interactive;
    ++m_previewSequence;
    cancelActivePreview();

    if (m_interactiveThrottleTimer.isActive())
    {
        m_interactivePreviewPending = true;
        return;
    }

    m_interactivePreviewPending = false;
    submitPreview();
    m_interactiveThrottleTimer.start();
}

// 목적: throttle 구간에 coalescing된 최신 interactive preview 제출
// 입력: 없음
// 출력: pending request 제출과 다음 throttle 구간 시작
void EditorOrchestrator::submitPendingInteractivePreview()
{
    if (!m_interactivePreviewPending || !m_editInProgress || m_currentSource.path.isEmpty() ||
        !m_sourceProcessingAllowed)
    {
        m_interactivePreviewPending = false;
        return;
    }

    m_interactivePreviewPending = false;
    submitPreview();
    m_interactiveThrottleTimer.start();
}

// 목적: 마지막 editor 입력을 immutable preview request로 제출
// 입력: 없음
// 출력: accepted request 저장 또는 previewFailed signal
void EditorOrchestrator::submitPreview()
{
    if (m_currentSource.path.isEmpty() || !m_sourceProcessingAllowed || !hasPreviewTarget(m_previewTargetSize))
    {
        return;
    }

    const EditorState currentState = state();
    const PreviewRequest request{
        currentState.photo,
        currentState.source,
        m_previewTargetSize,
        currentState.params,
        m_previewSequence,
        m_previewProgression,
        m_previewRenderMode,
    };
    const PreviewSubmissionResult submitted = m_previewOrchestrator->submitPreview(request);

    if (submitted.hasError())
    {
        emit previewFailed({0, currentState.photo, currentState.source.path, m_previewSequence, submitted.error()});
        return;
    }

    m_activePreviewRequestId = submitted.value();
    emit previewStarted(m_activePreviewRequestId);
}

// 목적: 현재 accepted preview request의 향후 결과 publish 취소
// 입력: 없음
// 출력: active owner request를 취소했으면 true
bool EditorOrchestrator::cancelActivePreview()
{
    if (m_activePreviewRequestId == 0)
    {
        return false;
    }

    const types::RequestId requestId = m_activePreviewRequestId;
    m_activePreviewRequestId = 0;
    const bool cancelled = m_previewOrchestrator->cancelPreview(requestId);
    if (cancelled)
    {
        emit previewCancelled(requestId);
    }
    return cancelled;
}

// 목적: preview 결과가 현재 editor selection과 revision에 일치하는지 확인
// 입력: result: PreviewOrchestrator가 전달한 결과
// 출력: adapter에 전달 가능한 현재 결과이면 true
bool EditorOrchestrator::isCurrentPreview(const PreviewResult& result) const
{
    const EditorState currentState = state();
    return result.requestId == m_activePreviewRequestId &&
           result.photo.photoId.value == currentState.photo.photoId.value &&
           result.photo.transientKey == currentState.photo.transientKey &&
           result.sourcePath == currentState.source.path &&
           result.photo.developRevision == currentState.photo.developRevision &&
           result.previewSequence == m_previewSequence;
}

// 목적: 현재 선택 PhotoId의 background source binding transition을 editor state에 반영
// 입력: update: 완료된 fingerprint request와 갱신된 catalog photo
// 출력: processing availability와 필요 시 progressive preview 갱신
void EditorOrchestrator::handleSourceBindingUpdated(const CatalogSourceUpdate& update)
{
    if (!m_currentCatalogPhotoId.has_value() || update.photoId.value != m_currentCatalogPhotoId->value ||
        m_catalogOrchestrator == nullptr || m_catalogOrchestrator->state().catalogPath != m_currentCatalogIdentity)
    {
        return;
    }

    const bool wasProcessingAllowed = m_sourceProcessingAllowed;
    m_currentSource = makeSourceDescriptor(update.photo);
    m_currentSourceState = update.photo.sourceState;
    m_sourceProcessingAllowed =
        update.photo.source.has_value() && catalog::allowsSourceProcessing(update.photo.sourceState);
    m_sourceResolution = makeSourceResolutionCapabilities(update.photo);

    if (!m_sourceProcessingAllowed)
    {
        finishEdit(false);
        m_previewDebounceTimer.stop();
        m_previewResizeDebounceTimer.stop();
        cancelActivePreview();
        ++m_previewSequence;
    }
    else if (!wasProcessingAllowed && !m_currentSource.path.isEmpty() && hasPreviewTarget(m_previewTargetSize))
    {
        scheduleFinalPreview(m_previewTargetSize, PreviewProgression::Progressive, PreviewTiming::Immediate);
    }

    publishStateChanged();
}

// 목적: Editor와 Preview의 Qt-free snapshot invalidation을 한 state transition에서 publish
// 입력: 없음
// 출력: client event adapter 알림
void EditorOrchestrator::publishStateChanged()
{
    emit editorSnapshotChanged();
    emit previewPresentationStateChanged();
}

// 목적: 현재 선택 사진의 persisted 또는 session baseline 대비 dirty 여부 계산
// 입력: 없음
// 출력: baseline과 현재 params가 다르면 true
bool EditorOrchestrator::isDirty() const
{
    const auto baseline = m_sessionBaselines.constFind(currentHistoryKey());
    return baseline != m_sessionBaselines.cend() && *baseline != m_currentParams;
}

// 목적: stable catalog identity로 editor-session history key 구성
// 입력: 없음
// 출력: 현재 catalog selection의 session history key 또는 선택이 없으면 빈 문자열
QString EditorOrchestrator::currentHistoryKey() const
{
    if (m_currentCatalogPhotoId.has_value())
    {
        return QStringLiteral("catalog:%1\nphoto:%2").arg(m_currentCatalogIdentity).arg(m_currentCatalogPhotoId->value);
    }

    return {};
}

}  // namespace flexraw::core::orchestration
