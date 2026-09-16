#include "editor_client_projection.h"

#include <cstddef>
#include <string>

#include <QByteArray>
#include <QString>

namespace flexraw::core::orchestration
{
namespace
{

// 목적: QString을 byte 길이가 보존된 UTF-8 client string으로 변환
// 입력: value: Qt 내부 문자열
// 출력: Qt-free UTF-8 string
[[nodiscard]] std::string toClientString(const QString& value)
{
    const QByteArray utf8 = value.toUtf8();
    return {utf8.constData(), static_cast<std::size_t>(utf8.size())};
}

// 목적: Qt-free UTF-8 client string을 QString으로 변환
// 입력: value: byte 길이를 가진 UTF-8 string
// 출력: transitional Qt adapter 문자열
[[nodiscard]] QString fromClientString(const std::string& value)
{
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

// 목적: Core file kind를 Editor source client enum으로 변환
// 입력: kind: source descriptor의 file 종류
// 출력: 같은 의미의 Qt-free enum
[[nodiscard]] client::CatalogFileKind toClientFileKind(types::SupportedFileKind kind) noexcept
{
    switch (kind)
    {
    case types::SupportedFileKind::Unknown:
        return client::CatalogFileKind::Unknown;
    case types::SupportedFileKind::Raw:
        return client::CatalogFileKind::Raw;
    case types::SupportedFileKind::RasterImage:
        return client::CatalogFileKind::RasterImage;
    }
    return client::CatalogFileKind::Unknown;
}

// 목적: Editor source client file kind를 Core enum으로 복원
// 입력: kind: Qt-free source file 종류
// 출력: 같은 의미의 Core enum
[[nodiscard]] types::SupportedFileKind fromClientFileKind(client::CatalogFileKind kind) noexcept
{
    switch (kind)
    {
    case client::CatalogFileKind::Unknown:
        return types::SupportedFileKind::Unknown;
    case client::CatalogFileKind::Raw:
        return types::SupportedFileKind::Raw;
    case client::CatalogFileKind::RasterImage:
        return types::SupportedFileKind::RasterImage;
    }
    return types::SupportedFileKind::Unknown;
}

// 목적: Catalog source binding state를 Qt-free Editor enum으로 변환
// 입력: state: 현재 persisted source 상태
// 출력: 같은 의미의 client enum
[[nodiscard]] client::CatalogSourceState toClientSourceState(catalog::SourceBindingState state) noexcept
{
    switch (state)
    {
    case catalog::SourceBindingState::FingerprintPending:
        return client::CatalogSourceState::FingerprintPending;
    case catalog::SourceBindingState::Available:
        return client::CatalogSourceState::Available;
    case catalog::SourceBindingState::Missing:
        return client::CatalogSourceState::Missing;
    case catalog::SourceBindingState::VerificationRequired:
        return client::CatalogSourceState::VerificationRequired;
    case catalog::SourceBindingState::IdentityUnverified:
        return client::CatalogSourceState::IdentityUnverified;
    case catalog::SourceBindingState::ReplacementDetected:
        return client::CatalogSourceState::ReplacementDetected;
    case catalog::SourceBindingState::Unreadable:
        return client::CatalogSourceState::Unreadable;
    case catalog::SourceBindingState::Unlinked:
        return client::CatalogSourceState::Unlinked;
    }
    return client::CatalogSourceState::FingerprintPending;
}

// 목적: Qt-free Editor source state를 Catalog domain enum으로 복원
// 입력: state: client snapshot의 source 상태
// 출력: 같은 의미의 Catalog enum
[[nodiscard]] catalog::SourceBindingState fromClientSourceState(client::CatalogSourceState state) noexcept
{
    switch (state)
    {
    case client::CatalogSourceState::FingerprintPending:
        return catalog::SourceBindingState::FingerprintPending;
    case client::CatalogSourceState::Available:
        return catalog::SourceBindingState::Available;
    case client::CatalogSourceState::Missing:
        return catalog::SourceBindingState::Missing;
    case client::CatalogSourceState::VerificationRequired:
        return catalog::SourceBindingState::VerificationRequired;
    case client::CatalogSourceState::IdentityUnverified:
        return catalog::SourceBindingState::IdentityUnverified;
    case client::CatalogSourceState::ReplacementDetected:
        return catalog::SourceBindingState::ReplacementDetected;
    case client::CatalogSourceState::Unreadable:
        return catalog::SourceBindingState::Unreadable;
    case client::CatalogSourceState::Unlinked:
        return catalog::SourceBindingState::Unlinked;
    }
    return catalog::SourceBindingState::FingerprintPending;
}

}  // namespace

// 목적: Core Develop parameter를 Qt-free Editor client 값으로 투영
// 입력: params: processing과 Editor session이 사용하는 현재 parameter
// 출력: 같은 필드 의미를 가진 Qt-free parameter 값
client::EditorDevelopParams toClientDevelopParams(const types::DevelopParams& params) noexcept
{
    client::EditorDevelopParams snapshot;
    snapshot.exposureEv = params.exposureEv;
    snapshot.contrast = params.contrast;
    snapshot.highlights = params.highlights;
    snapshot.shadows = params.shadows;
    snapshot.whites = params.whites;
    snapshot.blacks = params.blacks;
    snapshot.saturation = params.saturation;
    snapshot.vibrance = params.vibrance;
    snapshot.whiteBalanceMode = params.whiteBalanceMode == types::WhiteBalanceMode::Custom
                                    ? client::EditorWhiteBalanceMode::Custom
                                    : client::EditorWhiteBalanceMode::AsShot;
    snapshot.whiteBalanceTemperatureKelvin = params.whiteBalanceTemperatureKelvin;
    snapshot.whiteBalanceTint = params.whiteBalanceTint;
    snapshot.clarity = params.clarity;
    snapshot.dehaze = params.dehaze;
    snapshot.sharpeningAmount = params.sharpeningAmount;
    snapshot.sharpeningRadius = params.sharpeningRadius;
    snapshot.sharpeningDetail = params.sharpeningDetail;
    snapshot.sharpeningMasking = params.sharpeningMasking;
    snapshot.luminanceNoiseReduction = params.luminanceNoiseReduction;
    snapshot.colorNoiseReduction = params.colorNoiseReduction;
    snapshot.toneCurveShadows = params.toneCurveShadows;
    snapshot.toneCurveDarks = params.toneCurveDarks;
    snapshot.toneCurveLights = params.toneCurveLights;
    snapshot.toneCurveHighlights = params.toneCurveHighlights;
    snapshot.pointCurveBlack = params.pointCurveBlack;
    snapshot.pointCurveShadows = params.pointCurveShadows;
    snapshot.pointCurveMidtones = params.pointCurveMidtones;
    snapshot.pointCurveHighlights = params.pointCurveHighlights;
    snapshot.pointCurveWhite = params.pointCurveWhite;
    return snapshot;
}

// 목적: Qt-free Editor client parameter를 Core Develop 값으로 복원
// 입력: params: client command가 전달한 parameter
// 출력: Core validation에 전달할 Develop parameter 값
types::DevelopParams fromClientDevelopParams(const client::EditorDevelopParams& params) noexcept
{
    types::DevelopParams value;
    value.exposureEv = params.exposureEv;
    value.contrast = params.contrast;
    value.highlights = params.highlights;
    value.shadows = params.shadows;
    value.whites = params.whites;
    value.blacks = params.blacks;
    value.saturation = params.saturation;
    value.vibrance = params.vibrance;
    value.whiteBalanceMode = params.whiteBalanceMode == client::EditorWhiteBalanceMode::Custom
                                 ? types::WhiteBalanceMode::Custom
                                 : types::WhiteBalanceMode::AsShot;
    value.whiteBalanceTemperatureKelvin = params.whiteBalanceTemperatureKelvin;
    value.whiteBalanceTint = params.whiteBalanceTint;
    value.clarity = params.clarity;
    value.dehaze = params.dehaze;
    value.sharpeningAmount = params.sharpeningAmount;
    value.sharpeningRadius = params.sharpeningRadius;
    value.sharpeningDetail = params.sharpeningDetail;
    value.sharpeningMasking = params.sharpeningMasking;
    value.luminanceNoiseReduction = params.luminanceNoiseReduction;
    value.colorNoiseReduction = params.colorNoiseReduction;
    value.toneCurveShadows = params.toneCurveShadows;
    value.toneCurveDarks = params.toneCurveDarks;
    value.toneCurveLights = params.toneCurveLights;
    value.toneCurveHighlights = params.toneCurveHighlights;
    value.pointCurveBlack = params.pointCurveBlack;
    value.pointCurveShadows = params.pointCurveShadows;
    value.pointCurveMidtones = params.pointCurveMidtones;
    value.pointCurveHighlights = params.pointCurveHighlights;
    value.pointCurveWhite = params.pointCurveWhite;
    return value;
}

// 목적: transitional Editor state를 Qt-free immutable snapshot으로 투영
// 입력: state: 현재 Orchestration state, adjustmentActive: 연속 조작 진행 여부
// 출력: identity, revision, source와 history 의미를 보존한 client snapshot
client::EditorSnapshot toClientEditorSnapshot(const EditorState& state, bool adjustmentActive)
{
    client::EditorSnapshot snapshot;
    snapshot.hasSelection = state.hasSelection;
    snapshot.photoId = {state.photo.photoId.value};
    snapshot.developRevision = {state.photo.developRevision};
    snapshot.persistedRevision = {state.persistedRevision};
    if (!state.source.path.isEmpty())
    {
        snapshot.source = client::EditorSourceSnapshot{toClientString(state.source.path),
                                                       toClientString(state.source.extension),
                                                       toClientString(state.source.displayName),
                                                       toClientFileKind(state.source.kind)};
    }
    snapshot.params = toClientDevelopParams(state.params);
    snapshot.sourceProcessingAllowed = state.sourceProcessingAllowed;
    if (state.sourceState.has_value())
    {
        snapshot.sourceState = toClientSourceState(*state.sourceState);
    }
    snapshot.sourceResolution = {state.sourceResolution.canAcceptReplacement,
                                 state.sourceResolution.canRegisterReplacementAsNew,
                                 state.sourceResolution.canRelinkSource};
    snapshot.dirty = state.dirty;
    snapshot.adjustmentActive = adjustmentActive;
    snapshot.canUndo = state.canUndo;
    snapshot.canRedo = state.canRedo;
    return snapshot;
}

// 목적: Qt-free Editor snapshot을 transitional Qt adapter state로 복원
// 입력: snapshot: client interface가 반환한 immutable state
// 출력: 기존 Qt GUI consumer가 사용할 EditorState 값
EditorState fromClientEditorSnapshot(const client::EditorSnapshot& snapshot)
{
    EditorState state;
    state.hasSelection = snapshot.hasSelection;
    state.photo.photoId = types::PhotoId{snapshot.photoId.value};
    state.photo.developRevision = snapshot.developRevision.value;
    state.persistedRevision = snapshot.persistedRevision.value;
    if (snapshot.source.has_value())
    {
        state.source = {fromClientString(snapshot.source->path),
                        fromClientString(snapshot.source->extension),
                        fromClientString(snapshot.source->displayName),
                        fromClientFileKind(snapshot.source->kind)};
    }
    state.params = fromClientDevelopParams(snapshot.params);
    state.sourceProcessingAllowed = snapshot.sourceProcessingAllowed;
    if (snapshot.sourceState.has_value())
    {
        state.sourceState = fromClientSourceState(*snapshot.sourceState);
    }
    state.sourceResolution = {snapshot.sourceResolution.canAcceptReplacement,
                              snapshot.sourceResolution.canRegisterReplacementAsNew,
                              snapshot.sourceResolution.canRelinkSource};
    state.dirty = snapshot.dirty;
    state.canUndo = snapshot.canUndo;
    state.canRedo = snapshot.canRedo;
    return state;
}

}  // namespace flexraw::core::orchestration
