#pragma once

#include <optional>

#include "client_error.h"
#include "client_result.h"
#include "export_client.h"

namespace flexraw::core::client
{

struct ExportExecutionDefaults
{
    ExportPlacementPolicy placementPolicy{ExportPlacementPolicy::LocalOnly};
    std::optional<WorkerProfileId> preferredWorkerProfileId;

    bool operator==(const ExportExecutionDefaults&) const = default;
};

struct ExportDefaultsSnapshot
{
    ExportRasterOptions rasterOptions;
    ExportExecutionDefaults execution;

    bool operator==(const ExportDefaultsSnapshot&) const = default;
};

using ExportDefaultsResult = ClientResult<ExportDefaultsSnapshot, ClientError>;
using ExportRasterDefaultsResult = ClientResult<ExportRasterOptions, ClientError>;
using ExportExecutionDefaultsResult = ClientResult<ExportExecutionDefaults, ClientError>;

class IExportDefaultsClient
{
public:
    // 목적: implementation별 application-level Export default resource를 concrete destructor로 정리
    // 입력: 없음
    // 출력: 없음
    virtual ~IExportDefaultsClient() = default;

    // 목적: 다음 Export presentation이 사용할 application-wide raster와 placement 기본값 조회
    // 입력: 없음
    // 출력: immutable 기본값 snapshot 또는 저장소 오류
    [[nodiscard]] virtual ExportDefaultsResult exportDefaults() const = 0;

    // 목적: 성공한 Export의 raster option을 application-wide 기본값으로 저장
    // 입력: options: format, encoding, dimension, color와 metadata option
    // 출력: 저장된 option 또는 validation·저장소 오류
    [[nodiscard]] virtual ExportRasterDefaultsResult saveRasterExportDefaults(const ExportRasterOptions& options) = 0;

    // 목적: accepted Export의 placement와 preferred Worker identity를 application-wide 기본값으로 저장
    // 입력: defaults: Local/Remote/Auto policy와 optional stable Worker identity
    // 출력: 저장된 normalized 기본값 또는 validation·저장소 오류
    [[nodiscard]] virtual ExportExecutionDefaultsResult saveExportExecutionDefaults(
        const ExportExecutionDefaults& defaults) = 0;
};

}  // namespace flexraw::core::client
