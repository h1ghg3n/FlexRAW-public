#include "mcp_source_resolution_tools.h"

#include <cstdint>
#include <utility>

#include <QByteArray>
#include <QJsonArray>
#include <QStringList>

#include "mcp_client_error_projection.h"
#include "mcp_json_projection.h"

namespace flexraw::mcp
{
namespace
{

constexpr auto SourceEventsToolName = "source_resolution_events";
constexpr auto SourceCancelToolName = "source_cancel_request";

// 목적: Source request kind를 stable MCP enum text로 투영
// 입력: kind: frontend-neutral request kind
// 출력: source lifecycle command를 나타내는 snake_case text
[[nodiscard]] QString requestKindName(core::client::SourceRequestKind kind)
{
    using core::client::SourceRequestKind;
    switch (kind)
    {
    case SourceRequestKind::EstablishBaseline:
        return QStringLiteral("establish_baseline");
    case SourceRequestKind::VerifySource:
        return QStringLiteral("verify_source");
    case SourceRequestKind::AcceptReplacement:
        return QStringLiteral("accept_replacement");
    case SourceRequestKind::RegisterReplacementAsNew:
        return QStringLiteral("register_replacement_as_new");
    case SourceRequestKind::RelinkSource:
        return QStringLiteral("relink_source");
    }
    return QStringLiteral("verify_source");
}

// 목적: Source terminal state를 stable MCP enum text로 투영
// 입력: state: frontend-neutral terminal state
// 출력: completed/failed/cancelled 중 하나
[[nodiscard]] QString terminalStateName(core::client::SourceResolutionTerminalState state)
{
    using core::client::SourceResolutionTerminalState;
    switch (state)
    {
    case SourceResolutionTerminalState::Completed:
        return QStringLiteral("completed");
    case SourceResolutionTerminalState::Failed:
        return QStringLiteral("failed");
    case SourceResolutionTerminalState::Cancelled:
        return QStringLiteral("cancelled");
    }
    return QStringLiteral("failed");
}

// 목적: Catalog file kind를 Source event JSON enum text로 투영
// 입력: kind: frontend-neutral file kind
// 출력: unknown/raw/raster_image 중 하나
[[nodiscard]] QString fileKindName(core::client::CatalogFileKind kind)
{
    using core::client::CatalogFileKind;
    switch (kind)
    {
    case CatalogFileKind::Raw:
        return QStringLiteral("raw");
    case CatalogFileKind::RasterImage:
        return QStringLiteral("raster_image");
    case CatalogFileKind::Unknown:
        return QStringLiteral("unknown");
    }
    return QStringLiteral("unknown");
}

// 목적: Catalog scan status를 Source event JSON enum text로 투영
// 입력: status: frontend-neutral scan status
// 출력: pending/ready/unsupported/failed 중 하나
[[nodiscard]] QString scanStatusName(core::client::CatalogScanStatus status)
{
    using core::client::CatalogScanStatus;
    switch (status)
    {
    case CatalogScanStatus::Ready:
        return QStringLiteral("ready");
    case CatalogScanStatus::Unsupported:
        return QStringLiteral("unsupported");
    case CatalogScanStatus::Failed:
        return QStringLiteral("failed");
    case CatalogScanStatus::Pending:
        return QStringLiteral("pending");
    }
    return QStringLiteral("pending");
}

// 목적: Catalog source state를 Source event JSON enum text로 투영
// 입력: state: frontend-neutral source binding state
// 출력: source lifecycle을 표현하는 snake_case text
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

// 목적: Source request receipt를 precision-safe MCP JSON object로 투영
// 입력: receipt: owner request identity와 source context
// 출력: decimal string identity와 normalized locator object
[[nodiscard]] QJsonObject receiptToJson(const core::client::SourceRequestReceipt& receipt)
{
    return QJsonObject{{QStringLiteral("request_id"), QString::number(receipt.id.value)},
                       {QStringLiteral("kind"), requestKindName(receipt.kind)},
                       {QStringLiteral("photo_id"), QString::number(receipt.photoId.value)},
                       {QStringLiteral("source_locator"), toJsonString(receipt.sourceLocator)}};
}

// 목적: Catalog photo snapshot을 Source update용 MCP JSON object로 투영
// 입력: photo: frontend-neutral persisted photo snapshot
// 출력: identity/source/fingerprint 상태가 보존된 JSON object
[[nodiscard]] QJsonObject photoToJson(const core::client::CatalogPhotoSnapshot& photo)
{
    QJsonObject object{{QStringLiteral("photo_id"), QString::number(photo.id.value)},
                       {QStringLiteral("display_name"), toJsonString(photo.displayName)},
                       {QStringLiteral("last_known_path"), toJsonString(photo.lastKnownPath)},
                       {QStringLiteral("extension"), toJsonString(photo.extension)},
                       {QStringLiteral("file_kind"), fileKindName(photo.kind)},
                       {QStringLiteral("scan_status"), scanStatusName(photo.scanStatus)},
                       {QStringLiteral("source_state"), sourceStateName(photo.sourceState)},
                       {QStringLiteral("size_bytes"), QString::number(photo.fingerprint.sizeBytes)},
                       {QStringLiteral("modified_at_ms"), QString::number(photo.fingerprint.modifiedAtMs)}};
    object.insert(QStringLiteral("source_path"),
                  photo.sourcePath.has_value() ? QJsonValue(toJsonString(*photo.sourcePath))
                                               : QJsonValue(QJsonValue::Null));
    const QByteArray fingerprint(reinterpret_cast<const char*>(photo.fingerprint.sha256.data()),
                                 static_cast<qsizetype>(photo.fingerprint.sha256.size()));
    object.insert(QStringLiteral("sha256"), QString::fromLatin1(fingerprint.toHex()));
    return object;
}

// 목적: active Source request snapshot을 MCP JSON array로 투영
// 입력: snapshot: current active request 목록
// 출력: owner 순서를 보존한 receipt array
[[nodiscard]] QJsonArray activeRequestsToJson(const core::client::SourceResolutionSnapshot& snapshot)
{
    QJsonArray requests;
    for (const core::client::SourceRequestReceipt& receipt : snapshot.activeRequests)
    {
        requests.append(receiptToJson(receipt));
    }
    return requests;
}

// 목적: Source Resolution event 하나를 model-safe MCP JSON으로 투영
// 입력: event: immutable snapshot과 optional lifecycle payload
// 출력: sequence와 exact terminal 의미가 보존된 object
[[nodiscard]] QJsonObject eventToJson(const core::client::SourceResolutionEvent& event)
{
    QJsonObject object{{QStringLiteral("sequence"), QString::number(event.sequence.value)},
                       {QStringLiteral("initial"), event.initial},
                       {QStringLiteral("active_requests"), activeRequestsToJson(event.snapshot)},
                       {QStringLiteral("accepted"), QJsonValue(QJsonValue::Null)},
                       {QStringLiteral("update"), QJsonValue(QJsonValue::Null)},
                       {QStringLiteral("issue"), QJsonValue(QJsonValue::Null)},
                       {QStringLiteral("terminal"), QJsonValue(QJsonValue::Null)}};
    if (event.accepted.has_value())
    {
        object.insert(QStringLiteral("accepted"), receiptToJson(*event.accepted));
    }
    if (event.update.has_value())
    {
        QJsonObject update{{QStringLiteral("receipt"), receiptToJson(event.update->receipt)},
                           {QStringLiteral("photo"), photoToJson(event.update->photo)},
                           {QStringLiteral("created_photo"), QJsonValue(QJsonValue::Null)}};
        if (event.update->createdPhoto.has_value())
        {
            update.insert(QStringLiteral("created_photo"), photoToJson(*event.update->createdPhoto));
        }
        object.insert(QStringLiteral("update"), std::move(update));
    }
    if (event.issue.has_value())
    {
        object.insert(QStringLiteral("issue"),
                      QJsonObject{{QStringLiteral("receipt"), receiptToJson(event.issue->receipt)},
                                  {QStringLiteral("error"), clientErrorToJson(event.issue->error)}});
    }
    if (event.terminal.has_value())
    {
        QJsonObject terminal{{QStringLiteral("receipt"), receiptToJson(event.terminal->receipt)},
                             {QStringLiteral("state"), terminalStateName(event.terminal->state)},
                             {QStringLiteral("error"), QJsonValue(QJsonValue::Null)}};
        if (event.terminal->error.has_value())
        {
            terminal.insert(QStringLiteral("error"), clientErrorToJson(*event.terminal->error));
        }
        object.insert(QStringLiteral("terminal"), std::move(terminal));
    }
    return object;
}

// 목적: optional event cursor를 canonical unsigned decimal string으로 parse
// 입력: value: after_sequence JSON 값, parsed: 성공 시 채울 sequence
// 출력: 0 또는 leading zero 없는 unsigned 64-bit decimal이면 true
[[nodiscard]] bool parseEventSequence(const QJsonValue& value, std::uint64_t& parsed)
{
    if (!value.isString())
    {
        return false;
    }
    const QString text = value.toString();
    if (text == QStringLiteral("0"))
    {
        parsed = 0;
        return true;
    }
    if (text.isEmpty() || text.front() < QLatin1Char('1') || text.front() > QLatin1Char('9'))
    {
        return false;
    }
    for (const QChar character : text)
    {
        if (character < QLatin1Char('0') || character > QLatin1Char('9'))
        {
            return false;
        }
    }
    bool converted = false;
    const qulonglong value64 = text.toULongLong(&converted, 10);
    if (!converted)
    {
        return false;
    }
    parsed = static_cast<std::uint64_t>(value64);
    return true;
}

// 목적: event failure의 technical message를 model output과 분리해 diagnostic으로 수집
// 입력: event: optional issue 또는 failed terminal
// 출력: diagnostic이 없으면 빈 string
[[nodiscard]] std::string eventDiagnostic(const core::client::SourceResolutionEvent& event)
{
    if (event.issue.has_value())
    {
        return event.issue->error.technicalMessage;
    }
    if (event.terminal.has_value() && event.terminal->error.has_value())
    {
        return event.terminal->error->technicalMessage;
    }
    return {};
}

}  // namespace

// 목적: Source Resolution command/event contract를 bounded MCP event window와 write policy에 연결
// 입력: sourceClient: cancel owner, eventSource: ordered lifecycle source, writesEnabled: process write opt-in
// 출력: event source lifetime 안에서 사용할 MCP adapter
McpSourceResolutionTools::McpSourceResolutionTools(core::client::ISourceResolutionClient& sourceClient,
                                                   core::client::ISourceResolutionEventSource& eventSource,
                                                   bool writesEnabled)
    : m_sourceClient(&sourceClient), m_writesEnabled(writesEnabled)
{
    const core::client::SourceResolutionSubscriptionResult subscribed = eventSource.subscribeToSourceResolution(
        [this](const core::client::SourceResolutionEvent& event) { recordEvent(event); });
    if (subscribed.hasError())
    {
        m_subscriptionError = subscribed.error();
        m_startupDiagnostic = subscribed.error().technicalMessage;
        return;
    }
    m_subscription = subscribed.value();
}

// 목적: queued callback보다 먼저 subscription을 해제하고 adapter lifetime 종료
// 입력: 없음
// 출력: destruction 이후 Source Resolution callback 없음
McpSourceResolutionTools::~McpSourceResolutionTools()
{
    shutdown();
}

// 목적: bounded Source Resolution event 조회 tool의 stable wire name 반환
// 입력: 없음
// 출력: source_resolution_events
QString McpSourceResolutionTools::eventsToolName()
{
    return QString::fromLatin1(SourceEventsToolName);
}

// 목적: Source Resolution cancellation tool의 stable wire name 반환
// 입력: 없음
// 출력: source_cancel_request
QString McpSourceResolutionTools::cancelRequestToolName()
{
    return QString::fromLatin1(SourceCancelToolName);
}

// 목적: bounded event window tool의 schema와 read-only hint 반환
// 입력: 없음
// 출력: tools/list에 포함할 descriptor
QJsonObject McpSourceResolutionTools::eventsDescriptor() const
{
    return QJsonObject{
        {QStringLiteral("name"), eventsToolName()},
        {QStringLiteral("title"), QStringLiteral("Read Flexraw Source Resolution Events")},
        {QStringLiteral("description"),
         QStringLiteral("Return an ordered bounded window of source fingerprint and resolution lifecycle events.")},
        {QStringLiteral("inputSchema"),
         QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                     {QStringLiteral("properties"),
                      QJsonObject{{QStringLiteral("after_sequence"),
                                   QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                                               {QStringLiteral("pattern"), QStringLiteral("^(0|[1-9][0-9]*)$")}}},
                                  {QStringLiteral("limit"),
                                   QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")},
                                               {QStringLiteral("minimum"), 1},
                                               {QStringLiteral("maximum"), MaximumEventLimit}}}}},
                     {QStringLiteral("additionalProperties"), false}}},
        {QStringLiteral("annotations"),
         QJsonObject{{QStringLiteral("readOnlyHint"), true},
                     {QStringLiteral("destructiveHint"), false},
                     {QStringLiteral("idempotentHint"), true},
                     {QStringLiteral("openWorldHint"), false}}}};
}

