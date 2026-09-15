#include "mcp_editor_tools.h"

#include <cmath>
#include <cstdint>

#include <QJsonArray>
#include <QJsonValue>
#include <QStringList>

#include "mcp_client_error_projection.h"
#include "mcp_json_projection.h"

namespace flexraw::mcp
{
namespace
{

constexpr auto EditorGetStateToolName = "editor_get_state";
constexpr auto EditorSelectPhotoToolName = "editor_select_photo";
constexpr auto EditorSetExposureToolName = "editor_set_exposure";

// 목적: Editor source file kind를 stable MCP text로 변환
// 입력: kind: frontend-neutral source kind
// 출력: unknown/raw/raster_image 중 하나
[[nodiscard]] QString fileKindName(core::client::CatalogFileKind kind)
{
    switch (kind)
    {
    case core::client::CatalogFileKind::Raw:
        return QStringLiteral("raw");
    case core::client::CatalogFileKind::RasterImage:
        return QStringLiteral("raster_image");
    case core::client::CatalogFileKind::Unknown:
        return QStringLiteral("unknown");
    }
    return QStringLiteral("unknown");
}

// 목적: Catalog source state를 stable MCP text로 변환
// 입력: state: frontend-neutral source binding state
// 출력: snake_case source state
[[nodiscard]] QString sourceStateName(core::client::CatalogSourceState state)
{
    using core::client::CatalogSourceState;
    switch (state)
    {
    case CatalogSourceState::Available:
        return QStringLiteral("available");
    case CatalogSourceState::Missing:
        return QStringLiteral("missing");
    case CatalogSourceState::VerificationRequired:
        return QStringLiteral("verification_required");
    case CatalogSourceState::IdentityUnverified:
        return QStringLiteral("identity_unverified");
    case CatalogSourceState::ReplacementDetected:
        return QStringLiteral("replacement_detected");
    case CatalogSourceState::Unreadable:
        return QStringLiteral("unreadable");
    case CatalogSourceState::Unlinked:
        return QStringLiteral("unlinked");
    case CatalogSourceState::FingerprintPending:
        return QStringLiteral("fingerprint_pending");
    }
    return QStringLiteral("fingerprint_pending");
}

// 목적: Editor Develop parameter 전체를 model-visible JSON object로 투영
// 입력: params: frontend-neutral immutable parameter snapshot
// 출력: 현재 parameter field와 white balance mode를 가진 JSON object
[[nodiscard]] QJsonObject developParamsToJson(const core::client::EditorDevelopParams& params)
{
    return QJsonObject{
        {QStringLiteral("exposure_ev"), params.exposureEv},
        {QStringLiteral("contrast"), params.contrast},
        {QStringLiteral("highlights"), params.highlights},
        {QStringLiteral("shadows"), params.shadows},
        {QStringLiteral("whites"), params.whites},
        {QStringLiteral("blacks"), params.blacks},
        {QStringLiteral("saturation"), params.saturation},
        {QStringLiteral("vibrance"), params.vibrance},
        {QStringLiteral("white_balance_mode"),
         params.whiteBalanceMode == core::client::EditorWhiteBalanceMode::Custom ? QStringLiteral("custom")
                                                                                 : QStringLiteral("as_shot")},
        {QStringLiteral("white_balance_temperature_kelvin"), params.whiteBalanceTemperatureKelvin},
        {QStringLiteral("white_balance_tint"), params.whiteBalanceTint},
        {QStringLiteral("clarity"), params.clarity},
        {QStringLiteral("dehaze"), params.dehaze},
        {QStringLiteral("sharpening_amount"), params.sharpeningAmount},
        {QStringLiteral("sharpening_radius"), params.sharpeningRadius},
        {QStringLiteral("sharpening_detail"), params.sharpeningDetail},
        {QStringLiteral("sharpening_masking"), params.sharpeningMasking},
        {QStringLiteral("luminance_noise_reduction"), params.luminanceNoiseReduction},
        {QStringLiteral("color_noise_reduction"), params.colorNoiseReduction},
        {QStringLiteral("tone_curve_shadows"), params.toneCurveShadows},
        {QStringLiteral("tone_curve_darks"), params.toneCurveDarks},
        {QStringLiteral("tone_curve_lights"), params.toneCurveLights},
        {QStringLiteral("tone_curve_highlights"), params.toneCurveHighlights},
        {QStringLiteral("point_curve_black"), params.pointCurveBlack},
        {QStringLiteral("point_curve_shadows"), params.pointCurveShadows},
        {QStringLiteral("point_curve_midtones"), params.pointCurveMidtones},
        {QStringLiteral("point_curve_highlights"), params.pointCurveHighlights},
        {QStringLiteral("point_curve_white"), params.pointCurveWhite},
    };
}

// 목적: Editor immutable snapshot을 identity/revision-safe MCP JSON으로 투영
// 입력: snapshot: shared IEditorClient가 반환한 현재 state
// 출력: selection, Develop, source와 history capability object
[[nodiscard]] QJsonObject editorSnapshotToJson(const core::client::EditorSnapshot& snapshot)
{
    QJsonObject object{
        {QStringLiteral("has_selection"), snapshot.hasSelection},
        {QStringLiteral("photo_id"),
         snapshot.hasSelection ? QJsonValue(QString::number(snapshot.photoId.value)) : QJsonValue(QJsonValue::Null)},
        {QStringLiteral("develop_revision"), QString::number(snapshot.developRevision.value)},
        {QStringLiteral("persisted_revision"), QString::number(snapshot.persistedRevision.value)},
        {QStringLiteral("develop"), developParamsToJson(snapshot.params)},
        {QStringLiteral("source_processing_allowed"), snapshot.sourceProcessingAllowed},
        {QStringLiteral("dirty"), snapshot.dirty},
        {QStringLiteral("adjustment_active"), snapshot.adjustmentActive},
        {QStringLiteral("can_undo"), snapshot.canUndo},
        {QStringLiteral("can_redo"), snapshot.canRedo},
        {QStringLiteral("source_resolution"),
         QJsonObject{
             {QStringLiteral("can_accept_replacement"), snapshot.sourceResolution.canAcceptReplacement},
             {QStringLiteral("can_register_replacement_as_new"), snapshot.sourceResolution.canRegisterReplacementAsNew},
             {QStringLiteral("can_relink_source"), snapshot.sourceResolution.canRelinkSource}}},
    };
    if (snapshot.source.has_value())
    {
        object.insert(QStringLiteral("source"),
                      QJsonObject{{QStringLiteral("path"), toJsonString(snapshot.source->path)},
                                  {QStringLiteral("extension"), toJsonString(snapshot.source->extension)},
                                  {QStringLiteral("display_name"), toJsonString(snapshot.source->displayName)},
                                  {QStringLiteral("file_kind"), fileKindName(snapshot.source->kind)}});
    }
    else
    {
        object.insert(QStringLiteral("source"), QJsonValue(QJsonValue::Null));
    }
    object.insert(QStringLiteral("source_state"),
                  snapshot.sourceState.has_value() ? QJsonValue(sourceStateName(*snapshot.sourceState))
                                                   : QJsonValue(QJsonValue::Null));
    return object;
}

// 목적: Editor success snapshot을 공통 structured tool result로 포장
// 입력: snapshot: command 또는 state query 결과
// 출력: argumentsValid=true인 editor object result
[[nodiscard]] McpToolCallResult makeEditorSnapshotResult(const core::client::EditorSnapshot& snapshot)
{
    return {.argumentsValid = true,
            .structuredContent = QJsonObject{{QStringLiteral("editor"), editorSnapshotToJson(snapshot)}}};
}

// 목적: exact allowed key만 포함하는지 검증
// 입력: arguments: tool input, allowedKeys: 허용된 property 목록, validationError: 실패 설명
// 출력: 모든 key가 허용되면 true
[[nodiscard]] bool hasOnlyAllowedKeys(const QJsonObject& arguments,
                                      const QStringList& allowedKeys,
                                      QString& validationError)
{
    for (auto iterator = arguments.constBegin(); iterator != arguments.constEnd(); ++iterator)
    {
        if (!allowedKeys.contains(iterator.key()))
        {
            validationError = QStringLiteral("Unknown Editor argument: %1").arg(iterator.key());
            return false;
        }
    }
    return true;
}

}  // namespace

// 목적: MCP Editor projection을 shared frontend-neutral client에 연결
// 입력: editorClient: process-scoped Editor consumer, writesEnabled: Editor command opt-in
// 출력: client lifetime 동안 사용할 Editor MCP adapter
McpEditorTools::McpEditorTools(core::client::IEditorClient& editorClient, bool writesEnabled) noexcept
    : m_editorClient(&editorClient), m_writesEnabled(writesEnabled)
{}

// 목적: Editor snapshot tool의 stable wire name 반환
// 입력: 없음
// 출력: editor_get_state
QString McpEditorTools::getStateToolName()
{
    return QString::fromLatin1(EditorGetStateToolName);
}

// 목적: Photo selection tool의 stable wire name 반환
// 입력: 없음
// 출력: editor_select_photo
QString McpEditorTools::selectPhotoToolName()
{
    return QString::fromLatin1(EditorSelectPhotoToolName);
}

// 목적: absolute exposure update tool의 stable wire name 반환
// 입력: 없음
// 출력: editor_set_exposure
QString McpEditorTools::setExposureToolName()
{
    return QString::fromLatin1(EditorSetExposureToolName);
}

// 목적: immutable Editor snapshot tool schema 반환
// 입력: 없음
// 출력: tools/list descriptor
QJsonObject McpEditorTools::getStateDescriptor() const
{
    return QJsonObject{
        {QStringLiteral("name"), getStateToolName()},
        {QStringLiteral("title"), QStringLiteral("Get Flexraw Editor State")},
        {QStringLiteral("description"), QStringLiteral("Return the current immutable Editor session snapshot.")},
        {QStringLiteral("inputSchema"),
         QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                     {QStringLiteral("additionalProperties"), false}}},
        {QStringLiteral("annotations"),
         QJsonObject{{QStringLiteral("readOnlyHint"), true},
                     {QStringLiteral("destructiveHint"), false},
                     {QStringLiteral("idempotentHint"), true},
                     {QStringLiteral("openWorldHint"), false}}}};
}

