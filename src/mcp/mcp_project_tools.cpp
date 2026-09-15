#include "mcp_project_tools.h"

#include <cstdint>

#include <QJsonArray>
#include <QStringList>

#include "mcp_client_error_projection.h"
#include "mcp_json_projection.h"

namespace flexraw::mcp
{
namespace
{

constexpr auto ProjectListToolName = "catalog_list_projects";
constexpr auto ProjectMembershipAddToolName = "catalog_add_photo_to_project";

// 목적: Project snapshot을 precision-safe MCP JSON object로 투영
// 입력: project: frontend-neutral Project snapshot
// 출력: decimal string identity와 UTF-8 name object
[[nodiscard]] QJsonObject projectToJson(const core::client::CatalogProjectSnapshot& project)
{
    return QJsonObject{{QStringLiteral("project_id"), QString::number(project.id.value)},
                       {QStringLiteral("name"), toJsonString(project.name)}};
}

}  // namespace

// 목적: MCP Project projection을 frontend-neutral Project client와 write policy에 연결
// 입력: projectClient: Project command consumer, writesEnabled: explicit process write opt-in
// 출력: client lifetime 동안 사용할 Project MCP adapter
McpProjectTools::McpProjectTools(core::client::ICatalogProjectClient& projectClient, bool writesEnabled) noexcept
    : m_projectClient(&projectClient), m_writesEnabled(writesEnabled)
{}

// 목적: Project list tool의 stable wire name 반환
// 입력: 없음
// 출력: catalog_list_projects
QString McpProjectTools::listProjectsToolName()
{
    return QString::fromLatin1(ProjectListToolName);
}

// 목적: idempotent membership add tool의 stable wire name 반환
// 입력: 없음
// 출력: catalog_add_photo_to_project
QString McpProjectTools::addPhotoToProjectToolName()
{
    return QString::fromLatin1(ProjectMembershipAddToolName);
}

// 목적: Project list tool의 schema와 read-only hint 반환
// 입력: 없음
// 출력: tools/list에 포함할 descriptor
QJsonObject McpProjectTools::listProjectsDescriptor() const
{
    return QJsonObject{
        {QStringLiteral("name"), listProjectsToolName()},
        {QStringLiteral("title"), QStringLiteral("List Flexraw Catalog Projects")},
        {QStringLiteral("description"), QStringLiteral("Return the logical Projects in the active Flexraw Catalog.")},
        {QStringLiteral("inputSchema"),
         QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                     {QStringLiteral("additionalProperties"), false}}},
        {QStringLiteral("annotations"),
         QJsonObject{{QStringLiteral("readOnlyHint"), true},
                     {QStringLiteral("destructiveHint"), false},
                     {QStringLiteral("idempotentHint"), true},
                     {QStringLiteral("openWorldHint"), false}}}};
}

// 목적: Project membership add tool의 schema와 mutation hint 반환
// 입력: 없음
// 출력: write-enabled tools/list에 포함할 descriptor
QJsonObject McpProjectTools::addPhotoToProjectDescriptor() const
{
    const QJsonObject identitySchema{{QStringLiteral("type"), QStringLiteral("string")},
                                     {QStringLiteral("pattern"), QStringLiteral("^[1-9][0-9]*$")}};
    return QJsonObject{
        {QStringLiteral("name"), addPhotoToProjectToolName()},
        {QStringLiteral("title"), QStringLiteral("Add Photo to Flexraw Project")},
        {QStringLiteral("description"),
         QStringLiteral("Idempotently add an existing Catalog photo to an existing Project. Writes must be enabled.")},
        {QStringLiteral("inputSchema"),
         QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                     {QStringLiteral("properties"),
                      QJsonObject{{QStringLiteral("project_id"), identitySchema},
                                  {QStringLiteral("photo_id"), identitySchema}}},
                     {QStringLiteral("required"), QJsonArray{QStringLiteral("project_id"), QStringLiteral("photo_id")}},
                     {QStringLiteral("additionalProperties"), false}}},
        {QStringLiteral("annotations"),
         QJsonObject{{QStringLiteral("readOnlyHint"), false},
                     {QStringLiteral("destructiveHint"), false},
                     {QStringLiteral("idempotentHint"), true},
                     {QStringLiteral("openWorldHint"), false}}}};
}

// 목적: Project 목록을 frontend-neutral client에서 조회해 JSON으로 투영
// 입력: arguments: 비어 있어야 하는 tool arguments
// 출력: protocol validation 또는 structured success/domain failure
McpToolCallResult McpProjectTools::callListProjects(const QJsonObject& arguments) const
{
    if (!arguments.isEmpty())
    {
        return {.argumentsValid = false,
                .validationError = QStringLiteral("catalog_list_projects takes no arguments.")};
    }

    const core::client::CatalogProjectListResult listed = m_projectClient->listProjects();
    if (listed.hasError())
    {
        return makeClientErrorToolResult(listed.error());
    }
    QJsonArray projects;
    for (const core::client::CatalogProjectSnapshot& project : listed.value())
    {
        projects.append(projectToJson(project));
    }
    return {.argumentsValid = true,
            .structuredContent = QJsonObject{{QStringLiteral("projects"), std::move(projects)}}};
}

// 목적: write opt-in을 확인하고 idempotent Project membership 추가 수행
// 입력: arguments: decimal string project_id와 photo_id
// 출력: protocol validation 또는 structured success/domain failure
McpToolCallResult McpProjectTools::callAddPhotoToProject(const QJsonObject& arguments) const
{
    if (!m_writesEnabled)
    {
        return makeClientErrorToolResult(
            {core::client::ClientErrorCode::PermissionDenied, "MCP write access is disabled."});
    }

    static const QStringList AllowedKeys{QStringLiteral("project_id"), QStringLiteral("photo_id")};
    for (auto iterator = arguments.constBegin(); iterator != arguments.constEnd(); ++iterator)
    {
        if (!AllowedKeys.contains(iterator.key()))
        {
            return {.argumentsValid = false,
                    .validationError = QStringLiteral("Unknown Project membership argument: %1").arg(iterator.key())};
        }
    }
    std::int64_t projectId = 0;
    std::int64_t photoId = 0;
    if (!parsePositiveIdentity(arguments.value(QStringLiteral("project_id")), projectId) ||
        !parsePositiveIdentity(arguments.value(QStringLiteral("photo_id")), photoId))
    {
        return {.argumentsValid = false,
                .validationError = QStringLiteral("project_id and photo_id must be positive decimal strings.")};
    }

    const core::client::CatalogProjectMembershipResult added =
        m_projectClient->addPhotoToProject({{projectId}, {photoId}});
    if (added.hasError())
    {
        return makeClientErrorToolResult(added.error());
    }
    return {.argumentsValid = true,
            .structuredContent =
                QJsonObject{{QStringLiteral("membership"),
                             QJsonObject{{QStringLiteral("project_id"), QString::number(added.value().projectId.value)},
                                         {QStringLiteral("photo_id"), QString::number(added.value().photoId.value)}}}}};
}

// 목적: process가 명시적 write tool 광고를 허용했는지 확인
// 입력: 없음
// 출력: --allow-write가 지정됐으면 true
bool McpProjectTools::writesEnabled() const noexcept
{
    return m_writesEnabled;
}

}  // namespace flexraw::mcp