// 목적: Source Resolution cancellation tool의 schema와 mutation hint 반환
// 입력: 없음
// 출력: write-enabled tools/list에 포함할 descriptor
QJsonObject McpSourceResolutionTools::cancelRequestDescriptor() const
{
    return QJsonObject{
        {QStringLiteral("name"), cancelRequestToolName()},
        {QStringLiteral("title"), QStringLiteral("Cancel Flexraw Source Request")},
        {QStringLiteral("description"),
         QStringLiteral(
             "Cancel one currently active source fingerprint or resolution request. Writes must be enabled.")},
        {QStringLiteral("inputSchema"),
         QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                     {QStringLiteral("properties"),
                      QJsonObject{{QStringLiteral("request_id"),
                                   QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                                               {QStringLiteral("pattern"), QStringLiteral("^[1-9][0-9]*$")}}}}},
                     {QStringLiteral("required"), QJsonArray{QStringLiteral("request_id")}},
                     {QStringLiteral("additionalProperties"), false}}},
        {QStringLiteral("annotations"),
         QJsonObject{{QStringLiteral("readOnlyHint"), false},
                     {QStringLiteral("destructiveHint"), false},
                     {QStringLiteral("idempotentHint"), false},
                     {QStringLiteral("openWorldHint"), false}}}};
}