// 목적: stable Photo selection command schema 반환
// 입력: 없음
// 출력: tools/list descriptor
QJsonObject McpEditorTools::selectPhotoDescriptor() const
{
    return QJsonObject{
        {QStringLiteral("name"), selectPhotoToolName()},
        {QStringLiteral("title"), QStringLiteral("Select Flexraw Editor Photo")},
        {QStringLiteral("description"),
         QStringLiteral("Select an existing Catalog photo in the process-scoped Editor session.")},
        {QStringLiteral("inputSchema"),
         QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                     {QStringLiteral("properties"),
                      QJsonObject{{QStringLiteral("photo_id"),
                                   QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                                               {QStringLiteral("pattern"), QStringLiteral("^[1-9][0-9]*$")}}}}},
                     {QStringLiteral("required"), QJsonArray{QStringLiteral("photo_id")}},
                     {QStringLiteral("additionalProperties"), false}}},
        {QStringLiteral("annotations"),
         QJsonObject{{QStringLiteral("readOnlyHint"), false},
                     {QStringLiteral("destructiveHint"), false},
                     {QStringLiteral("idempotentHint"), true},
                     {QStringLiteral("openWorldHint"), false}}}};
}

// 목적: session-only absolute exposure command schema 반환
// 입력: 없음
// 출력: tools/list descriptor
QJsonObject McpEditorTools::setExposureDescriptor() const
{
    return QJsonObject{
        {QStringLiteral("name"), setExposureToolName()},
        {QStringLiteral("title"), QStringLiteral("Set Flexraw Editor Exposure")},
        {QStringLiteral("description"),
         QStringLiteral(
             "Set absolute exposure in the process-scoped Editor session without saving it to the Catalog.")},
        {QStringLiteral("inputSchema"),
         QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                     {QStringLiteral("properties"),
                      QJsonObject{{QStringLiteral("exposure_ev"),
                                   QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}}}},
                     {QStringLiteral("required"), QJsonArray{QStringLiteral("exposure_ev")}},
                     {QStringLiteral("additionalProperties"), false}}},
        {QStringLiteral("annotations"),
         QJsonObject{{QStringLiteral("readOnlyHint"), false},
                     {QStringLiteral("destructiveHint"), false},
                     {QStringLiteral("idempotentHint"), true},
                     {QStringLiteral("openWorldHint"), false}}}};
}

