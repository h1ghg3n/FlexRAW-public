#include "remote_render_preflight.h"

#include <utility>

#include "remote_render_request_mapper.h"
#include "shared_storage_locator.h"

namespace flexraw::worker::client
{
namespace
{

// 목적: preflight 분류와 공통 CoreError를 typed 오류로 구성
// 입력: code/coreCode/message: preflight 원인과 adapter-facing 진단
// 출력: 구성된 RemoteRenderPreflightError
[[nodiscard]] RemoteRenderPreflightError makePreflightError(const RemoteRenderPreflightErrorCode code,
                                                            const core::types::ErrorCode coreCode,
                                                            QString message)
{
    return {code, {coreCode, std::move(message)}};
}

// 목적: shared storage marker 탐색 오류를 공통 preflight 오류로 변환
// 입력: error: typed marker/path locator 오류
// 출력: marker 부재/schema/path 원인을 보존한 오류
[[nodiscard]] RemoteRenderPreflightError mapPreflightError(const SharedStorageLocatorError& error)
{
    switch (error.code)
    {
    case SharedStorageLocatorErrorCode::NoStorageMarker:
        return makePreflightError(
            RemoteRenderPreflightErrorCode::NoStorageMarker, core::types::ErrorCode::NotFound, error.message);
    case SharedStorageLocatorErrorCode::UnsupportedSchema:
        return makePreflightError(RemoteRenderPreflightErrorCode::UnsupportedStorageSchema,
                                  core::types::ErrorCode::InvalidArgument,
                                  error.message);
    case SharedStorageLocatorErrorCode::MarkerUnreadable:
    case SharedStorageLocatorErrorCode::MalformedMarker:
    case SharedStorageLocatorErrorCode::InvalidStorageId:
        return makePreflightError(RemoteRenderPreflightErrorCode::InvalidStorageMarker,
                                  core::types::ErrorCode::InvalidArgument,
                                  error.message);
    case SharedStorageLocatorErrorCode::SourceNotFound:
    case SharedStorageLocatorErrorCode::OutputParentNotFound:
        return makePreflightError(
            RemoteRenderPreflightErrorCode::PathMappingFailed, core::types::ErrorCode::NotFound, error.message);
    case SharedStorageLocatorErrorCode::InvalidPath:
    case SharedStorageLocatorErrorCode::NonPortablePath:
        return makePreflightError(RemoteRenderPreflightErrorCode::PathMappingFailed,
                                  core::types::ErrorCode::InvalidArgument,
                                  error.message);
    }
    return makePreflightError(RemoteRenderPreflightErrorCode::PathMappingFailed,
                              core::types::ErrorCode::Unknown,
                              QStringLiteral("Shared storage preflight failed."));
}

// 목적: mapper path 오류를 공통 preflight 오류로 변환
// 입력: error: typed canonical root mapping 오류
// 출력: 적절한 InvalidArgument/NotFound 오류
[[nodiscard]] RemoteRenderPreflightError mapPreflightError(const RemoteRenderPathError& error)
{
    const bool notFound = error.code == RemoteRenderPathErrorCode::SourceNotFound ||
                          error.code == RemoteRenderPathErrorCode::OutputParentNotFound;
    return makePreflightError(RemoteRenderPreflightErrorCode::PathMappingFailed,
                              notFound ? core::types::ErrorCode::NotFound : core::types::ErrorCode::InvalidArgument,
                              error.message);
}

}  // namespace

// 목적: Desktop absolute render request를 shared-storage 검증 후 Worker wire request로 변환
// 입력: profile: endpoint storage identity, request: Desktop local absolute path와 processing snapshot
// 출력: root-relative Remote request 또는 marker/profile/path typed 오류
RemoteRenderPreflightResult prepareRemoteRenderRequest(const RemoteWorkerProfile& profile,
                                                       const core::render::ResolvedRenderRequest& request)
{
    if (profile.expectedSourceStorageId.isNull() || profile.expectedOutputStorageId.isNull())
    {
        return RemoteRenderPreflightResult::failure(
            makePreflightError(RemoteRenderPreflightErrorCode::InvalidProfile,
                               core::types::ErrorCode::InvalidArgument,
                               QStringLiteral("Remote Worker storage profile is invalid.")));
    }

    const LocateSharedStorageResult sourceStorage = SharedStorageLocator::locateSource(request.sourcePath);
    if (sourceStorage.hasError())
    {
        return RemoteRenderPreflightResult::failure(mapPreflightError(sourceStorage.error()));
    }
    const LocateSharedStorageResult outputStorage = SharedStorageLocator::locateOutput(request.outputPath);
    if (outputStorage.hasError())
    {
        return RemoteRenderPreflightResult::failure(mapPreflightError(outputStorage.error()));
    }
    if (sourceStorage.value().storageId != profile.expectedSourceStorageId)
    {
        return RemoteRenderPreflightResult::failure(
            makePreflightError(RemoteRenderPreflightErrorCode::StorageMismatch,
                               core::types::ErrorCode::Conflict,
                               QStringLiteral("Source shared storage does not match the configured Worker profile.")));
    }
    if (outputStorage.value().storageId != profile.expectedOutputStorageId)
    {
        return RemoteRenderPreflightResult::failure(
            makePreflightError(RemoteRenderPreflightErrorCode::StorageMismatch,
                               core::types::ErrorCode::Conflict,
                               QStringLiteral("Output shared storage does not match the configured Worker profile.")));
    }

    RemoteRenderRequestMapper::CreateResult mapper =
        RemoteRenderRequestMapper::create({sourceStorage.value().localRoot, outputStorage.value().localRoot});
    if (mapper.hasError())
    {
        return RemoteRenderPreflightResult::failure(mapPreflightError(mapper.error()));
    }
    RemoteRenderRequestMapper::MapResult mapped = mapper.value().map(request);
    if (mapped.hasError())
    {
        return RemoteRenderPreflightResult::failure(mapPreflightError(mapped.error()));
    }
    if (mapped.value().sourceRelativePath != sourceStorage.value().relativePath ||
        mapped.value().outputRelativePath != outputStorage.value().relativePath)
    {
        return RemoteRenderPreflightResult::failure(
            makePreflightError(RemoteRenderPreflightErrorCode::PathMappingFailed,
                               core::types::ErrorCode::Conflict,
                               QStringLiteral("Shared storage path changed during Remote render preflight.")));
    }
    return RemoteRenderPreflightResult::success(std::move(mapped.value()));
}

}  // namespace flexraw::worker::client