// 목적: retained Source Resolution event를 sequence 이후 bounded window로 투영
// 입력: arguments: optional after_sequence decimal string과 limit
// 출력: ordered event window 또는 validation/subscription failure
McpToolCallResult McpSourceResolutionTools::callEvents(const QJsonObject& arguments) const
{
    static const QStringList AllowedKeys{QStringLiteral("after_sequence"), QStringLiteral("limit")};
    for (auto iterator = arguments.constBegin(); iterator != arguments.constEnd(); ++iterator)
    {
        if (!AllowedKeys.contains(iterator.key()))
        {
            return {.argumentsValid = false,
                    .validationError = QStringLiteral("Unknown Source Resolution argument: %1").arg(iterator.key())};
        }
    }
    if (m_subscriptionError.has_value())
    {
        return makeClientErrorToolResult(*m_subscriptionError);
    }

    std::uint64_t afterSequence = 0;
    if (arguments.contains(QStringLiteral("after_sequence")) &&
        !parseEventSequence(arguments.value(QStringLiteral("after_sequence")), afterSequence))
    {
        return {.argumentsValid = false,
                .validationError = QStringLiteral("after_sequence must be a canonical unsigned decimal string.")};
    }
    const QJsonValue limitValue = arguments.value(QStringLiteral("limit"));
    const int limit = limitValue.isUndefined() ? DefaultEventLimit : limitValue.toInt(0);
    if ((!limitValue.isUndefined() && !limitValue.isDouble()) || limit < 1 || limit > MaximumEventLimit)
    {
        return {.argumentsValid = false,
                .validationError = QStringLiteral("limit must be an integer from 1 through 64.")};
    }

    const std::optional<std::uint64_t> oldest =
        m_events.empty() ? std::nullopt : std::optional<std::uint64_t>{m_events.front().sequence.value};
    const std::optional<std::uint64_t> latest =
        m_events.empty() ? std::nullopt : std::optional<std::uint64_t>{m_events.back().sequence.value};
    const bool gap = oldest.has_value() && *oldest > 1 && afterSequence < *oldest - 1;
    QJsonArray events;
    std::string diagnostic;
    std::uint64_t lastReturned = afterSequence;
    for (const core::client::SourceResolutionEvent& event : m_events)
    {
        if (event.sequence.value <= afterSequence)
        {
            continue;
        }
        if (events.size() >= limit)
        {
            break;
        }
        events.append(eventToJson(event));
        lastReturned = event.sequence.value;
        const std::string currentDiagnostic = eventDiagnostic(event);
        if (!currentDiagnostic.empty())
        {
            if (!diagnostic.empty())
            {
                diagnostic.push_back('\n');
            }
            diagnostic.append(currentDiagnostic);
        }
    }

    const bool hasMore = latest.has_value() && lastReturned < *latest;
    return {.argumentsValid = true,
            .structuredContent =
                QJsonObject{{QStringLiteral("oldest_available_sequence"),
                             oldest.has_value() ? QJsonValue(QString::number(*oldest)) : QJsonValue(QJsonValue::Null)},
                            {QStringLiteral("latest_sequence"),
                             latest.has_value() ? QJsonValue(QString::number(*latest)) : QJsonValue(QJsonValue::Null)},
                            {QStringLiteral("gap"), gap},
                            {QStringLiteral("has_more"), hasMore},
                            {QStringLiteral("events"), std::move(events)}},
            .diagnostic = std::move(diagnostic)};
}