// 목적: 현재 Editor immutable snapshot을 JSON으로 투영
// 입력: arguments: 비어 있어야 하는 tool arguments
// 출력: protocol validation 또는 structured snapshot
McpToolCallResult McpEditorTools::callGetState(const QJsonObject& arguments) const
{
    if (!arguments.isEmpty())
    {
        return {.argumentsValid = false, .validationError = QStringLiteral("editor_get_state takes no arguments.")};
    }
    return makeEditorSnapshotResult(m_editorClient->editorSnapshot());
}

// 목적: stable PhotoId를 기존 EditorOrchestrator session에 선택
// 입력: arguments: decimal string photo_id
// 출력: protocol validation 또는 structured snapshot/domain failure
McpToolCallResult McpEditorTools::callSelectPhoto(const QJsonObject& arguments) const
{
    if (!m_writesEnabled)
    {
        return makeClientErrorToolResult(
            {core::client::ClientErrorCode::PermissionDenied, "MCP Editor commands are disabled."});
    }
    QString validationError;
    if (!hasOnlyAllowedKeys(arguments, {QStringLiteral("photo_id")}, validationError))
    {
        return {.argumentsValid = false, .validationError = validationError};
    }
    std::int64_t photoId = 0;
    if (!parsePositiveIdentity(arguments.value(QStringLiteral("photo_id")), photoId))
    {
        return {.argumentsValid = false,
                .validationError = QStringLiteral("photo_id must be a positive decimal string.")};
    }
    const core::client::EditorResult selected = m_editorClient->selectPhoto({{photoId}});
    return selected.hasError() ? makeClientErrorToolResult(selected.error())
                               : makeEditorSnapshotResult(selected.value());
}

