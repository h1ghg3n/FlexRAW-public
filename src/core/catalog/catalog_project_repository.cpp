#include "catalog_project_repository.h"

#include <utility>

#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace flexraw::core::catalog
{
namespace
{

// 목적: Project persistence 실패 정보를 DatabaseError CoreError로 생성
// 입력: message: 호출자에게 전달할 database 오류 설명
// 출력: DatabaseError로 분류된 CoreError 객체
[[nodiscard]] types::CoreError makeDatabaseError(QString message)
{
    return {types::ErrorCode::DatabaseError, std::move(message)};
}

// 목적: Project command의 잘못된 입력을 InvalidArgument CoreError로 생성
// 입력: message: 호출자에게 전달할 validation 설명
// 출력: InvalidArgument로 분류된 CoreError 객체
[[nodiscard]] types::CoreError makeInvalidArgumentError(QString message)
{
    return {types::ErrorCode::InvalidArgument, std::move(message)};
}

// 목적: 존재하지 않는 Project 또는 Photo identity를 NotFound CoreError로 생성
// 입력: message: 호출자에게 전달할 missing entity 설명
// 출력: NotFound로 분류된 CoreError 객체
[[nodiscard]] types::CoreError makeNotFoundError(QString message)
{
    return {types::ErrorCode::NotFound, std::move(message)};
}

// 목적: user input Project 이름을 persistence 형태로 정규화
// 입력: name: 앞뒤 공백을 포함할 수 있는 표시 이름
// 출력: 앞뒤 공백이 제거된 이름
[[nodiscard]] QString normalizeProjectName(const QString& name)
{
    return name.trimmed();
}

// 목적: SQLite query의 현재 row를 Project record로 변환
// 입력: query: id와 name column을 순서대로 보유한 query row
// 출력: 검증된 Project record 또는 손상된 catalog data 오류
[[nodiscard]] CatalogProjectRecordResult readProjectRecord(const QSqlQuery& query)
{
    CatalogProjectRecord record{{query.value(0).toLongLong()}, query.value(1).toString()};
    if (!isValidProjectId(record.id) || record.name.trimmed().isEmpty())
    {
        return CatalogProjectRecordResult::failure(
            makeDatabaseError(QStringLiteral("The catalog contains an invalid Project record.")));
    }
    return CatalogProjectRecordResult::success(std::move(record));
}

// 목적: membership command 대상 Project와 Photo identity 존재 여부 확인
// 입력: database: 열린 SQLite connection, projectId/photoId: membership 양쪽 identity
// 출력: 둘 다 존재하면 성공, invalid·not-found·database 오류면 실패
[[nodiscard]] CatalogProjectMutationResult validateMembershipTargets(QSqlDatabase& database,
                                                                     ProjectId projectId,
                                                                     types::PhotoId photoId)
{
    if (!isValidProjectId(projectId) || !types::isValidPhotoId(photoId))
    {
        return CatalogProjectMutationResult::failure(
            makeInvalidArgumentError(QStringLiteral("Project membership identity is invalid.")));
    }

    QSqlQuery query(database);
    query.prepare(QStringLiteral("SELECT EXISTS(SELECT 1 FROM projects WHERE id = :projectId), "
                                 "EXISTS(SELECT 1 FROM photos WHERE id = :photoId)"));
    query.bindValue(QStringLiteral(":projectId"), projectId.value);
    query.bindValue(QStringLiteral(":photoId"), photoId.value);
    if (!query.exec() || !query.next())
    {
        return CatalogProjectMutationResult::failure(makeDatabaseError(
            QStringLiteral("Unable to validate Project membership targets: %1").arg(query.lastError().text())));
    }
    if (!query.value(0).toBool())
    {
        return CatalogProjectMutationResult::failure(makeNotFoundError(QStringLiteral("Project does not exist.")));
    }
    if (!query.value(1).toBool())
    {
        return CatalogProjectMutationResult::failure(
            makeNotFoundError(QStringLiteral("Catalog photo does not exist.")));
    }
    return CatalogProjectMutationResult::success(std::monostate{});
}

}  // namespace

// 목적: 열린 catalog database에 연결된 Project repository 생성
// 입력: database: projects와 project_photos table을 보유한 열린 CatalogDatabase
// 출력: 초기화된 CatalogProjectRepository 객체
CatalogProjectRepository::CatalogProjectRepository(CatalogDatabase& database) : m_database(database) {}

// 목적: active Catalog 안에 이름을 가진 Project 생성
// 입력: name: 공백 제거 후 비어 있지 않은 표시 이름
// 출력: 발급된 ProjectId와 정규화된 이름 또는 validation·database 오류
CatalogProjectRecordResult CatalogProjectRepository::createProject(const QString& name)
{
    const QString normalizedName = normalizeProjectName(name);
    if (normalizedName.isEmpty())
    {
        return CatalogProjectRecordResult::failure(makeInvalidArgumentError(QStringLiteral("Project name is empty.")));
    }

    QSqlQuery query(m_database.m_database);
    query.prepare(QStringLiteral("INSERT INTO projects (name) VALUES (:name)"));
    query.bindValue(QStringLiteral(":name"), normalizedName);
    if (!query.exec())
    {
        return CatalogProjectRecordResult::failure(
            makeDatabaseError(QStringLiteral("Unable to create Project: %1").arg(query.lastError().text())));
    }

    const ProjectId projectId{query.lastInsertId().toLongLong()};
    if (!isValidProjectId(projectId))
    {
        return CatalogProjectRecordResult::failure(
            makeDatabaseError(QStringLiteral("Created Project did not return a valid identity.")));
    }
    return CatalogProjectRecordResult::success({projectId, normalizedName});
}

// 목적: active Catalog의 Project를 이름과 identity 순서로 조회
// 입력: 없음
// 출력: bounded Photo data를 포함하지 않는 Project record 목록 또는 database 오류
CatalogProjectListResult CatalogProjectRepository::queryProjects() const
{
    QSqlQuery query(m_database.m_database);
    if (!query.exec(QStringLiteral("SELECT id, name FROM projects ORDER BY name COLLATE NOCASE ASC, id ASC")))
    {
        return CatalogProjectListResult::failure(
            makeDatabaseError(QStringLiteral("Unable to query Projects: %1").arg(query.lastError().text())));
    }

    QVector<CatalogProjectRecord> projects;
    while (query.next())
    {
        CatalogProjectRecordResult project = readProjectRecord(query);
        if (project.hasError())
        {
            return CatalogProjectListResult::failure(project.error());
        }
        projects.push_back(std::move(project.value()));
    }
    return CatalogProjectListResult::success(std::move(projects));
}

// 목적: catalog-local ProjectId로 Project record 조회
// 입력: projectId: 조회할 Project identity
// 출력: 일치하는 Project 또는 없으면 빈 값
CatalogProjectFindResult CatalogProjectRepository::findById(ProjectId projectId) const
{
    if (!isValidProjectId(projectId))
    {
        return CatalogProjectFindResult::failure(
            makeInvalidArgumentError(QStringLiteral("Project identity is invalid.")));
    }

    QSqlQuery query(m_database.m_database);
    query.prepare(QStringLiteral("SELECT id, name FROM projects WHERE id = :projectId"));
    query.bindValue(QStringLiteral(":projectId"), projectId.value);
    if (!query.exec())
    {
        return CatalogProjectFindResult::failure(
            makeDatabaseError(QStringLiteral("Unable to find Project: %1").arg(query.lastError().text())));
    }
    if (!query.next())
    {
        return CatalogProjectFindResult::success(std::nullopt);
    }

    CatalogProjectRecordResult project = readProjectRecord(query);
    return project.hasError() ? CatalogProjectFindResult::failure(project.error())
                              : CatalogProjectFindResult::success(std::move(project.value()));
}

// 목적: 기존 Project의 표시 이름 변경
// 입력: projectId: 변경 대상, name: 공백 제거 후 비어 있지 않은 새 이름
// 출력: 갱신된 Project record 또는 validation·not-found·database 오류
CatalogProjectRecordResult CatalogProjectRepository::renameProject(ProjectId projectId, const QString& name)
{
    const QString normalizedName = normalizeProjectName(name);
    if (!isValidProjectId(projectId) || normalizedName.isEmpty())
    {
        return CatalogProjectRecordResult::failure(
            makeInvalidArgumentError(QStringLiteral("Project identity or name is invalid.")));
    }

    QSqlQuery query(m_database.m_database);
    query.prepare(QStringLiteral("UPDATE projects SET name = :name WHERE id = :projectId"));
    query.bindValue(QStringLiteral(":name"), normalizedName);
    query.bindValue(QStringLiteral(":projectId"), projectId.value);
    if (!query.exec())
    {
        return CatalogProjectRecordResult::failure(
            makeDatabaseError(QStringLiteral("Unable to rename Project: %1").arg(query.lastError().text())));
    }
    if (query.numRowsAffected() != 1)
    {
        return CatalogProjectRecordResult::failure(makeNotFoundError(QStringLiteral("Project does not exist.")));
    }
    return CatalogProjectRecordResult::success({projectId, normalizedName});
}

// 목적: Project와 membership만 삭제
// 입력: projectId: 삭제할 Project identity
// 출력: Photo와 Develop state를 보존한 성공 표식 또는 not-found·database 오류
CatalogProjectMutationResult CatalogProjectRepository::removeProject(ProjectId projectId)
{
    if (!isValidProjectId(projectId))
    {
        return CatalogProjectMutationResult::failure(
            makeInvalidArgumentError(QStringLiteral("Project identity is invalid.")));
    }

    QSqlQuery query(m_database.m_database);
    query.prepare(QStringLiteral("DELETE FROM projects WHERE id = :projectId"));
    query.bindValue(QStringLiteral(":projectId"), projectId.value);
    if (!query.exec())
    {
        return CatalogProjectMutationResult::failure(
            makeDatabaseError(QStringLiteral("Unable to remove Project: %1").arg(query.lastError().text())));
    }
    if (query.numRowsAffected() != 1)
    {
        return CatalogProjectMutationResult::failure(makeNotFoundError(QStringLiteral("Project does not exist.")));
    }
    return CatalogProjectMutationResult::success(std::monostate{});
}

// 목적: Project와 Photo 사이 membership을 idempotent하게 추가
// 입력: projectId: 대상 Project, photoId: 소유권을 이전하지 않을 Photo identity
// 출력: 이미 존재해도 성공, identity가 없거나 database 실패면 오류
CatalogProjectMutationResult CatalogProjectRepository::addPhoto(ProjectId projectId, types::PhotoId photoId)
{
    QSqlDatabase& database = m_database.m_database;
    CatalogProjectMutationResult validation = validateMembershipTargets(database, projectId, photoId);
    if (validation.hasError())
    {
        return validation;
    }

    QSqlQuery query(database);
    query.prepare(QStringLiteral("INSERT OR IGNORE INTO project_photos (project_id, photo_id) "
                                 "VALUES (:projectId, :photoId)"));
    query.bindValue(QStringLiteral(":projectId"), projectId.value);
    query.bindValue(QStringLiteral(":photoId"), photoId.value);
    if (!query.exec())
    {
        return CatalogProjectMutationResult::failure(makeDatabaseError(
            QStringLiteral("Unable to add Project photo membership: %1").arg(query.lastError().text())));
    }
    return CatalogProjectMutationResult::success(std::monostate{});
}

// 목적: Project와 Photo 사이 membership을 idempotent하게 제거
// 입력: projectId: 대상 Project, photoId: Project에서 제외할 Photo identity
// 출력: membership이 없어도 성공, identity가 없거나 database 실패면 오류
CatalogProjectMutationResult CatalogProjectRepository::removePhoto(ProjectId projectId, types::PhotoId photoId)
{
    QSqlDatabase& database = m_database.m_database;
    CatalogProjectMutationResult validation = validateMembershipTargets(database, projectId, photoId);
    if (validation.hasError())
    {
        return validation;
    }

    QSqlQuery query(database);
    query.prepare(QStringLiteral("DELETE FROM project_photos WHERE project_id = :projectId AND photo_id = :photoId"));
    query.bindValue(QStringLiteral(":projectId"), projectId.value);
    query.bindValue(QStringLiteral(":photoId"), photoId.value);
    if (!query.exec())
    {
        return CatalogProjectMutationResult::failure(makeDatabaseError(
            QStringLiteral("Unable to remove Project photo membership: %1").arg(query.lastError().text())));
    }
    return CatalogProjectMutationResult::success(std::monostate{});
}

}  // namespace flexraw::core::catalog
