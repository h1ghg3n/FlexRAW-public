#pragma once

#include <functional>
#include <optional>

#include <QMutex>

#include "export_contracts.h"

namespace flexraw::core::orchestration
{

struct ExportProgressSnapshot
{
    int completedCount{0};
    int totalCount{0};
    int succeededCount{0};
    int failedCount{0};
    QString currentSourcePath;
};

using ExportProgressCallback = std::function<void(ExportProgressSnapshot)>;
using ExportPipelineResult = types::Result<ExportReport, types::CoreError>;

struct PreparedExportItem
{
    ExportFileRequest request;
    std::optional<types::CoreError> preparationError;
};

struct PreparedExport
{
    QVector<PreparedExportItem> items;
};

using ExportPreparationResult = types::Result<PreparedExport, types::CoreError>;

class IExportPipeline
{
public:
    virtual ~IExportPipeline() = default;

    // 목적: Catalog/scan 의존성을 worker 실행 전 immutable file item으로 해석
    // 입력: request: file 또는 batch 요청, cancellationToken: cooperative cancellation 상태
    // 출력: 실행 가능한 item 목록 또는 request 준비 단계 오류
    [[nodiscard]] virtual ExportPreparationResult prepare(
        const ExportRequest& request, const types::CancellationToken& cancellationToken) const;

    // 목적: 준비된 item 하나를 synchronous processing leaf로 실행
    // 입력: item: source/output/resolved develop snapshot, cancellationToken: cooperative cancellation 상태
    // 출력: item 단위 성공 또는 실패 상세 결과
    [[nodiscard]] virtual ExportItemResult executeItem(
        const PreparedExportItem& item, const types::CancellationToken& cancellationToken) const;

    // 목적: 한 export request를 synchronous pipeline으로 처리
    // 입력: request: file 또는 batch 요청, cancellationToken: cooperative cancellation 상태, progress: 진행 callback
    // 출력: item별 상세 report 또는 request 준비 단계 오류
    [[nodiscard]] virtual ExportPipelineResult execute(const ExportRequest& request,
                                                       const types::CancellationToken& cancellationToken,
                                                       const ExportProgressCallback& progress) const = 0;
};

class FileExportPipeline final : public IExportPipeline
{
public:
    // 목적: file 또는 folder request를 resolved immutable item 목록으로 준비
    // 입력: request: file 또는 batch 요청, cancellationToken: cooperative cancellation 상태
    // 출력: item 목록 또는 scan/output/catalog 준비 오류
    [[nodiscard]] ExportPreparationResult prepare(
        const ExportRequest& request, const types::CancellationToken& cancellationToken) const override;

    // 목적: 준비된 file item 하나를 RAW/raster processing leaf로 실행
    // 입력: item: resolved source/output/develop 값, cancellationToken: cooperative cancellation 상태
    // 출력: item 단위 성공 또는 실패 상세 결과
    [[nodiscard]] ExportItemResult executeItem(
        const PreparedExportItem& item, const types::CancellationToken& cancellationToken) const override;

    // 목적: Core domain module을 조합해 file-backed export request 처리
    // 입력: request: file 또는 batch 요청, cancellationToken: cooperative cancellation 상태, progress: 진행 callback
    // 출력: item별 상세 report 또는 scan, catalog, output 준비 오류
    [[nodiscard]] ExportPipelineResult execute(const ExportRequest& request,
                                               const types::CancellationToken& cancellationToken,
                                               const ExportProgressCallback& progress) const override;

private:
    mutable QMutex m_catalogAccessMutex;
};

}  // namespace flexraw::core::orchestration
