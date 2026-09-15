#include "mcp_catalog_tools.h"

#include <cmath>
#include <cstdint>
#include <limits>
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

constexpr auto CatalogQueryToolName = "catalog_query_photos";

// 목적: Catalog file kind를 MCP JSON enum text로 투영
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

// 목적: Catalog scan status를 MCP JSON enum text로 투영
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

// 목적: Catalog source state를 MCP JSON enum text로 투영
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

// 목적: Catalog page cursor를 JSON precision-safe object로 투영
// 입력: cursor: display order key와 optional scope를 포함한 cursor
// 출력: 다음 tools/call에서 그대로 사용할 수 있는 JSON object
[[nodiscard]] QJsonObject cursorToJson(const core::client::CatalogPhotoPageCursor& cursor)
{
    QJsonObject object{{QStringLiteral("display_name"), toJsonString(cursor.displayName)},
                       {QStringLiteral("photo_id"), QString::number(cursor.photoId.value)}};
    if (cursor.exactFolderPath.has_value())
    {
        object.insert(QStringLiteral("exact_folder_path"), toJsonString(*cursor.exactFolderPath));
    }
    if (cursor.projectId.has_value())
    {
        object.insert(QStringLiteral("project_id"), QString::number(cursor.projectId->value));
    }
    return object;
}

// 목적: Catalog photo snapshot을 MCP structured content object로 투영
// 입력: photo: frontend-neutral persisted photo snapshot
// 출력: identity 정밀도와 optional source 의미가 보존된 JSON object
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

// 목적: bounded Catalog page를 photos와 optional cursors JSON object로 투영
// 입력: page: frontend-neutral query result
// 출력: structured MCP tool content
[[nodiscard]] QJsonObject pageToJson(const core::client::CatalogPhotoPage& page)
{
    QJsonArray photos;
    for (const core::client::CatalogPhotoSnapshot& photo : page.photos)
    {
        photos.append(photoToJson(photo));
    }
    QJsonObject result{{QStringLiteral("photos"), photos},
                       {QStringLiteral("previous_cursor"), QJsonValue(QJsonValue::Null)},
                       {QStringLiteral("next_cursor"), QJsonValue(QJsonValue::Null)}};
    if (page.previousCursor.has_value())
    {
        result.insert(QStringLiteral("previous_cursor"), cursorToJson(*page.previousCursor));
    }
    if (page.nextCursor.has_value())
    {
        result.insert(QStringLiteral("next_cursor"), cursorToJson(*page.nextCursor));
    }
    return result;
}

}  // namespace

McpCatalogTools::McpCatalogTools(core::client::ICatalogPhotoClient& catalogPhotoClient) noexcept
    : m_catalogPhotoClient(&catalogPhotoClient)
{}

QString McpCatalogTools::queryPhotosToolName()
{
    return QString::fromLatin1(CatalogQueryToolName);
}

QJsonObject McpCatalogTools::queryPhotosDescriptor() const
{
    const QJsonObject cursorSchema{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("properties"),
         QJsonObject{
             {QStringLiteral("display_name"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
             {QStringLiteral("photo_id"),
              QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                          {QStringLiteral("pattern"), QStringLiteral("^[1-9][0-9]*$")}}},
             {QStringLiteral("exact_folder_path"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
             {QStringLiteral("project_id"),
              QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                          {QStringLiteral("pattern"), QStringLiteral("^[1-9][0-9]*$")}}}}},
        {QStringLiteral("required"), QJsonArray{QStringLiteral("display_name"), QStringLiteral("photo_id")}},
        {QStringLiteral("additionalProperties"), false}};
    const QJsonObject inputSchema{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("properties"),
         QJsonObject{
             {QStringLiteral("page_size"),
              QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")},
                          {QStringLiteral("minimum"), 1},
                          {QStringLiteral("maximum"), core::client::MaximumCatalogPhotoPageSize},
                          {QStringLiteral("default"), core::client::DefaultCatalogPhotoPageSize}}},
             {QStringLiteral("direction"),
              QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                          {QStringLiteral("enum"), QJsonArray{QStringLiteral("forward"), QStringLiteral("backward")}},
                          {QStringLiteral("default"), QStringLiteral("forward")}}},
             {QStringLiteral("cursor"), cursorSchema},
             {QStringLiteral("exact_folder_path"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
             {QStringLiteral("project_id"),
              QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                          {QStringLiteral("pattern"), QStringLiteral("^[1-9][0-9]*$")}}}}},
        {QStringLiteral("additionalProperties"), false}};
    return QJsonObject{
        {QStringLiteral("name"), queryPhotosToolName()},
        {QStringLiteral("title"), QStringLiteral("Query Flexraw Catalog Photos")},
        {QStringLiteral("description"),
         QStringLiteral("Return one bounded keyset page from the active Flexraw Catalog. IDs are decimal strings.")},
        {QStringLiteral("inputSchema"), inputSchema},
        {QStringLiteral("annotations"),
         QJsonObject{{QStringLiteral("readOnlyHint"), true},
                     {QStringLiteral("destructiveHint"), false},
                     {QStringLiteral("idempotentHint"), true},
                     {QStringLiteral("openWorldHint"), false}}}};
}

McpToolCallResult McpCatalogTools::callQueryPhotos(const QJsonObject& arguments) const
{
    core::client::CatalogPhotoPageRequest request;
    QString validationError;
    if (!parsePhotoPageRequest(arguments, request, validationError))
    {
        return {.argumentsValid = false, .validationError = std::move(validationError)};
    }

    const core::client::CatalogPhotoPageResult queried = m_catalogPhotoClient->queryPhotoPage(request);
    if (queried.hasError())
    {
        return makeClientErrorToolResult(queried.error());
    }
    return {.argumentsValid = true, .structuredContent = pageToJson(queried.value())};
}

