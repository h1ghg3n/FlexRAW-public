#pragma once

#include <optional>
#include <variant>

#include <QString>
#include <QVector>

#include "catalog_database.h"
#include "catalog_project.h"
#include "photo_identity.h"
#include "result.h"

namespace flexraw::core::catalog
{

using CatalogProjectRecordResult = types::Result<CatalogProjectRecord, types::CoreError>;
using CatalogProjectListResult = types::Result<QVector<CatalogProjectRecord>, types::CoreError>;
using CatalogProjectFindResult = types::Result<std::optional<CatalogProjectRecord>, types::CoreError>;
using CatalogProjectMutationResult = types::Result<std::monostate, types::CoreError>;

class CatalogProjectRepository final
{
public:
    // 목적: 열린 catalog database에 연결된 Project repository 생성
    // 입력: database: projects와 project_photos table을 보유한 열린 CatalogDatabase
    // 출력: 초기화된 CatalogProjectRepository 객체
    explicit CatalogProjectRepository(CatalogDatabase& database);

    // 목적: active Catalog 안에 이름을 가진 Project 생성
    // 입력: name: 공백 제거 후 비어 있지 않은 표시 이름
    // 출력: 발급된 ProjectId와 정규화된 이름 또는 validation·database 오류
    [[nodiscard]] CatalogProjectRecordResult createProject(const QString& name);

    // 목적: active Catalog의 Project를 이름과 identity 순서로 조회
    // 입력: 없음
    // 출력: bounded Photo data를 포함하지 않는 Project record 목록 또는 database 오류
    [[nodiscard]] CatalogProjectListResult queryProjects() const;

    // 목적: catalog-local ProjectId로 Project record 조회
    // 입력: projectId: 조회할 Project identity
    // 출력: 일치하는 Project 또는 없으면 빈 값
    [[nodiscard]] CatalogProjectFindResult findById(ProjectId projectId) const;

    // 목적: 기존 Project의 표시 이름 변경
    // 입력: projectId: 변경 대상, name: 공백 제거 후 비어 있지 않은 새 이름
    // 출력: 갱신된 Project record 또는 validation·not-found·database 오류
    [[nodiscard]] CatalogProjectRecordResult renameProject(ProjectId projectId, const QString& name);

    // 목적: Project와 membership만 삭제
    // 입력: projectId: 삭제할 Project identity
    // 출력: Photo와 Develop state를 보존한 성공 표식 또는 not-found·database 오류
    [[nodiscard]] CatalogProjectMutationResult removeProject(ProjectId projectId);

    // 목적: Project와 Photo 사이 membership을 idempotent하게 추가
    // 입력: projectId: 대상 Project, photoId: 소유권을 이전하지 않을 Photo identity
    // 출력: 이미 존재해도 성공, identity가 없거나 database 실패면 오류
    [[nodiscard]] CatalogProjectMutationResult addPhoto(ProjectId projectId, types::PhotoId photoId);

    // 목적: Project와 Photo 사이 membership을 idempotent하게 제거
    // 입력: projectId: 대상 Project, photoId: Project에서 제외할 Photo identity
    // 출력: membership이 없어도 성공, identity가 없거나 database 실패면 오류
    [[nodiscard]] CatalogProjectMutationResult removePhoto(ProjectId projectId, types::PhotoId photoId);

private:
    CatalogDatabase& m_database;
};

}  // namespace flexraw::core::catalog