// 목적: write opt-in을 확인하고 accepted Source Resolution request 취소
// 입력: arguments: positive decimal request_id
// 출력: cancelled identity 또는 validation/domain failure
McpToolCallResult McpSourceResolutionTools::callCancelRequest(const QJsonObject& arguments) const
{
    if (!m_writesEnabled)
    {
        return makeClientErrorToolResult(
            {core::client::ClientErrorCode::PermissionDenied, "MCP write access is disabled."});
    }
    if (m_subscriptionError.has_value())
    {
        return makeClientErrorToolResult(*m_subscriptionError);
    }
    if (arguments.size() != 1 || !arguments.contains(QStringLiteral("request_id")))
    {
        return {.argumentsValid = false,
                .validationError = QStringLiteral("source_cancel_request requires only request_id.")};
    }
    std::uint64_t requestId = 0;
    if (!parseEventSequence(arguments.value(QStringLiteral("request_id")), requestId) || requestId == 0)
    {
        return {.argumentsValid = false,
                .validationError = QStringLiteral("request_id must be a positive decimal string.")};
    }

    const core::client::SourceRequestCancelResult cancelled = m_sourceClient->cancelSourceRequest({requestId});
    if (cancelled.hasError())
    {
        return makeClientErrorToolResult(cancelled.error());
    }
    return {.argumentsValid = true,
            .structuredContent =
                QJsonObject{{QStringLiteral("cancelled_request_id"), QString::number(cancelled.value().value)}}};
}

