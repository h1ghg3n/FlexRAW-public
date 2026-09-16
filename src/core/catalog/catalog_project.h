#pragma once

#include <QString>
#include <QtTypes>

namespace flexraw::core::catalog
{

struct ProjectId
{
    qint64 value{0};

    bool operator==(const ProjectId&) const = default;
};

struct CatalogProjectRecord
{
    ProjectId id;
    QString name;
};

// 목적: ProjectId가 catalog에서 발급된 양의 SQLite identity인지 확인
// 입력: projectId: 확인할 catalog-local Project identity
// 출력: 0보다 큰 identity면 true
[[nodiscard]] constexpr bool isValidProjectId(ProjectId projectId) noexcept
{
    return projectId.value > 0;
}

}  // namespace flexraw::core::catalog