bool McpCatalogTools::parsePhotoPageRequest(const QJsonObject& arguments,
                                            core::client::CatalogPhotoPageRequest& request,
                                            QString& errorMessage) const
{
    static const QStringList AllowedKeys{QStringLiteral("page_size"),
                                         QStringLiteral("direction"),
                                         QStringLiteral("cursor"),
                                         QStringLiteral("exact_folder_path"),
                                         QStringLiteral("project_id")};
    for (auto iterator = arguments.constBegin(); iterator != arguments.constEnd(); ++iterator)
    {
        if (!AllowedKeys.contains(iterator.key()))
        {
            errorMessage = QStringLiteral("Unknown catalog query argument: %1").arg(iterator.key());
            return false;
        }
    }
    if (arguments.contains(QStringLiteral("page_size")))
    {
        const QJsonValue pageSize = arguments.value(QStringLiteral("page_size"));
        const double numeric = pageSize.toDouble(std::numeric_limits<double>::quiet_NaN());
        if (!pageSize.isDouble() || !std::isfinite(numeric) || std::floor(numeric) != numeric || numeric < 1.0 ||
            numeric > core::client::MaximumCatalogPhotoPageSize)
        {
            errorMessage = QStringLiteral("page_size must be an integer from 1 to %1.")
                               .arg(core::client::MaximumCatalogPhotoPageSize);
            return false;
        }
        request.pageSize = static_cast<std::int32_t>(numeric);
    }

    const QString direction = arguments.value(QStringLiteral("direction")).toString(QStringLiteral("forward"));
    if (direction == QStringLiteral("forward"))
    {
        request.direction = core::client::CatalogPhotoPageDirection::Forward;
    }
    else if (direction == QStringLiteral("backward"))
    {
        request.direction = core::client::CatalogPhotoPageDirection::Backward;
    }
    else
    {
        errorMessage = QStringLiteral("direction must be forward or backward.");
        return false;
    }

    if (arguments.contains(QStringLiteral("exact_folder_path")))
    {
        if (!arguments.value(QStringLiteral("exact_folder_path")).isString() ||
            arguments.value(QStringLiteral("exact_folder_path")).toString().isEmpty())
        {
            errorMessage = QStringLiteral("exact_folder_path must be a non-empty string.");
            return false;
        }
        request.exactFolderPath = fromJsonString(arguments.value(QStringLiteral("exact_folder_path")).toString());
    }
    if (arguments.contains(QStringLiteral("project_id")))
    {
        std::int64_t projectId = 0;
        if (!parsePositiveIdentity(arguments.value(QStringLiteral("project_id")), projectId))
        {
            errorMessage = QStringLiteral("project_id must be a positive decimal string.");
            return false;
        }
        request.projectId = core::client::ClientProjectId{projectId};
    }
    if (request.exactFolderPath.has_value() && request.projectId.has_value())
    {
        errorMessage = QStringLiteral("exact_folder_path and project_id are mutually exclusive.");
        return false;
    }

    if (arguments.contains(QStringLiteral("cursor")))
    {
        if (!arguments.value(QStringLiteral("cursor")).isObject())
        {
            errorMessage = QStringLiteral("cursor must be an object.");
            return false;
        }
        const QJsonObject cursorObject = arguments.value(QStringLiteral("cursor")).toObject();
        static const QStringList AllowedCursorKeys{QStringLiteral("display_name"),
                                                   QStringLiteral("photo_id"),
                                                   QStringLiteral("exact_folder_path"),
                                                   QStringLiteral("project_id")};
        for (auto iterator = cursorObject.constBegin(); iterator != cursorObject.constEnd(); ++iterator)
        {
            if (!AllowedCursorKeys.contains(iterator.key()))
            {
                errorMessage = QStringLiteral("Unknown cursor field: %1").arg(iterator.key());
                return false;
            }
        }

        const QString displayName = cursorObject.value(QStringLiteral("display_name")).toString();
        std::int64_t photoId = 0;
        if (displayName.isEmpty() || !parsePositiveIdentity(cursorObject.value(QStringLiteral("photo_id")), photoId))
        {
            errorMessage = QStringLiteral("cursor requires display_name and a positive decimal photo_id string.");
            return false;
        }
        core::client::CatalogPhotoPageCursor cursor;
        cursor.displayName = fromJsonString(displayName);
        cursor.photoId = core::client::ClientPhotoId{photoId};
        if (cursorObject.contains(QStringLiteral("exact_folder_path")))
        {
            const QString folderPath = cursorObject.value(QStringLiteral("exact_folder_path")).toString();
            if (folderPath.isEmpty())
            {
                errorMessage = QStringLiteral("cursor exact_folder_path must be a non-empty string.");
                return false;
            }
            cursor.exactFolderPath = fromJsonString(folderPath);
        }
        if (cursorObject.contains(QStringLiteral("project_id")))
        {
            std::int64_t cursorProjectId = 0;
            if (!parsePositiveIdentity(cursorObject.value(QStringLiteral("project_id")), cursorProjectId))
            {
                errorMessage = QStringLiteral("cursor project_id must be a positive decimal string.");
                return false;
            }
            cursor.projectId = core::client::ClientProjectId{cursorProjectId};
        }
        if (cursor.exactFolderPath.has_value() && cursor.projectId.has_value())
        {
            errorMessage = QStringLiteral("cursor exact_folder_path and project_id are mutually exclusive.");
            return false;
        }
        request.cursor = std::move(cursor);
    }
    return true;
}

}  // namespace flexraw::mcp
