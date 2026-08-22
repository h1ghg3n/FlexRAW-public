#include "resolved_render_pipeline.h"

#include <algorithm>
#include <utility>

#include <QFileInfo>

#include "develop.h"
#include "export.h"
#include "raw_file_reader.h"
#include "stage_timer.h"

namespace flexraw::core::render
{
namespace
{

// 목적: cancellation state를 render pipeline failure로 변환
// 입력: stats: 이미 완료된 stage timing, totalTimer: 전체 실행 timer
// 출력: total timing과 Cancelled CoreError를 포함한 실패 결과
[[nodiscard]] ResolvedRenderPipelineResult makeCancelledResult(measurement::RenderStats stats,
                                                               const measurement::StageTimer& totalTimer)
{
    stats.totalNanoseconds = totalTimer.elapsedNanoseconds();
    return ResolvedRenderPipelineResult::failure(
        {{types::ErrorCode::Cancelled, QStringLiteral("Resolved render cancellation requested.")}, stats});
}

// 목적: domain error에 완료된 stage와 total timing을 결합
// 입력: error: 실패 원인, stats: 완료 stage timing, totalTimer: 전체 실행 timer
// 출력: timing을 보존한 resolved render 실패
[[nodiscard]] ResolvedRenderPipelineResult makeFailure(types::CoreError error,
                                                       measurement::RenderStats stats,
                                                       const measurement::StageTimer& totalTimer)
{
    stats.totalNanoseconds = totalTimer.elapsedNanoseconds();
    return ResolvedRenderPipelineResult::failure({std::move(error), stats});
}

}  // namespace

// 목적: RAW decode, source conversion, develop, raster output을 순차적으로 실행
// 입력: request: resolved render 값, cancellationToken: stage 사이에 확인할 중단 상태
// 출력: atomic output artifact와 RenderStats 또는 stage failure
ResolvedRenderPipelineResult ResolvedRenderPipeline::execute(const ResolvedRenderRequest& request,
                                                             const types::CancellationToken& cancellationToken) const
{
    const measurement::StageTimer totalTimer;
    measurement::RenderStats stats;

    if (cancellationToken.isCancellationRequested())
    {
        return makeCancelledResult(stats, totalTimer);
    }

    const develop::DevelopParamsValidationResult validatedParams =
        develop::validateDevelopParams(request.developParams);
    if (validatedParams.hasError())
    {
        return makeFailure(validatedParams.error(), stats, totalTimer);
    }

    const export_::RasterExportResult validatedOptions = export_::validateRasterExportOptions(request.outputOptions);
    if (validatedOptions.hasError())
    {
        return makeFailure(validatedOptions.error(), stats, totalTimer);
    }

    const export_::RasterExportPathResult validatedOutputPath = export_::validateRasterExportPath(request.outputPath);
    if (validatedOutputPath.hasError())
    {
        return makeFailure(validatedOutputPath.error(), stats, totalTimer);
    }

    const measurement::StageTimer decodeTimer;
    const raw::RawPreviewImageResult decodedRaw = raw::decodeRawPreviewImage(request.sourcePath);
    stats.decodeNanoseconds = decodeTimer.elapsedNanoseconds();
    if (decodedRaw.hasError())
    {
        return makeFailure(decodedRaw.error(), stats, totalTimer);
    }

    if (cancellationToken.isCancellationRequested())
    {
        return makeCancelledResult(stats, totalTimer);
    }

    const measurement::StageTimer conversionTimer;
    const export_::RasterSourceImageResult sourceImage = export_::prepareRawForRasterExport(decodedRaw.value());
    stats.sourceConversionNanoseconds = conversionTimer.elapsedNanoseconds();
    if (sourceImage.hasError())
    {
        return makeFailure(sourceImage.error(), stats, totalTimer);
    }

    if (cancellationToken.isCancellationRequested())
    {
        return makeCancelledResult(stats, totalTimer);
    }

    const measurement::StageTimer developTimer;
    const develop::DevelopImageResult developedImage =
        develop::applyDevelop(sourceImage.value(), validatedParams.value());
    stats.developNanoseconds = developTimer.elapsedNanoseconds();
    if (developedImage.hasError())
    {
        return makeFailure(developedImage.error(), stats, totalTimer);
    }

    if (cancellationToken.isCancellationRequested())
    {
        return makeCancelledResult(stats, totalTimer);
    }

    const measurement::StageTimer outputTimer;
    const export_::RasterExportResult exported = export_::writeRasterImage(developedImage.value(),
                                                                           validatedOutputPath.value(),
                                                                           request.outputOptions,
                                                                           decodedRaw.value().diagnostics.sourcePath,
                                                                           &cancellationToken);
    stats.outputNanoseconds = outputTimer.elapsedNanoseconds();
    if (exported.hasError())
    {
        return makeFailure(exported.error(), stats, totalTimer);
    }

    const QFileInfo artifactInfo(validatedOutputPath.value());
    const qint64 artifactSize = artifactInfo.size();
    stats.totalNanoseconds = totalTimer.elapsedNanoseconds();
    return ResolvedRenderPipelineResult::success(
        {{artifactInfo.absoluteFilePath(), static_cast<std::uint64_t>(std::max<qint64>(artifactSize, 0))}, stats});
}

}  // namespace flexraw::core::render
