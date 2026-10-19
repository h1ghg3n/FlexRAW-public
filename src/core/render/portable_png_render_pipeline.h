#pragma once

#include <cstdint>
#include <filesystem>
#include <variant>

#include "color_processing.h"
#include "develop_params.h"
#include "develop_processing.h"
#include "png_artifact_publisher.h"
#include "png_encoder.h"
#include "raw_processing.h"
#include "raw_rgba8_bridge.h"
#include "result.h"

namespace flexraw::core::render
{

enum class PortablePngRenderStage : std::uint8_t
{
    Decode,
    SourceBridge,
    Develop,
    Color,
    Encode,
    Publish,
};

enum class PortablePngRenderInternalErrorCode : std::uint8_t
{
    ImageAllocationFailed,
};

using PortablePngRenderCause = std::variant<PortablePngRenderInternalErrorCode,
                                            raw::RawDecodeError,
                                            raw::RawRgba8BridgeError,
                                            develop::DevelopProcessingError,
                                            color::ColorProcessingError,
                                            export_::PngEncodingError,
                                            export_::PngArtifactPublishError>;

struct PortablePngRenderError
{
    PortablePngRenderStage stage{PortablePngRenderStage::Decode};
    PortablePngRenderCause cause{PortablePngRenderInternalErrorCode::ImageAllocationFailed};
};

struct PortablePngRenderRequest
{
    std::filesystem::path inputPath;
    std::filesystem::path outputPath;
    types::DevelopParams developParams;
};

using PortablePngRenderResult = types::Result<export_::PngArtifact, PortablePngRenderError>;

// 목적: resolved local RAW를 Qt 없이 Develop/Color 처리하고 PNG artifact로 publish
// 입력: request: native input/output path와 neutral Develop recipe
// 출력: no-replace local artifact 또는 실패 stage와 neutral cause
[[nodiscard]] PortablePngRenderResult renderRawToPortablePng(const PortablePngRenderRequest& request);

}  // namespace flexraw::core::render
