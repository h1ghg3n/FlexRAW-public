#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include <gtest/gtest.h>

#include "develop_params_validation.h"
#include "develop_processing.h"
#include "hsl_processing.h"

namespace flexraw::core::develop
{
namespace
{

TEST(HslProcessingTest, MatchesAnalyticPrimaryColorAdjustments)
{
    types::HslParams params;
    params.channels[0].hue = 1.0F;
    std::array<std::uint8_t, 4> red{255, 0, 0, 19};
    detail::applyHslToSrgbPixel(red.data(), params);
    EXPECT_EQ(red, (std::array<std::uint8_t, 4>{255, 128, 0, 19}));
    params.channels[0].hue = -1.0F;
    red = {255, 0, 0, 19};
    detail::applyHslToSrgbPixel(red.data(), params);
    EXPECT_EQ(red, (std::array<std::uint8_t, 4>{255, 0, 128, 19}));
    params.channels[0] = {0.0F, -1.0F, 0.0F};
    red = {255, 0, 0, 19};
    detail::applyHslToSrgbPixel(red.data(), params);
    EXPECT_EQ(red, (std::array<std::uint8_t, 4>{128, 128, 128, 19}));
    params.channels[0] = {0.0F, 0.0F, -0.5F};
    red = {255, 0, 0, 19};
    detail::applyHslToSrgbPixel(red.data(), params);
    EXPECT_EQ(red, (std::array<std::uint8_t, 4>{128, 0, 0, 19}));
}

TEST(HslProcessingTest, BlendsOriginalHueWeightsAndClampsLightnessEndpoints)
{
    types::HslParams params;
    params.channels[0].hue = 1.0F;
    params.channels[1].hue = -1.0F;
    std::array<std::uint8_t, 4> midpoint{240, 60, 0, 7};
    detail::applyHslToSrgbPixel(midpoint.data(), params);
    EXPECT_EQ(midpoint, (std::array<std::uint8_t, 4>{240, 60, 0, 7}));
    params.channels[0].hue = 0.5F;
    params.channels[1].hue = 0.5F;
    detail::applyHslToSrgbPixel(midpoint.data(), params);
    EXPECT_EQ(midpoint, (std::array<std::uint8_t, 4>{240, 120, 0, 7}));
    for (const float adjustment : {-1.0F, 1.0F})
    {
        params = {};
        params.channels[0].luminance = adjustment;
        std::array<std::uint8_t, 4> red{255, 0, 0, 7};
        detail::applyHslToSrgbPixel(red.data(), params);
        const std::uint8_t expected = adjustment < 0.0F ? 0 : 255;
        EXPECT_EQ(red, (std::array<std::uint8_t, 4>{expected, expected, expected, 7}));
    }
}

TEST(HslProcessingTest, PreservesEveryNeutralAndUnselectedPrimary)
{
    types::HslParams params;
    params.channels[0] = {1.0F, -1.0F, 1.0F};
    for (int value = 0; value <= 255; ++value)
    {
        const auto gray = static_cast<std::uint8_t>(value);
        std::array<std::uint8_t, 4> pixel{gray, gray, gray, gray};
        const auto original = pixel;
        detail::applyHslToSrgbPixel(pixel.data(), params);
        EXPECT_EQ(pixel, original);
    }
    std::array<std::uint8_t, 4> green{0, 255, 0, 23};
    const auto original = green;
    detail::applyHslToSrgbPixel(green.data(), params);
    EXPECT_EQ(green, original);
}

TEST(HslProcessingTest, SelectsAllEightHueCenters)
{
    const std::array<std::array<std::uint8_t, 4>, 8> colors{{
        {240, 0, 0, 5},
        {240, 120, 0, 5},
        {240, 240, 0, 5},
        {0, 240, 0, 5},
        {0, 240, 240, 5},
        {0, 0, 240, 5},
        {120, 0, 240, 5},
        {240, 0, 240, 5},
    }};
    for (std::size_t index = 0; index < colors.size(); ++index)
    {
        types::HslParams params;
        params.channels[index].saturation = -1.0F;
        auto pixel = colors[index];
        detail::applyHslToSrgbPixel(pixel.data(), params);
        EXPECT_EQ(pixel, (std::array<std::uint8_t, 4>{120, 120, 120, 5})) << index;
    }
}

TEST(HslProcessingTest, IsContinuousAcrossRedWrapAndProtectsNearNeutral)
{
    types::HslParams params;
    params.channels[0] = {0.8F, -0.5F, 0.3F};
    std::array<std::uint8_t, 4> left{255, 0, 1, 42};
    std::array<std::uint8_t, 4> right{255, 1, 0, 42};
    detail::applyHslToSrgbPixel(left.data(), params);
    detail::applyHslToSrgbPixel(right.data(), params);
    for (std::size_t index = 0; index < 3; ++index)
    {
        EXPECT_LE(std::abs(static_cast<int>(left[index]) - static_cast<int>(right[index])), 2);
    }
    std::array<std::uint8_t, 4> neutral{128, 127, 127, 42};
    detail::applyHslToSrgbPixel(neutral.data(), params);
    EXPECT_EQ(neutral, (std::array<std::uint8_t, 4>{128, 127, 127, 42}));
}

TEST(HslProcessingTest, NeutralRecipeAndActiveRecipePreserveStrideAlphaAndInput)
{
    constexpr std::uint32_t Width = 7;
    constexpr std::uint32_t Height = 3;
    constexpr std::size_t SourceStride = Width * 4 + 5;
    constexpr std::size_t DestinationStride = Width * 4 + 9;
    std::vector<std::uint8_t> source(SourceStride * Height, 0xA5);
    for (std::uint32_t row = 0; row < Height; ++row)
    {
        for (std::uint32_t column = 0; column < Width; ++column)
        {
            const auto offset = row * SourceStride + column * 4;
            source[offset] = 240;
            source[offset + 1] = 0;
            source[offset + 2] = 0;
            source[offset + 3] = static_cast<std::uint8_t>(row * Width + column);
        }
    }
    const auto original = source;
    const auto input = image::ConstRgba8ImageView::create(Width, Height, SourceStride, source);
    ASSERT_TRUE(input.has_value());
    for (const bool active : {false, true})
    {
        std::vector<std::uint8_t> output(DestinationStride * Height, 0xBC);
        const auto destination = image::MutableRgba8ImageView::create(Width, Height, DestinationStride, output);
        ASSERT_TRUE(destination.has_value());
        types::DevelopParams params;
        if (active)
        {
            params.hsl.channels[0].saturation = -1.0F;
        }
        ASSERT_TRUE(applyDevelop(*input, *destination, params, 1).hasValue());
        EXPECT_EQ(maximumUsefulDevelopCpuPermits(1024, params), 1);
        for (std::uint32_t row = 0; row < Height; ++row)
        {
            for (std::uint32_t column = 0; column < Width; ++column)
            {
                const auto offset = row * DestinationStride + column * 4;
                EXPECT_EQ(output[offset], active ? 120 : 240);
                EXPECT_EQ(output[offset + 1], active ? 120 : 0);
                EXPECT_EQ(output[offset + 2], active ? 120 : 0);
                EXPECT_EQ(output[offset + 3], source[row * SourceStride + column * 4 + 3]);
            }
            for (std::size_t index = Width * 4; index < DestinationStride; ++index)
            {
                EXPECT_EQ(output[row * DestinationStride + index], 0xBC);
            }
        }
    }
    EXPECT_EQ(source, original);
}

TEST(HslProcessingTest, RejectsInvalidValuesBeforeWritingDestination)
{
    const std::array<float, 4> invalid{
        -1.01F, 1.01F, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()};
    using Member = float types::HslChannelAdjustment::*;
    const std::array<Member, 3> members{&types::HslChannelAdjustment::hue,
                                        &types::HslChannelAdjustment::saturation,
                                        &types::HslChannelAdjustment::luminance};
    std::array<std::uint8_t, 4> input{255, 0, 0, 255};
    const auto source = image::ConstRgba8ImageView::create(1, 1, 4, input);
    ASSERT_TRUE(source.has_value());
    for (std::size_t channel = 0; channel < 8; ++channel)
    {
        for (const auto member : members)
        {
            for (const float value : invalid)
            {
                types::DevelopParams params;
                params.hsl.channels[channel].*member = value;
                std::array<std::uint8_t, 4> output{11, 22, 33, 44};
                const auto original = output;
                const auto destination = image::MutableRgba8ImageView::create(1, 1, 4, output);
                ASSERT_TRUE(destination.has_value());
                EXPECT_EQ(findDevelopParamsValidationIssue(params), DevelopParamsValidationIssue::HslOutOfRange);
                const auto result = applyDevelop(*source, *destination, params, 1);
                ASSERT_TRUE(result.hasError());
                EXPECT_EQ(result.error().code, DevelopProcessingErrorCode::InvalidParameters);
                EXPECT_EQ(output, original);
            }
        }
    }
}

}  // namespace
}  // namespace flexraw::core::develop
