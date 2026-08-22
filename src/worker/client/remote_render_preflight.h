#pragma once

#include <cstdint>

#include "remote_render_contracts.h"

namespace flexraw::worker::client
{

enum class RemoteRenderPreflightErrorCode : std::uint8_t
{
    InvalidProfile,
    NoStorageMarker,
    InvalidStorageMarker,
    UnsupportedStorageSchema,
    StorageMismatch,
    PathMappingFailed,
    ShuttingDown,
    RequestIdExhausted,
};

struct RemoteRenderPreflightError
{
    RemoteRenderPreflightErrorCode code{RemoteRenderPreflightErrorCode::PathMappingFailed};
    core::types::CoreError cause;
};

using RemoteRenderPreflightResult = core::types::Result<RemoteRenderRequest, RemoteRenderPreflightError>;

// 목적: Desktop absolute render request를 shared-storage 검증 후 Worker wire request로 변환
// 입력: profile: endpoint storage identity, request: Desktop local absolute path와 processing snapshot
// 출력: root-relative Remote request 또는 marker/profile/path typed 오류
[[nodiscard]] RemoteRenderPreflightResult prepareRemoteRenderRequest(
    const RemoteWorkerProfile& profile, const core::render::ResolvedRenderRequest& request);

}  // namespace flexraw::worker::client