// 목적: process가 명시적 cancellation tool 광고를 허용했는지 확인
// 입력: 없음
// 출력: --allow-write가 지정됐으면 true
bool McpSourceResolutionTools::writesEnabled() const noexcept
{
    return m_writesEnabled;
}

// 목적: Source Resolution event subscription 구성 성공 여부 확인
// 입력: 없음
// 출력: ordered event를 받을 준비가 됐으면 true
bool McpSourceResolutionTools::ready() const noexcept
{
    return !m_subscriptionError.has_value() && m_subscription != nullptr && m_subscription->isActive();
}

// 목적: startup subscription failure의 diagnostic-only message 조회
// 입력: 없음
// 출력: ready 상태면 빈 string, 실패하면 technical message
const std::string& McpSourceResolutionTools::startupDiagnostic() const noexcept
{
    return m_startupDiagnostic;
}

// 목적: EOF/process shutdown 전에 queued event와 이후 callback 전달 차단
// 입력: 없음
// 출력: idempotent inactive subscription 상태
void McpSourceResolutionTools::shutdown() noexcept
{
    if (m_shuttingDown)
    {
        return;
    }
    m_shuttingDown = true;
    if (m_subscription != nullptr)
    {
        m_subscription->unsubscribe();
        m_subscription.reset();
    }
}

// 목적: delivery context에서 immutable event를 bounded retained window에 추가
// 입력: event: ordered Source Resolution lifecycle event
// 출력: capacity를 넘은 oldest event가 제거된 최신 window
void McpSourceResolutionTools::recordEvent(const core::client::SourceResolutionEvent& event)
{
    if (m_shuttingDown || event.sequence.value == 0)
    {
        return;
    }
    if (!m_events.empty() && event.sequence.value <= m_events.back().sequence.value)
    {
        return;
    }
    m_events.push_back(event);
    while (m_events.size() > RetainedEventCapacity)
    {
        m_events.pop_front();
    }
}

}  // namespace flexraw::mcp
