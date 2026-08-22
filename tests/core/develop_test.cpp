#include <algorithm>
#include <cmath>
#include <limits>

#include <QColor>
#include <QImage>

#include <gtest/gtest.h>

#include "develop.h"

namespace flexraw::core::develop
{
namespace
{

// 목적: 지정 RGBA 값으로 1 pixel test image 생성
// 입력: color: 생성할 pixel 색상
// 출력: RGBA8888 format의 1 pixel QImage
[[nodiscard]] QImage createPixelImage(const QColor& color)
{
    QImage image(1, 1, QImage::Format_RGBA8888);
    image.setPixelColor(0, 0, color);
    return image;
}

// 목적: test reference에서 8-bit sRGB channel을 linear-light 값으로 변환
// 입력: channel: 0~255 범위의 sRGB channel 값
// 출력: IEC 61966-2-1 transfer function을 적용한 0~1 linear-light 값
[[nodiscard]] float referenceSrgbChannelToLinear(int channel)
{
    const float encodedValue = static_cast<float>(channel) / 255.0F;
    if (encodedValue <= 0.04045F)
    {
        return encodedValue / 12.92F;
    }
    return std::pow((encodedValue + 0.055F) / 1.055F, 2.4F);
}

// 목적: test reference에서 linear-light 값을 8-bit sRGB channel로 변환
// 입력: linearValue: exposure가 적용된 linear-light 값
// 출력: IEC 61966-2-1 transfer function과 clamp/round를 적용한 channel 값
[[nodiscard]] int referenceLinearToSrgbChannel(float linearValue)
{
    const float encodedValue =
        linearValue <= 0.0031308F ? linearValue * 12.92F : 1.055F * std::pow(linearValue, 1.0F / 2.4F) - 0.055F;
    return static_cast<int>(std::lround(std::clamp(encodedValue, 0.0F, 1.0F) * 255.0F));
}

TEST(Develop, PreservesImageForZeroExposure)
{
    const QImage source = createPixelImage(QColor{128, 64, 32, 90});
    const types::DevelopParams params{};

    const DevelopImageResult result = applyDevelop(source, params);

    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(source.pixelColor(0, 0), result.value().pixelColor(0, 0));
}

TEST(Develop, AppliesExposureInLinearLight)
{
    const QImage source = createPixelImage(QColor{128, 128, 128, 90});
    const DevelopImageResult brighter = applyDevelop(source, types::DevelopParams{1.0F});
    const DevelopImageResult darker = applyDevelop(source, types::DevelopParams{-1.0F});

    ASSERT_TRUE(brighter.hasValue());
    ASSERT_TRUE(darker.hasValue());
    EXPECT_EQ(176, brighter.value().pixelColor(0, 0).red());
    EXPECT_EQ(92, darker.value().pixelColor(0, 0).red());
    EXPECT_EQ(90, brighter.value().pixelColor(0, 0).alpha());
    EXPECT_EQ(90, darker.value().pixelColor(0, 0).alpha());
}

TEST(Develop, PreservesExhaustiveEightBitSrgbTransferDuringExposure)
{
    constexpr int ChannelValueCount = 256;
    constexpr float ExposureEv = 0.35F;
    QImage source(ChannelValueCount, 1, QImage::Format_RGBA8888);
    uchar* sourcePixels = source.scanLine(0);
    for (int channel = 0; channel < ChannelValueCount; ++channel)
    {
        uchar* pixel = sourcePixels + channel * 4;
        pixel[0] = static_cast<uchar>(channel);
        pixel[1] = static_cast<uchar>(channel);
        pixel[2] = static_cast<uchar>(channel);
        pixel[3] = static_cast<uchar>(255 - channel);
    }

    types::DevelopParams params;
    params.exposureEv = ExposureEv;
    const DevelopImageResult result = applyDevelop(source, params);

    ASSERT_TRUE(result.hasValue());
    const uchar* resultPixels = result.value().constScanLine(0);
    const float exposureScale = std::exp2(ExposureEv);
    for (int channel = 0; channel < ChannelValueCount; ++channel)
    {
        const int expected = referenceLinearToSrgbChannel(referenceSrgbChannelToLinear(channel) * exposureScale);
        const uchar* pixel = resultPixels + channel * 4;
        EXPECT_EQ(expected, pixel[0]) << "red channel=" << channel;
        EXPECT_EQ(expected, pixel[1]) << "green channel=" << channel;
        EXPECT_EQ(expected, pixel[2]) << "blue channel=" << channel;
        EXPECT_EQ(255 - channel, pixel[3]) << "alpha channel=" << channel;
    }
}

TEST(Develop, ClipsOverexposedChannels)
{
    const QImage source = createPixelImage(QColor{255, 64, 0, 255});
    const DevelopImageResult result = applyDevelop(source, types::DevelopParams{4.0F});

    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(255, result.value().pixelColor(0, 0).red());
    EXPECT_EQ(0, result.value().pixelColor(0, 0).blue());
}

TEST(Develop, AdjustsContrastAroundLinearMiddleGray)
{
    const QImage shadows = createPixelImage(QColor{64, 64, 64, 90});
    const QImage highlights = createPixelImage(QColor{192, 192, 192, 90});
    const types::DevelopParams higherContrast{0.0F, 0.5F};
    const types::DevelopParams lowerContrast{0.0F, -0.5F};

    const DevelopImageResult darkerShadows = applyDevelop(shadows, higherContrast);
    const DevelopImageResult brighterHighlights = applyDevelop(highlights, higherContrast);
    const DevelopImageResult lighterShadows = applyDevelop(shadows, lowerContrast);
    const DevelopImageResult darkerHighlights = applyDevelop(highlights, lowerContrast);

    ASSERT_TRUE(darkerShadows.hasValue());
    ASSERT_TRUE(brighterHighlights.hasValue());
    ASSERT_TRUE(lighterShadows.hasValue());
    ASSERT_TRUE(darkerHighlights.hasValue());
    EXPECT_LT(darkerShadows.value().pixelColor(0, 0).red(), shadows.pixelColor(0, 0).red());
    EXPECT_GT(brighterHighlights.value().pixelColor(0, 0).red(), highlights.pixelColor(0, 0).red());
    EXPECT_GT(lighterShadows.value().pixelColor(0, 0).red(), shadows.pixelColor(0, 0).red());
    EXPECT_LT(darkerHighlights.value().pixelColor(0, 0).red(), highlights.pixelColor(0, 0).red());
    EXPECT_EQ(90, darkerShadows.value().pixelColor(0, 0).alpha());
}

TEST(Develop, AdjustsShadowsWithoutChangingHighlights)
{
    const QImage shadows = createPixelImage(QColor{32, 32, 32, 90});
    const QImage highlights = createPixelImage(QColor{224, 224, 224, 90});
    const DevelopImageResult raisedShadows = applyDevelop(shadows, types::DevelopParams{0.0F, 0.0F, 0.0F, 0.5F});
    const DevelopImageResult loweredShadows = applyDevelop(shadows, types::DevelopParams{0.0F, 0.0F, 0.0F, -0.5F});
    const DevelopImageResult unchangedHighlights =
        applyDevelop(highlights, types::DevelopParams{0.0F, 0.0F, 0.0F, 0.5F});

    ASSERT_TRUE(raisedShadows.hasValue());
    ASSERT_TRUE(loweredShadows.hasValue());
    ASSERT_TRUE(unchangedHighlights.hasValue());
    EXPECT_GT(raisedShadows.value().pixelColor(0, 0).red(), shadows.pixelColor(0, 0).red());
    EXPECT_LT(loweredShadows.value().pixelColor(0, 0).red(), shadows.pixelColor(0, 0).red());
    EXPECT_EQ(highlights.pixelColor(0, 0), unchangedHighlights.value().pixelColor(0, 0));
    EXPECT_EQ(90, raisedShadows.value().pixelColor(0, 0).alpha());
}

TEST(Develop, AdjustsHighlightsWithoutChangingShadows)
{
    const QImage shadows = createPixelImage(QColor{32, 32, 32, 90});
    const QImage highlights = createPixelImage(QColor{224, 224, 224, 90});
    const DevelopImageResult raisedHighlights = applyDevelop(highlights, types::DevelopParams{0.0F, 0.0F, 0.5F, 0.0F});
    const DevelopImageResult loweredHighlights =
        applyDevelop(highlights, types::DevelopParams{0.0F, 0.0F, -0.5F, 0.0F});
    const DevelopImageResult unchangedShadows = applyDevelop(shadows, types::DevelopParams{0.0F, 0.0F, 0.5F, 0.0F});

    ASSERT_TRUE(raisedHighlights.hasValue());
    ASSERT_TRUE(loweredHighlights.hasValue());
    ASSERT_TRUE(unchangedShadows.hasValue());
    EXPECT_GT(raisedHighlights.value().pixelColor(0, 0).red(), highlights.pixelColor(0, 0).red());
    EXPECT_LT(loweredHighlights.value().pixelColor(0, 0).red(), highlights.pixelColor(0, 0).red());
    EXPECT_EQ(shadows.pixelColor(0, 0), unchangedShadows.value().pixelColor(0, 0));
    EXPECT_EQ(90, loweredHighlights.value().pixelColor(0, 0).alpha());
}

TEST(Develop, AdjustsBlacksWithoutChangingWhites)
{
    const QImage blacks = createPixelImage(QColor{16, 16, 16, 90});
    const QImage whites = createPixelImage(QColor{240, 240, 240, 90});
    const DevelopImageResult raisedBlacks =
        applyDevelop(blacks, types::DevelopParams{0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.5F});
    const DevelopImageResult loweredBlacks =
        applyDevelop(blacks, types::DevelopParams{0.0F, 0.0F, 0.0F, 0.0F, 0.0F, -0.5F});
    const DevelopImageResult unchangedWhites =
        applyDevelop(whites, types::DevelopParams{0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.5F});

    ASSERT_TRUE(raisedBlacks.hasValue());
    ASSERT_TRUE(loweredBlacks.hasValue());
    ASSERT_TRUE(unchangedWhites.hasValue());
    EXPECT_GT(raisedBlacks.value().pixelColor(0, 0).red(), blacks.pixelColor(0, 0).red());
    EXPECT_LT(loweredBlacks.value().pixelColor(0, 0).red(), blacks.pixelColor(0, 0).red());
    EXPECT_EQ(whites.pixelColor(0, 0), unchangedWhites.value().pixelColor(0, 0));
    EXPECT_EQ(90, raisedBlacks.value().pixelColor(0, 0).alpha());
}

TEST(Develop, AdjustsWhitesWithoutChangingBlacks)
{
    const QImage blacks = createPixelImage(QColor{16, 16, 16, 90});
    const QImage whites = createPixelImage(QColor{240, 240, 240, 90});
    const DevelopImageResult raisedWhites =
        applyDevelop(whites, types::DevelopParams{0.0F, 0.0F, 0.0F, 0.0F, 0.5F, 0.0F});
    const DevelopImageResult loweredWhites =
        applyDevelop(whites, types::DevelopParams{0.0F, 0.0F, 0.0F, 0.0F, -0.5F, 0.0F});
    const DevelopImageResult unchangedBlacks =
        applyDevelop(blacks, types::DevelopParams{0.0F, 0.0F, 0.0F, 0.0F, 0.5F, 0.0F});

    ASSERT_TRUE(raisedWhites.hasValue());
    ASSERT_TRUE(loweredWhites.hasValue());
    ASSERT_TRUE(unchangedBlacks.hasValue());
    EXPECT_GT(raisedWhites.value().pixelColor(0, 0).red(), whites.pixelColor(0, 0).red());
    EXPECT_LT(loweredWhites.value().pixelColor(0, 0).red(), whites.pixelColor(0, 0).red());
    EXPECT_EQ(blacks.pixelColor(0, 0), unchangedBlacks.value().pixelColor(0, 0));
    EXPECT_EQ(90, loweredWhites.value().pixelColor(0, 0).alpha());
}

TEST(Develop, AdjustsSaturationAroundLinearLuminance)
{
    const QImage source = createPixelImage(QColor{160, 96, 32, 90});
    const DevelopImageResult desaturated =
        applyDevelop(source, types::DevelopParams{0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, -1.0F});
    const DevelopImageResult saturated =
        applyDevelop(source, types::DevelopParams{0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.5F});

    ASSERT_TRUE(desaturated.hasValue());
    ASSERT_TRUE(saturated.hasValue());
    const QColor grayscale = desaturated.value().pixelColor(0, 0);
    const QColor moreSaturated = saturated.value().pixelColor(0, 0);
    EXPECT_EQ(grayscale.red(), grayscale.green());
    EXPECT_EQ(grayscale.green(), grayscale.blue());
    EXPECT_GT(moreSaturated.red(), source.pixelColor(0, 0).red());
    EXPECT_LT(moreSaturated.green(), source.pixelColor(0, 0).green());
    EXPECT_LT(moreSaturated.blue(), source.pixelColor(0, 0).blue());
    EXPECT_EQ(90, moreSaturated.alpha());
}

TEST(Develop, PreservesNeutralGrayForSaturation)
{
    const QImage source = createPixelImage(QColor{128, 128, 128, 90});
    const DevelopImageResult result =
        applyDevelop(source, types::DevelopParams{0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.75F});

    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(source.pixelColor(0, 0), result.value().pixelColor(0, 0));
}

TEST(Develop, PrioritizesMutedColorsForPositiveVibrance)
{
    const QImage mutedSource = createPixelImage(QColor{128, 112, 96, 90});
    const QImage saturatedSource = createPixelImage(QColor{160, 96, 32, 90});
    const types::DevelopParams vibrance{0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.75F};

    const DevelopImageResult mutedResult = applyDevelop(mutedSource, vibrance);
    const DevelopImageResult saturatedResult = applyDevelop(saturatedSource, vibrance);

    ASSERT_TRUE(mutedResult.hasValue());
    ASSERT_TRUE(saturatedResult.hasValue());
    const QColor mutedColor = mutedResult.value().pixelColor(0, 0);
    const QColor saturatedColor = saturatedResult.value().pixelColor(0, 0);
    EXPECT_GT(mutedColor.red(), mutedSource.pixelColor(0, 0).red());
    EXPECT_LT(mutedColor.blue(), mutedSource.pixelColor(0, 0).blue());
    EXPECT_GT(mutedColor.red() - mutedSource.pixelColor(0, 0).red(),
              saturatedColor.red() - saturatedSource.pixelColor(0, 0).red());
    EXPECT_EQ(90, mutedColor.alpha());
}

TEST(Develop, ReducesChromaForNegativeVibrance)
{
    const QImage source = createPixelImage(QColor{160, 96, 32, 90});
    const DevelopImageResult result =
        applyDevelop(source, types::DevelopParams{0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, -0.5F});

    ASSERT_TRUE(result.hasValue());
    const QColor resultColor = result.value().pixelColor(0, 0);
    EXPECT_LT(resultColor.red(), source.pixelColor(0, 0).red());
    EXPECT_GT(resultColor.blue(), source.pixelColor(0, 0).blue());
    EXPECT_EQ(90, resultColor.alpha());
}

TEST(Develop, AppliesCustomWhiteBalanceWithoutChangingAlpha)
{
    const QImage source = createPixelImage(QColor{128, 128, 128, 90});
    types::DevelopParams params;
    params.whiteBalanceMode = types::WhiteBalanceMode::Custom;
    params.whiteBalanceTemperatureKelvin = 3200.0F;
    params.whiteBalanceTint = 0.4F;

    const DevelopImageResult result = applyDevelop(source, params);

    ASSERT_TRUE(result.hasValue());
    const QColor adjusted = result.value().pixelColor(0, 0);
    EXPECT_EQ(adjusted.red(), source.pixelColor(0, 0).red());
    EXPECT_LT(adjusted.green(), source.pixelColor(0, 0).green());
    EXPECT_LT(adjusted.blue(), source.pixelColor(0, 0).blue());
    EXPECT_EQ(90, adjusted.alpha());
}

TEST(Develop, AdjustsMidtoneLocalContrastForClarity)
{
    QImage source(QSize(3, 3), QImage::Format_RGBA8888);
    source.fill(QColor(128, 128, 128, 90));
    source.setPixelColor(1, 1, QColor(160, 160, 160, 90));
    types::DevelopParams positiveClarity;
    positiveClarity.clarity = 1.0F;
    types::DevelopParams negativeClarity;
    negativeClarity.clarity = -1.0F;

    const DevelopImageResult positiveResult = applyDevelop(source, positiveClarity);
    const DevelopImageResult negativeResult = applyDevelop(source, negativeClarity);

    ASSERT_TRUE(positiveResult.hasValue());
    ASSERT_TRUE(negativeResult.hasValue());
    EXPECT_GT(positiveResult.value().pixelColor(1, 1).red(), source.pixelColor(1, 1).red());
    EXPECT_LT(negativeResult.value().pixelColor(1, 1).red(), source.pixelColor(1, 1).red());
    EXPECT_EQ(90, positiveResult.value().pixelColor(1, 1).alpha());
}

TEST(Develop, IncreasesContrastAndChromaForPositiveDehaze)
{
    const QImage source = createPixelImage(QColor{160, 128, 96, 90});
    types::DevelopParams params;
    params.dehaze = 1.0F;

    const DevelopImageResult result = applyDevelop(source, params);

    ASSERT_TRUE(result.hasValue());
    const QColor adjusted = result.value().pixelColor(0, 0);
    EXPECT_GT(adjusted.red() - adjusted.blue(), source.pixelColor(0, 0).red() - source.pixelColor(0, 0).blue());
    EXPECT_GT(adjusted.red(), source.pixelColor(0, 0).red());
    EXPECT_EQ(90, adjusted.alpha());
}

TEST(Develop, AdjustsSelectedToneCurveRegions)
{
    const QImage shadows = createPixelImage(QColor{32, 32, 32, 90});
    const QImage highlights = createPixelImage(QColor{224, 224, 224, 90});
    types::DevelopParams shadowCurve;
    shadowCurve.toneCurveShadows = 1.0F;
    types::DevelopParams highlightCurve;
    highlightCurve.toneCurveHighlights = -1.0F;

    const DevelopImageResult shadowResult = applyDevelop(shadows, shadowCurve);
    const DevelopImageResult highlightResult = applyDevelop(highlights, highlightCurve);

    ASSERT_TRUE(shadowResult.hasValue());
    ASSERT_TRUE(highlightResult.hasValue());
    EXPECT_GT(shadowResult.value().pixelColor(0, 0).red(), shadows.pixelColor(0, 0).red());
    EXPECT_LT(highlightResult.value().pixelColor(0, 0).red(), highlights.pixelColor(0, 0).red());
    EXPECT_EQ(90, shadowResult.value().pixelColor(0, 0).alpha());
}

TEST(Develop, InterpolatesPointCurveAnchors)
{
    const QImage source = createPixelImage(QColor{188, 188, 188, 90});
    types::DevelopParams params;
    params.pointCurveMidtones = 1.0F;

    const DevelopImageResult result = applyDevelop(source, params);

    ASSERT_TRUE(result.hasValue());
    EXPECT_GT(result.value().pixelColor(0, 0).red(), source.pixelColor(0, 0).red());
    EXPECT_EQ(90, result.value().pixelColor(0, 0).alpha());
}

TEST(Develop, ReducesLuminanceAndColorNoiseIndependently)
{
    QImage luminanceSource(QSize(3, 3), QImage::Format_RGBA8888);
    luminanceSource.fill(QColor(128, 128, 128, 90));
    luminanceSource.setPixelColor(1, 1, QColor(160, 160, 160, 90));
    types::DevelopParams luminanceParams;
    luminanceParams.luminanceNoiseReduction = 1.0F;

    QImage colorSource(QSize(3, 3), QImage::Format_RGBA8888);
    colorSource.fill(QColor(128, 128, 128, 90));
    colorSource.setPixelColor(1, 1, QColor(160, 128, 96, 90));
    types::DevelopParams colorParams;
    colorParams.colorNoiseReduction = 1.0F;

    const DevelopImageResult luminanceResult = applyDevelop(luminanceSource, luminanceParams);
    const DevelopImageResult colorResult = applyDevelop(colorSource, colorParams);

    ASSERT_TRUE(luminanceResult.hasValue());
    ASSERT_TRUE(colorResult.hasValue());
    EXPECT_LT(luminanceResult.value().pixelColor(1, 1).red(), luminanceSource.pixelColor(1, 1).red());
    EXPECT_LT(colorResult.value().pixelColor(1, 1).red() - colorResult.value().pixelColor(1, 1).blue(),
              colorSource.pixelColor(1, 1).red() - colorSource.pixelColor(1, 1).blue());
    EXPECT_EQ(90, colorResult.value().pixelColor(1, 1).alpha());
}

TEST(Develop, AdjustsLocalDetailForSharpeningAmount)
{
    QImage source(QSize(3, 3), QImage::Format_RGBA8888);
    source.fill(QColor(128, 128, 128, 90));
    source.setPixelColor(1, 1, QColor(160, 160, 160, 90));
    types::DevelopParams sharpen;
    sharpen.sharpeningAmount = 1.0F;
    types::DevelopParams soften;
    soften.sharpeningAmount = -1.0F;

    const DevelopImageResult sharpened = applyDevelop(source, sharpen);
    const DevelopImageResult softened = applyDevelop(source, soften);

    ASSERT_TRUE(sharpened.hasValue());
    ASSERT_TRUE(softened.hasValue());
    EXPECT_GT(sharpened.value().pixelColor(1, 1).red(), source.pixelColor(1, 1).red());
    EXPECT_LT(softened.value().pixelColor(1, 1).red(), source.pixelColor(1, 1).red());
    EXPECT_EQ(90, sharpened.value().pixelColor(1, 1).alpha());
}

TEST(Develop, AdjustsSharpeningScaleAndDetailWithRadius)
{
    QImage source(QSize(7, 7), QImage::Format_RGBA8888);
    source.fill(QColor(128, 128, 128, 90));
    source.setPixelColor(3, 3, QColor(160, 160, 160, 90));
    types::DevelopParams broadDetail;
    broadDetail.sharpeningAmount = 1.0F;
    broadDetail.sharpeningRadius = 3.0F;
    broadDetail.sharpeningDetail = -1.0F;
    types::DevelopParams fineDetail = broadDetail;
    fineDetail.sharpeningDetail = 1.0F;

    const DevelopImageResult broadResult = applyDevelop(source, broadDetail);
    const DevelopImageResult fineResult = applyDevelop(source, fineDetail);

    ASSERT_TRUE(broadResult.hasValue());
    ASSERT_TRUE(fineResult.hasValue());
    EXPECT_GT(broadResult.value().pixelColor(3, 3).red(), source.pixelColor(3, 3).red());
    EXPECT_NE(broadResult.value().pixelColor(3, 3).red(), fineResult.value().pixelColor(3, 3).red());
    EXPECT_EQ(90, broadResult.value().pixelColor(3, 3).alpha());
}

TEST(Develop, MasksSharpeningAwayFromLowContrastPixels)
{
    QImage source(QSize(5, 5), QImage::Format_RGBA8888);
    source.fill(QColor(128, 128, 128, 90));
    source.setPixelColor(2, 2, QColor(160, 160, 160, 90));
    types::DevelopParams unmasked;
    unmasked.sharpeningAmount = 1.0F;
    types::DevelopParams masked = unmasked;
    masked.sharpeningMasking = 1.0F;

    const DevelopImageResult unmaskedResult = applyDevelop(source, unmasked);
    const DevelopImageResult maskedResult = applyDevelop(source, masked);

    ASSERT_TRUE(unmaskedResult.hasValue());
    ASSERT_TRUE(maskedResult.hasValue());
    EXPECT_GT(unmaskedResult.value().pixelColor(2, 2).red(), maskedResult.value().pixelColor(2, 2).red());
    EXPECT_GT(maskedResult.value().pixelColor(2, 2).red(), source.pixelColor(2, 2).red());
    EXPECT_EQ(90, maskedResult.value().pixelColor(2, 2).alpha());
}

TEST(Develop, RejectsNullImage)
{
    const DevelopImageResult result = applyDevelop(QImage{}, types::DevelopParams{});

    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(types::ErrorCode::InvalidArgument, result.error().code);
}

TEST(Develop, RejectsNonFiniteExposure)
{
    const QImage source = createPixelImage(QColor{128, 128, 128, 255});
    const DevelopImageResult result =
        applyDevelop(source, types::DevelopParams{std::numeric_limits<float>::infinity()});

    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(types::ErrorCode::InvalidArgument, result.error().code);
}

TEST(Develop, RejectsInvalidContrast)
{
    const QImage source = createPixelImage(QColor{128, 128, 128, 255});
    const DevelopImageResult outOfRange = applyDevelop(source, types::DevelopParams{0.0F, 1.1F});
    const DevelopImageResult nonFinite =
        applyDevelop(source, types::DevelopParams{0.0F, std::numeric_limits<float>::infinity()});

    ASSERT_TRUE(outOfRange.hasError());
    ASSERT_TRUE(nonFinite.hasError());
    EXPECT_EQ(types::ErrorCode::InvalidArgument, outOfRange.error().code);
    EXPECT_EQ(types::ErrorCode::InvalidArgument, nonFinite.error().code);
}

TEST(Develop, RejectsInvalidHighlightAndShadow)
{
    const QImage source = createPixelImage(QColor{128, 128, 128, 255});
    const DevelopImageResult invalidHighlights = applyDevelop(source, types::DevelopParams{0.0F, 0.0F, 1.1F, 0.0F});
    const DevelopImageResult invalidShadows =
        applyDevelop(source, types::DevelopParams{0.0F, 0.0F, 0.0F, std::numeric_limits<float>::infinity()});

    ASSERT_TRUE(invalidHighlights.hasError());
    ASSERT_TRUE(invalidShadows.hasError());
    EXPECT_EQ(types::ErrorCode::InvalidArgument, invalidHighlights.error().code);
    EXPECT_EQ(types::ErrorCode::InvalidArgument, invalidShadows.error().code);
}

TEST(Develop, RejectsInvalidWhiteAndBlack)
{
    const QImage source = createPixelImage(QColor{128, 128, 128, 255});
    const DevelopImageResult invalidWhites =
        applyDevelop(source, types::DevelopParams{0.0F, 0.0F, 0.0F, 0.0F, 1.1F, 0.0F});
    const DevelopImageResult invalidBlacks = applyDevelop(
        source, types::DevelopParams{0.0F, 0.0F, 0.0F, 0.0F, 0.0F, std::numeric_limits<float>::infinity()});

    ASSERT_TRUE(invalidWhites.hasError());
    ASSERT_TRUE(invalidBlacks.hasError());
    EXPECT_EQ(types::ErrorCode::InvalidArgument, invalidWhites.error().code);
    EXPECT_EQ(types::ErrorCode::InvalidArgument, invalidBlacks.error().code);
}

TEST(Develop, RejectsInvalidSaturation)
{
    const QImage source = createPixelImage(QColor{128, 64, 32, 255});
    const DevelopImageResult outOfRange =
        applyDevelop(source, types::DevelopParams{0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.1F});
    const DevelopImageResult nonFinite = applyDevelop(
        source, types::DevelopParams{0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, std::numeric_limits<float>::infinity()});

    ASSERT_TRUE(outOfRange.hasError());
    ASSERT_TRUE(nonFinite.hasError());
    EXPECT_EQ(types::ErrorCode::InvalidArgument, outOfRange.error().code);
    EXPECT_EQ(types::ErrorCode::InvalidArgument, nonFinite.error().code);
}

TEST(Develop, RejectsInvalidVibrance)
{
    const QImage source = createPixelImage(QColor{128, 64, 32, 255});
    const DevelopImageResult outOfRange =
        applyDevelop(source, types::DevelopParams{0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.1F});
    const DevelopImageResult nonFinite = applyDevelop(
        source, types::DevelopParams{0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, std::numeric_limits<float>::infinity()});

    ASSERT_TRUE(outOfRange.hasError());
    ASSERT_TRUE(nonFinite.hasError());
    EXPECT_EQ(types::ErrorCode::InvalidArgument, outOfRange.error().code);
    EXPECT_EQ(types::ErrorCode::InvalidArgument, nonFinite.error().code);
}

TEST(Develop, RejectsInvalidWhiteBalanceValues)
{
    const QImage source = createPixelImage(QColor{128, 64, 32, 255});
    types::DevelopParams invalidTemperature;
    invalidTemperature.whiteBalanceTemperatureKelvin = 1999.0F;
    types::DevelopParams invalidTint;
    invalidTint.whiteBalanceTint = 1.1F;

    const DevelopImageResult temperatureResult = applyDevelop(source, invalidTemperature);
    const DevelopImageResult tintResult = applyDevelop(source, invalidTint);

    ASSERT_TRUE(temperatureResult.hasError());
    ASSERT_TRUE(tintResult.hasError());
    EXPECT_EQ(types::ErrorCode::InvalidArgument, temperatureResult.error().code);
    EXPECT_EQ(types::ErrorCode::InvalidArgument, tintResult.error().code);
}

TEST(Develop, RejectsInvalidClarity)
{
    const QImage source = createPixelImage(QColor{128, 64, 32, 255});
    types::DevelopParams params;
    params.clarity = 1.1F;

    const DevelopImageResult result = applyDevelop(source, params);

    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(types::ErrorCode::InvalidArgument, result.error().code);
}

TEST(Develop, RejectsInvalidDehaze)
{
    const QImage source = createPixelImage(QColor{128, 64, 32, 255});
    types::DevelopParams params;
    params.dehaze = std::numeric_limits<float>::infinity();

    const DevelopImageResult result = applyDevelop(source, params);

    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(types::ErrorCode::InvalidArgument, result.error().code);
}

TEST(Develop, RejectsInvalidSharpeningRadiusAndDetail)
{
    const QImage source = createPixelImage(QColor{128, 64, 32, 255});
    types::DevelopParams invalidRadius;
    invalidRadius.sharpeningRadius = 3.1F;
    types::DevelopParams invalidDetail;
    invalidDetail.sharpeningDetail = std::numeric_limits<float>::infinity();

    const DevelopImageResult radiusResult = applyDevelop(source, invalidRadius);
    const DevelopImageResult detailResult = applyDevelop(source, invalidDetail);

    ASSERT_TRUE(radiusResult.hasError());
    ASSERT_TRUE(detailResult.hasError());
    EXPECT_EQ(types::ErrorCode::InvalidArgument, radiusResult.error().code);
    EXPECT_EQ(types::ErrorCode::InvalidArgument, detailResult.error().code);
}

TEST(Develop, RejectsInvalidSharpeningMasking)
{
    const QImage source = createPixelImage(QColor{128, 64, 32, 255});
    types::DevelopParams params;
    params.sharpeningMasking = -0.1F;

    const DevelopImageResult result = applyDevelop(source, params);

    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(types::ErrorCode::InvalidArgument, result.error().code);
}

TEST(Develop, RejectsInvalidNoiseReduction)
{
    const QImage source = createPixelImage(QColor{128, 64, 32, 255});
    types::DevelopParams luminanceParams;
    luminanceParams.luminanceNoiseReduction = 1.1F;
    types::DevelopParams colorParams;
    colorParams.colorNoiseReduction = std::numeric_limits<float>::infinity();

    const DevelopImageResult luminanceResult = applyDevelop(source, luminanceParams);
    const DevelopImageResult colorResult = applyDevelop(source, colorParams);

    ASSERT_TRUE(luminanceResult.hasError());
    ASSERT_TRUE(colorResult.hasError());
    EXPECT_EQ(types::ErrorCode::InvalidArgument, luminanceResult.error().code);
    EXPECT_EQ(types::ErrorCode::InvalidArgument, colorResult.error().code);
}

TEST(Develop, RejectsInvalidToneCurve)
{
    const QImage source = createPixelImage(QColor{128, 64, 32, 255});
    types::DevelopParams params;
    params.toneCurveLights = 1.1F;

    const DevelopImageResult result = applyDevelop(source, params);

    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(types::ErrorCode::InvalidArgument, result.error().code);
}

TEST(Develop, RejectsInvalidPointCurve)
{
    const QImage source = createPixelImage(QColor{128, 64, 32, 255});
    types::DevelopParams params;
    params.pointCurveWhite = std::numeric_limits<float>::infinity();

    const DevelopImageResult result = applyDevelop(source, params);

    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(types::ErrorCode::InvalidArgument, result.error().code);
}

}  // namespace
}  // namespace flexraw::core::develop