// 목적: 현재 snapshot의 나머지 Develop 값을 보존하며 absolute exposure 갱신
// 입력: arguments: finite JSON number exposure_ev
// 출력: protocol validation 또는 structured snapshot/domain failure
McpToolCallResult McpEditorTools::callSetExposure(const QJsonObject& arguments) const
{
    if (!m_writesEnabled)
    {
        return makeClientErrorToolResult(
            {core::client::ClientErrorCode::PermissionDenied, "MCP Editor commands are disabled."});
    }
    QString validationError;
    if (!hasOnlyAllowedKeys(arguments, {QStringLiteral("exposure_ev")}, validationError))
    {
        return {.argumentsValid = false, .validationError = validationError};
    }
    const QJsonValue exposureValue = arguments.value(QStringLiteral("exposure_ev"));
    if (!exposureValue.isDouble() || !std::isfinite(exposureValue.toDouble()))
    {
        return {.argumentsValid = false,
                .validationError = QStringLiteral("exposure_ev must be a finite JSON number.")};
    }

    core::client::EditorDevelopParams params = m_editorClient->editorSnapshot().params;
    params.exposureEv = static_cast<float>(exposureValue.toDouble());
    const core::client::EditorResult updated = m_editorClient->updateDevelopParams({params});
    return updated.hasError() ? makeClientErrorToolResult(updated.error()) : makeEditorSnapshotResult(updated.value());
}

// 목적: process가 source observation 가능한 Editor command 광고를 허용했는지 확인
// 입력: 없음
// 출력: --allow-write가 지정됐으면 true
bool McpEditorTools::writesEnabled() const noexcept
{
    return m_writesEnabled;
}

}  // namespace flexraw::mcp
