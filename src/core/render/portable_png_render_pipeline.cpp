#include "portable_png_render_pipeline.h"

#include <optional>
#include <utility>

#include "owned_rgba8_image.h"

namespace flexraw::core::render
{
namespace
{

// 목적: pipeline stage와 module failure를 결합한 실패 결과 생성
// 입력: stage: 실패한 처리 단계, cause: module 또는 allocation 오류
// 출력: stage identity를 보존한 render 실패
template<typename Cause>
[[nodiscard]] PortablePngRenderResult makeFailure(const PortablePngRenderStage stage, Cause cause)
{
    return PortablePngRenderResult::failure(PortablePngRenderError{stage, std::move(cause)});
}

// 목적: source geometry와 같은 tight RGBA8 destination allocation
// 입력: source: geometry를 제공할 current processing owner
// 출력: allocated owner 또는 geometry allocation 실패
[[nodiscard]] std::optional<image::OwnedRgba8Image> allocateLike(const image::OwnedRgba8Image& source)
{
    return image::OwnedRgba8Image::allocate(source.width(), source.height());
}

}  // namespace

PortablePngRenderResult renderRawToPortablePng(const PortablePngRenderRequest& request)
{
    raw::RawDecodeResult decoded = raw::decodeRawRaster8(request.inputPath);
    if (decoded.hasError())
    {
        return makeFailure(PortablePngRenderStage::Decode, decoded.error());
    }

    raw::RawRgba8BridgeResult bridged = raw::consumeRawRasterToRgba8(std::move(decoded.value()));
    if (bridged.hasError())
    {
        return makeFailure(PortablePngRenderStage::SourceBridge, bridged.error());
    }
    image::OwnedRgba8Image current = std::move(bridged.value());

    std::optional<image::OwnedRgba8Image> developed = allocateLike(current);
    if (!developed.has_value())
    {
        return makeFailure(PortablePngRenderStage::Develop, PortablePngRenderInternalErrorCode::ImageAllocationFailed);
    }
    const develop::DevelopProcessingResult developResult =
        develop::applyDevelop(current.constView(), developed->mutableView(), request.developParams, 1);
    if (developResult.hasError())
    {
        return makeFailure(PortablePngRenderStage::Develop, developResult.error());
    }
    current = std::move(*developed);

    color::IccProfileDataResult srgbProfile = color::createSrgbIccProfileData();
    if (srgbProfile.hasError())
    {
        return makeFailure(PortablePngRenderStage::Color, srgbProfile.error());
    }
    std::optional<image::OwnedRgba8Image> colorOutput = allocateLike(current);
    if (!colorOutput.has_value())
    {
        return makeFailure(PortablePngRenderStage::Color, PortablePngRenderInternalErrorCode::ImageAllocationFailed);
    }
    const color::ColorProcessingResult colorResult = color::transformIccRgba8(
        current.constView(), colorOutput->mutableView(), srgbProfile.value(), srgbProfile.value());
    if (colorResult.hasError())
    {
        return makeFailure(PortablePngRenderStage::Color, colorResult.error());
    }
    current = std::move(*colorOutput);

    export_::PngEncodingResult encoded = export_::encodePng8Srgb(current.constView());
    if (encoded.hasError())
    {
        return makeFailure(PortablePngRenderStage::Encode, encoded.error());
    }

    export_::PngArtifactPublishResult published =
        export_::publishPngArtifactNoReplace(std::move(encoded.value()), request.outputPath);
    if (published.hasError())
    {
        return makeFailure(PortablePngRenderStage::Publish, published.error());
    }
    return PortablePngRenderResult::success(std::move(published.value()));
}

}  // namespace flexraw::core::render
