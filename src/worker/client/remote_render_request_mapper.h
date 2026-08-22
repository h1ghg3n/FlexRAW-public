#pragma once

#include <cstdint>

#include <QString>

#include "remote_render_contracts.h"
#include "render_contracts.h"
#include "result.h"

namespace flexraw::worker::client
{

struct RemoteRenderLocalRoots
{
    QString sourceRoot;
    QString outputRoot;
};

enum class RemoteRenderPathErrorCode : std::uint8_t
{
    InvalidRoot,
    InvalidPath,
    SourceNotFound,
    SourceOutsideRoot,
    OutputParentNotFound,
    OutputOutsideRoot,
    NonPortablePath,
};

struct RemoteRenderPathError
{
    RemoteRenderPathErrorCode code{RemoteRenderPathErrorCode::InvalidPath};
    QString message;
};

class RemoteRenderRequestMapper final
{
public:
    using CreateResult = core::types::Result<RemoteRenderRequestMapper, RemoteRenderPathError>;
    using MapResult = core::types::Result<RemoteRenderRequest, RemoteRenderPathError>;

    // 목적: Desktop local root와 Worker root의 relative-path 대응 경계 생성
    // 입력: roots: Worker source/output root에 각각 대응하는 기존 absolute local directory
    // 출력: canonical local root를 보유한 mapper 또는 configuration 오류
    [[nodiscard]] static CreateResult create(const RemoteRenderLocalRoots& roots);

    // 목적: local absolute render request를 Worker wire용 root-relative request로 변환
    // 입력: request: 기존 source file과 output parent를 사용하는 resolved render request
    // 출력: portable relative path를 가진 remote request 또는 path mapping 오류
    [[nodiscard]] MapResult map(const core::render::ResolvedRenderRequest& request) const;

private:
    // 목적: 검증된 canonical local root로 mapper 구성
    // 입력: sourceRoot/outputRoot: Worker root와 동일 relative layout을 갖는 local root
    // 출력: request mapping에 사용할 mapper
    RemoteRenderRequestMapper(QString sourceRoot, QString outputRoot);

    QString m_sourceRoot;
    QString m_outputRoot;
};

}  // namespace flexraw::worker::client
