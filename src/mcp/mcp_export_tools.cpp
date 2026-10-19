#include "mcp_export_tools.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

#include <QFileInfo>
#include <QJsonArray>
#include <QStringList>

#include "editor_client.h"
#include "mcp_client_error_projection.h"
#include "mcp_json_projection.h"

namespace flexraw::mcp
{
namespace
{

constexpr auto ExportSubmitToolName = "export_selected_photo_local";
constexpr auto ExportEventsToolName = "export_events";
constexpr auto ExportCancelToolName = "export_cancel_request";

// 목적: Export failure kind를 stable MCP enum text로 투영
// 입력: kind: frontend-neutral domain failure classification
// 출력: none/eligibility/dispatch_exhausted/execution/ambiguous 중 하나
[[nodiscard]] QString failureKindName(core::client::ExportFailureKind kind)
{
    using core::client::ExportFailureKind;
    switch (kind)
    {
    case ExportFailureKind::None:
        return QStringLiteral("none");
    case ExportFailureKind::Eligibility:
        return QStringLiteral("eligibility");
    case ExportFailureKind::DispatchExhausted:
        return QStringLiteral("dispatch_exhausted");
    case ExportFailureKind::Execution:
        return QStringLiteral("execution");
    case ExportFailureKind::Ambiguous:
        return QStringLiteral("ambiguous");
    }
    return QStringLiteral("execution");
}

// 목적: Export scheduling metric을 fixed wire field object로 투영
// 입력: metrics: request 또는 application lifetime counter
// 출력: 모든 product scheduling counter를 포함한 JSON object
[[nodiscard]] QJsonObject schedulingToJson(const core::client::ExportSchedulingMetrics& metrics)
{
    return QJsonObject{{QStringLiteral("submitted"), QString::number(metrics.submitted)},
                       {QStringLiteral("succeeded"), QString::number(metrics.succeeded)},
                       {QStringLiteral("failed"), QString::number(metrics.failed)},
                       {QStringLiteral("cancelled"), QString::number(metrics.cancelled)},
                       {QStringLiteral("ambiguous"), QString::number(metrics.ambiguous)},
                       {QStringLiteral("local_dispatch_attempts"), QString::number(metrics.localDispatchAttempts)},
                       {QStringLiteral("remote_dispatch_attempts"), QString::number(metrics.remoteDispatchAttempts)},
                       {QStringLiteral("server_busy_count"), QString::number(metrics.serverBusyCount)},
                       {QStringLiteral("resource_busy_count"), QString::number(metrics.resourceBusyCount)},
                       {QStringLiteral("connection_failed_count"), QString::number(metrics.connectionFailedCount)},
                       {QStringLiteral("local_executed"), QString::number(metrics.localExecuted)},
                       {QStringLiteral("remote_executed"), QString::number(metrics.remoteExecuted)}};
}

// 목적: active Export snapshot을 precision-safe identity와 count array로 투영
// 입력: snapshot: current active request와 lifetime scheduling state
// 출력: active request array
[[nodiscard]] QJsonArray activeRequestsToJson(const core::client::ExportSnapshot& snapshot)
{
    QJsonArray requests;
    for (const core::client::ActiveExportSnapshot& request : snapshot.activeRequests)
    {
        requests.append(QJsonObject{{QStringLiteral("request_id"), QString::number(request.requestId.value)},
                                    {QStringLiteral("completed_count"), QString::number(request.completedCount)},
                                    {QStringLiteral("total_count"), QString::number(request.totalCount)},
                                    {QStringLiteral("succeeded_count"), QString::number(request.succeededCount)},
                                    {QStringLiteral("failed_count"), QString::number(request.failedCount)}});
    }
    return requests;
}

// 목적: completed item result를 artifact locator와 model-safe failure로 투영
// 입력: item: immutable Export terminal item
// 출력: source/output, success, failure kind와 optional error object
[[nodiscard]] QJsonObject itemToJson(const core::client::ExportItemResult& item)
{
    QJsonObject object{{QStringLiteral("source_locator"), toJsonString(item.sourceLocator)},
                       {QStringLiteral("output_locator"), toJsonString(item.outputLocator)},
                       {QStringLiteral("succeeded"), item.succeeded},
                       {QStringLiteral("failure_kind"), failureKindName(item.failureKind)},
                       {QStringLiteral("error"), QJsonValue(QJsonValue::Null)}};
    if (item.error.has_value())
    {
        object.insert(QStringLiteral("error"), clientErrorToJson(*item.error));
    }
    return object;
}

// 목적: aggregate Export report를 item artifact와 fixed-width count로 투영
// 입력: report: exact completed request report
// 출력: item result와 request scheduling counter를 포함한 object
[[nodiscard]] QJsonObject reportToJson(const core::client::ExportReport& report)
{
    QJsonArray items;
    for (const core::client::ExportItemResult& item : report.items)
    {
        items.append(itemToJson(item));
    }
    return QJsonObject{{QStringLiteral("total_count"), QString::number(report.totalCount)},
                       {QStringLiteral("succeeded_count"), QString::number(report.succeededCount)},
                       {QStringLiteral("failed_count"), QString::number(report.failedCount)},
                       {QStringLiteral("items"), std::move(items)},
                       {QStringLiteral("scheduling"), schedulingToJson(report.scheduling)}};
}

// 목적: Export event 하나를 model-safe MCP JSON으로 투영
// 입력: event: immutable snapshot과 optional lifecycle payload
// 출력: sequence와 exact terminal 의미가 보존된 object
[[nodiscard]] QJsonObject eventToJson(const core::client::ExportEvent& event)
{
    QJsonObject object{{QStringLiteral("sequence"), QString::number(event.sequence.value)},
                       {QStringLiteral("initial"), event.initial},
                       {QStringLiteral("active_requests"), activeRequestsToJson(event.snapshot)},
                       {QStringLiteral("scheduling"), schedulingToJson(event.snapshot.scheduling)},
                       {QStringLiteral("accepted"), QJsonValue(QJsonValue::Null)},
                       {QStringLiteral("progress"), QJsonValue(QJsonValue::Null)},
                       {QStringLiteral("completed"), QJsonValue(QJsonValue::Null)},
                       {QStringLiteral("failed"), QJsonValue(QJsonValue::Null)},
                       {QStringLiteral("cancelled"), QJsonValue(QJsonValue::Null)}};
    if (event.accepted.has_value())
    {
        object.insert(QStringLiteral("accepted"),
                      QJsonObject{{QStringLiteral("request_id"), QString::number(event.accepted->requestId.value)}});
    }
    if (event.progress.has_value())
    {
        object.insert(QStringLiteral("progress"),
                      QJsonObject{{QStringLiteral("request_id"), QString::number(event.progress->requestId.value)},
                                  {QStringLiteral("completed_count"), QString::number(event.progress->completedCount)},
                                  {QStringLiteral("total_count"), QString::number(event.progress->totalCount)},
                                  {QStringLiteral("succeeded_count"), QString::number(event.progress->succeededCount)},
                                  {QStringLiteral("failed_count"), QString::number(event.progress->failedCount)},
                                  {QStringLiteral("current_source_locator"),
                                   toJsonString(event.progress->currentSourceLocator)}});
    }
    if (event.completed.has_value())
    {
        object.insert(QStringLiteral("completed"),
                      QJsonObject{{QStringLiteral("request_id"), QString::number(event.completed->requestId.value)},
                                  {QStringLiteral("report"), reportToJson(event.completed->report)}});
    }
    if (event.failed.has_value())
    {
        object.insert(QStringLiteral("failed"),
                      QJsonObject{{QStringLiteral("request_id"), QString::number(event.failed->requestId.value)},
                                  {QStringLiteral("error"), clientErrorToJson(event.failed->error)}});
    }
    if (event.cancelled.has_value())
    {
        object.insert(QStringLiteral("cancelled"),
                      QJsonObject{{QStringLiteral("request_id"), QString::number(event.cancelled->requestId.value)}});
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

// 목적: first-slice JPEG tool이 format과 혼동되지 않는 output locator인지 확인
// 입력: locator: user-selected output path
// 출력: 비어 있지 않고 jpg/jpeg suffix이면 true; OS별 path 의미는 downstream Platform 경계에서 판정
[[nodiscard]] bool isJpegOutputLocator(const QString& locator)
{
    if (locator.isEmpty())
    {
        return false;
    }
    const QString suffix = QFileInfo(locator).suffix().toLower();
    return suffix == QStringLiteral("jpg") || suffix == QStringLiteral("jpeg");
}

// 목적: JSON number가 손실 없이 bounded C++ integer로 표현되는지 확인
// 입력: value: optional JSON number, default/minimum/maximum: 허용 범위, parsed: 결과
// 출력: 생략되었거나 정수 범위 안이면 true
[[nodiscard]] bool parseBoundedInteger(
    const QJsonValue& value, const int defaultValue, const int minimum, const int maximum, int& parsed)
{
    if (value.isUndefined())
    {
        parsed = defaultValue;
        return true;
    }
    const double number = value.toDouble(std::numeric_limits<double>::quiet_NaN());
    if (!value.isDouble() || !std::isfinite(number) || std::floor(number) != number || number < minimum ||
        number > maximum)
    {
        return false;
    }
    parsed = static_cast<int>(number);
    return true;
}

// 목적: Export event failure의 technical message를 model output과 분리해 diagnostic으로 수집
// 입력: event: optional request failure 또는 completed item failure
// 출력: diagnostic이 없으면 빈 string
[[nodiscard]] std::string eventDiagnostic(const core::client::ExportEvent& event)
{
    std::string diagnostic;
    if (event.failed.has_value())
    {
        diagnostic = event.failed->error.technicalMessage;
    }
    if (event.completed.has_value())
    {
        for (const core::client::ExportItemResult& item : event.completed->report.items)
        {
            if (!item.error.has_value() || item.error->technicalMessage.empty())
            {
                continue;
            }
            if (!diagnostic.empty())
            {
                diagnostic.push_back('\n');
            }
            diagnostic.append(item.error->technicalMessage);
        }
    }
    return diagnostic;
}

}  // namespace

// 목적: current Editor selection과 Export lifecycle을 bounded Local JPEG MCP surface에 연결
// 입력: editorClient/exportClient/eventSource: 공유 Product Contract, writesEnabled: process write opt-in
// 출력: event source lifetime 안에서 사용할 MCP Export adapter
McpExportTools::McpExportTools(core::client::IEditorClient& editorClient,
                               core::client::IExportClient& exportClient,
                               core::client::IExportEventSource& eventSource,
                               const bool writesEnabled)
    : m_editorClient(&editorClient), m_exportClient(&exportClient), m_writesEnabled(writesEnabled)
{
    const core::client::ExportSubscriptionResult subscribed =
        eventSource.subscribeToExports([this](const core::client::ExportEvent& event) { recordEvent(event); });
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
// 출력: destruction 이후 Export callback 없음
McpExportTools::~McpExportTools()
{
    shutdown();
}

// 목적: selected-photo Local JPEG submit tool의 stable wire name 반환
// 입력: 없음
// 출력: export_selected_photo_local
QString McpExportTools::submitSelectedPhotoToolName()
{
    return QString::fromLatin1(ExportSubmitToolName);
}

// 목적: bounded Export event poll tool의 stable wire name 반환
// 입력: 없음
// 출력: export_events
QString McpExportTools::eventsToolName()
{
    return QString::fromLatin1(ExportEventsToolName);
}

// 목적: Export cancellation tool의 stable wire name 반환
// 입력: 없음
// 출력: export_cancel_request
QString McpExportTools::cancelRequestToolName()
{
    return QString::fromLatin1(ExportCancelToolName);
}

// 목적: Local JPEG artifact write tool의 schema와 mutation hint 반환
// 입력: 없음
// 출력: write-enabled tools/list에 포함할 descriptor
QJsonObject McpExportTools::submitSelectedPhotoDescriptor() const
{
    return QJsonObject{
        {QStringLiteral("name"), submitSelectedPhotoToolName()},
        {QStringLiteral("title"), QStringLiteral("Export Selected Flexraw Photo Locally")},
        {QStringLiteral("description"),
         QStringLiteral(
             "Export the current Editor selection and in-memory Develop state to one local JPEG artifact. ") +
             QStringLiteral("The output locator must end in .jpg or .jpeg and writes must be enabled.")},
        {QStringLiteral("inputSchema"),
         QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                     {QStringLiteral("properties"),
                      QJsonObject{{QStringLiteral("output_locator"),
                                   QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                                               {QStringLiteral("minLength"), 1}}},
                                  {QStringLiteral("jpeg_quality"),
                                   QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")},
                                               {QStringLiteral("minimum"), 1},
                                               {QStringLiteral("maximum"), 100}}}}},
                     {QStringLiteral("required"), QJsonArray{QStringLiteral("output_locator")}},
                     {QStringLiteral("additionalProperties"), false}}},
        {QStringLiteral("annotations"),
         QJsonObject{{QStringLiteral("readOnlyHint"), false},
                     {QStringLiteral("destructiveHint"), false},
                     {QStringLiteral("idempotentHint"), false},
                     {QStringLiteral("openWorldHint"), false}}}};
}

// 목적: bounded Export event window tool의 schema와 read-only hint 반환
// 입력: 없음
// 출력: tools/list에 포함할 descriptor
QJsonObject McpExportTools::eventsDescriptor() const
{
    return QJsonObject{
        {QStringLiteral("name"), eventsToolName()},
        {QStringLiteral("title"), QStringLiteral("Read Flexraw Export Events")},
        {QStringLiteral("description"),
         QStringLiteral("Return an ordered bounded window of Export accepted, progress and exact terminal events.")},
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

// 목적: Export cancellation tool의 schema와 mutation hint 반환
// 입력: 없음
// 출력: write-enabled tools/list에 포함할 descriptor
QJsonObject McpExportTools::cancelRequestDescriptor() const
{
    return QJsonObject{
        {QStringLiteral("name"), cancelRequestToolName()},
        {QStringLiteral("title"), QStringLiteral("Cancel Flexraw Export Request")},
        {QStringLiteral("description"),
         QStringLiteral("Cancel one currently active Export request. Writes must be enabled.")},
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

// 목적: current Editor selection과 in-memory Develop state를 Local JPEG Export로 제출
// 입력: arguments: output_locator와 optional jpeg_quality
// 출력: accepted request identity와 requested artifact path 또는 validation/domain failure
McpToolCallResult McpExportTools::callSubmitSelectedPhoto(const QJsonObject& arguments) const
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
    static const QStringList AllowedKeys{QStringLiteral("output_locator"), QStringLiteral("jpeg_quality")};
    for (auto iterator = arguments.constBegin(); iterator != arguments.constEnd(); ++iterator)
    {
        if (!AllowedKeys.contains(iterator.key()))
        {
            return {.argumentsValid = false,
                    .validationError = QStringLiteral("Unknown Export argument: %1").arg(iterator.key())};
        }
    }
    if (!arguments.value(QStringLiteral("output_locator")).isString())
    {
        return {.argumentsValid = false,
                .validationError = QStringLiteral("export_selected_photo_local requires output_locator.")};
    }
    const QString outputLocator = arguments.value(QStringLiteral("output_locator")).toString();
    if (!isJpegOutputLocator(outputLocator))
    {
        return {.argumentsValid = false,
                .validationError = QStringLiteral("output_locator must end in .jpg or .jpeg.")};
    }
    const QJsonValue qualityValue = arguments.value(QStringLiteral("jpeg_quality"));
    int quality = 90;
    if (!parseBoundedInteger(qualityValue, 90, 1, 100, quality))
    {
        return {.argumentsValid = false,
                .validationError = QStringLiteral("jpeg_quality must be an integer from 1 through 100.")};
    }

    const core::client::EditorSnapshot editor = m_editorClient->editorSnapshot();
    if (!editor.hasSelection)
    {
        return makeClientErrorToolResult(
            {core::client::ClientErrorCode::Conflict, "Local Export requires a current Editor selection."});
    }
    if (!editor.source.has_value() || editor.source->path.empty())
    {
        return makeClientErrorToolResult(
            {core::client::ClientErrorCode::NotFound, "The current Editor selection has no source locator."});
    }
    if (!editor.sourceProcessingAllowed)
    {
        return makeClientErrorToolResult(
            {core::client::ClientErrorCode::Conflict, "The current Editor source is not ready for processing."});
    }
    if (editor.source->kind == core::client::CatalogFileKind::Unknown)
    {
        return makeClientErrorToolResult(
            {core::client::ClientErrorCode::UnsupportedFormat, "The current Editor source kind is unsupported."});
    }

    core::client::ExportRasterOptions options;
    options.format = core::client::ExportRasterFormat::Jpeg;
    options.jpegQuality = quality;
    const core::client::ExportFileRequest request{
        *editor.source, fromJsonString(outputLocator), {}, editor.params, options, false};
    const core::client::ExportSubmissionResult submitted = m_exportClient->submitExport(
        {core::client::ExportRequest{request}, {core::client::ExportPlacementPolicy::LocalOnly, std::nullopt}});
    if (submitted.hasError())
    {
        return makeClientErrorToolResult(submitted.error());
    }
    return {.argumentsValid = true,
            .structuredContent =
                QJsonObject{{QStringLiteral("request_id"), QString::number(submitted.value().requestId.value)},
                            {QStringLiteral("output_locator"), outputLocator},
                            {QStringLiteral("format"), QStringLiteral("jpeg")}}};
}

// 목적: retained Export event를 sequence 이후 bounded window로 투영
// 입력: arguments: optional after_sequence decimal string과 limit
// 출력: ordered event window 또는 validation/subscription failure
McpToolCallResult McpExportTools::callEvents(const QJsonObject& arguments) const
{
    static const QStringList AllowedKeys{QStringLiteral("after_sequence"), QStringLiteral("limit")};
    for (auto iterator = arguments.constBegin(); iterator != arguments.constEnd(); ++iterator)
    {
        if (!AllowedKeys.contains(iterator.key()))
        {
            return {.argumentsValid = false,
                    .validationError = QStringLiteral("Unknown Export event argument: %1").arg(iterator.key())};
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
    int limit = DefaultEventLimit;
    if (!parseBoundedInteger(limitValue, DefaultEventLimit, 1, MaximumEventLimit, limit))
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
    for (const core::client::ExportEvent& event : m_events)
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

// 목적: write opt-in을 확인하고 accepted Export request 취소
// 입력: arguments: positive decimal request_id
// 출력: cancelled identity 또는 validation/domain failure
McpToolCallResult McpExportTools::callCancelRequest(const QJsonObject& arguments) const
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
                .validationError = QStringLiteral("export_cancel_request requires only request_id.")};
    }
    std::uint64_t requestId = 0;
    if (!parseEventSequence(arguments.value(QStringLiteral("request_id")), requestId) || requestId == 0)
    {
        return {.argumentsValid = false,
                .validationError = QStringLiteral("request_id must be a positive decimal string.")};
    }

    const core::client::ExportCancellationResult cancelled = m_exportClient->cancelExport({requestId});
    if (cancelled.hasError())
    {
        return makeClientErrorToolResult(cancelled.error());
    }
    return {.argumentsValid = true,
            .structuredContent =
                QJsonObject{{QStringLiteral("cancelled_request_id"), QString::number(cancelled.value().value)}}};
}

// 목적: process가 Export artifact write와 cancellation tool 광고를 허용했는지 확인
// 입력: 없음
// 출력: --allow-write가 지정됐으면 true
bool McpExportTools::writesEnabled() const noexcept
{
    return m_writesEnabled;
}

// 목적: Export event subscription 구성 성공 여부 확인
// 입력: 없음
// 출력: ordered event를 받을 준비가 됐으면 true
bool McpExportTools::ready() const noexcept
{
    return !m_subscriptionError.has_value() && m_subscription != nullptr && m_subscription->isActive();
}

// 목적: startup subscription failure의 diagnostic-only message 조회
// 입력: 없음
// 출력: ready 상태면 빈 string, 실패하면 technical message
const std::string& McpExportTools::startupDiagnostic() const noexcept
{
    return m_startupDiagnostic;
}

// 목적: EOF/process shutdown 전에 queued event와 이후 callback 전달 차단
// 입력: 없음
// 출력: idempotent inactive subscription 상태
void McpExportTools::shutdown() noexcept
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
// 입력: event: ordered Export lifecycle event
// 출력: capacity를 넘은 oldest event가 제거된 최신 window
void McpExportTools::recordEvent(const core::client::ExportEvent& event)
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
