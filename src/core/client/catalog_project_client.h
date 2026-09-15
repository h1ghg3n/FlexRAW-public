#pragma once

#include <string>
#include <vector>

#include "client_error.h"
#include "client_identity.h"
#include "client_result.h"

namespace flexraw::core::client
{

struct CatalogProjectSnapshot
{
    ClientProjectId id;
    std::string name;

    bool operator==(const CatalogProjectSnapshot&) const = default;
};

struct CreateProjectCommand
{
    std::string name;
};

struct RenameProjectCommand
{
    ClientProjectId projectId;
    std::string name;
};

struct DeleteProjectCommand
{
    ClientProjectId projectId;
};

struct ProjectPhotoMembershipCommand
{
    ClientProjectId projectId;
    ClientPhotoId photoId;
};

struct CatalogProjectDeleteReceipt
{
    ClientProjectId projectId;

    bool operator==(const CatalogProjectDeleteReceipt&) const = default;
};

struct CatalogProjectMembershipReceipt
{
    ClientProjectId projectId;
    ClientPhotoId photoId;

    bool operator==(const CatalogProjectMembershipReceipt&) const = default;
};

using CatalogProjectResult = ClientResult<CatalogProjectSnapshot, ClientError>;
using CatalogProjectListResult = ClientResult<std::vector<CatalogProjectSnapshot>, ClientError>;
using CatalogProjectDeleteResult = ClientResult<CatalogProjectDeleteReceipt, ClientError>;
using CatalogProjectMembershipResult = ClientResult<CatalogProjectMembershipReceipt, ClientError>;

class ICatalogProjectClient
{
public:
    // 목적: implementation별 resource를 올바른 concrete destructor로 정리
    // 입력: 없음
    // 출력: 없음
    virtual ~ICatalogProjectClient() = default;

    // 목적: active Catalog의 Project state snapshot 목록 조회
    // 입력: 없음
    // 출력: UTF-8 이름과 fixed-width identity 목록 또는 구조화된 오류
    [[nodiscard]] virtual CatalogProjectListResult listProjects() const = 0;

    // 목적: active Catalog에 이름이 지정된 Project 생성
    // 입력: command: UTF-8 Project 표시 이름
    // 출력: 생성된 Project snapshot 또는 validation·session·database 오류
    [[nodiscard]] virtual CatalogProjectResult createProject(const CreateProjectCommand& command) = 0;

    // 목적: active Catalog Project의 표시 이름 변경
    // 입력: command: fixed-width identity와 UTF-8 새 표시 이름
    // 출력: 변경된 Project snapshot 또는 validation·session·database 오류
    [[nodiscard]] virtual CatalogProjectResult renameProject(const RenameProjectCommand& command) = 0;

    // 목적: active Catalog에서 Project와 membership만 삭제
    // 입력: command: 삭제할 Project identity
    // 출력: Photo 보존을 전제로 한 mutation receipt 또는 구조화된 오류
    [[nodiscard]] virtual CatalogProjectDeleteResult deleteProject(const DeleteProjectCommand& command) = 0;

    // 목적: 기존 Catalog Photo를 Project membership에 idempotent하게 추가
    // 입력: command: Project와 Photo identity 한 쌍
    // 출력: 처리한 identity receipt 또는 구조화된 오류
    [[nodiscard]] virtual CatalogProjectMembershipResult addPhotoToProject(
        const ProjectPhotoMembershipCommand& command) = 0;

    // 목적: 기존 Catalog Photo를 Project membership에서 idempotent하게 제거
    // 입력: command: Project와 Photo identity 한 쌍
    // 출력: 처리한 identity receipt 또는 구조화된 오류
    [[nodiscard]] virtual CatalogProjectMembershipResult removePhotoFromProject(
        const ProjectPhotoMembershipCommand& command) = 0;
};

}  // namespace flexraw::core::client
