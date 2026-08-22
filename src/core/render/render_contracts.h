#pragma once

#include <cstdint>

#include <QString>

#include "develop_params.h"
#include "error.h"
#include "export_options.h"
#include "render_stats.h"
#include "result.h"

namespace flexraw::core::render
{

struct ResolvedRenderRequest
{
    QString sourcePath;
    QString outputPath;
    types::DevelopParams developParams;
    export_::RasterExportOptions outputOptions;
};

struct RenderArtifact
{
    QString outputPath;
    std::uint64_t byteSize{0};
};

struct ResolvedRenderResult
{
    RenderArtifact artifact;
    measurement::RenderStats stats;
};

struct ResolvedRenderFailure
{
    types::CoreError cause;
    measurement::RenderStats stats;
};

using ResolvedRenderPipelineResult = types::Result<ResolvedRenderResult, ResolvedRenderFailure>;

}  // namespace flexraw::core::render
