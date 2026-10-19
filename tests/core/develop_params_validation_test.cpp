#include <array>
#include <cstddef>
#include <limits>

#include <gtest/gtest.h>

#include "develop_params_validation.h"

namespace flexraw::core::develop
{
namespace
{

struct ValidationCase
{
    DevelopParamsValidationIssue expectedIssue;
    void (*invalidate)(types::DevelopParams&);
};

TEST(DevelopParamsValidationTest, AcceptsDefaultParameters)
{
    EXPECT_FALSE(findDevelopParamsValidationIssue(types::DevelopParams{}).has_value());
}

TEST(DevelopParamsValidationTest, IdentifiesEveryValidationCategory)
{
    const std::array<ValidationCase, 20> cases{{
        {DevelopParamsValidationIssue::ExposureNotFinite,
         [](types::DevelopParams& params) { params.exposureEv = std::numeric_limits<float>::infinity(); }},
        {DevelopParamsValidationIssue::ContrastOutOfRange,
         [](types::DevelopParams& params) { params.contrast = 1.1F; }},
        {DevelopParamsValidationIssue::HighlightsOutOfRange,
         [](types::DevelopParams& params) { params.highlights = -1.1F; }},
        {DevelopParamsValidationIssue::ShadowsOutOfRange,
         [](types::DevelopParams& params) { params.shadows = std::numeric_limits<float>::quiet_NaN(); }},
        {DevelopParamsValidationIssue::WhitesOutOfRange, [](types::DevelopParams& params) { params.whites = 1.1F; }},
        {DevelopParamsValidationIssue::BlacksOutOfRange, [](types::DevelopParams& params) { params.blacks = -1.1F; }},
        {DevelopParamsValidationIssue::SaturationOutOfRange,
         [](types::DevelopParams& params) { params.saturation = 1.1F; }},
        {DevelopParamsValidationIssue::VibranceOutOfRange,
         [](types::DevelopParams& params) { params.vibrance = -1.1F; }},
        {DevelopParamsValidationIssue::WhiteBalanceModeInvalid,
         [](types::DevelopParams& params) { params.whiteBalanceMode = static_cast<types::WhiteBalanceMode>(99); }},
        {DevelopParamsValidationIssue::WhiteBalanceTemperatureOutOfRange,
         [](types::DevelopParams& params) { params.whiteBalanceTemperatureKelvin = 1999.0F; }},
        {DevelopParamsValidationIssue::WhiteBalanceTintOutOfRange,
         [](types::DevelopParams& params) { params.whiteBalanceTint = 1.1F; }},
        {DevelopParamsValidationIssue::ClarityOutOfRange, [](types::DevelopParams& params) { params.clarity = -1.1F; }},
        {DevelopParamsValidationIssue::DehazeOutOfRange, [](types::DevelopParams& params) { params.dehaze = 1.1F; }},
        {DevelopParamsValidationIssue::SharpeningAmountOutOfRange,
         [](types::DevelopParams& params) { params.sharpeningAmount = -1.1F; }},
        {DevelopParamsValidationIssue::SharpeningRadiusOutOfRange,
         [](types::DevelopParams& params) { params.sharpeningRadius = 3.1F; }},
        {DevelopParamsValidationIssue::SharpeningDetailOutOfRange,
         [](types::DevelopParams& params) { params.sharpeningDetail = 1.1F; }},
        {DevelopParamsValidationIssue::SharpeningMaskingOutOfRange,
         [](types::DevelopParams& params) { params.sharpeningMasking = -0.1F; }},
        {DevelopParamsValidationIssue::NoiseReductionOutOfRange,
         [](types::DevelopParams& params) { params.colorNoiseReduction = 1.1F; }},
        {DevelopParamsValidationIssue::ToneCurveOutOfRange,
         [](types::DevelopParams& params) { params.toneCurveHighlights = 1.1F; }},
        {DevelopParamsValidationIssue::PointCurveOutOfRange,
         [](types::DevelopParams& params) { params.pointCurveMidtones = std::numeric_limits<float>::infinity(); }},
    }};

    for (std::size_t index = 0; index < cases.size(); ++index)
    {
        types::DevelopParams params;
        cases[index].invalidate(params);
        const std::optional<DevelopParamsValidationIssue> issue = findDevelopParamsValidationIssue(params);

        ASSERT_TRUE(issue.has_value()) << "case=" << index;
        EXPECT_EQ(cases[index].expectedIssue, *issue) << "case=" << index;
    }
}

TEST(DevelopParamsValidationTest, ReportsFirstIssueInCompatibilityOrder)
{
    types::DevelopParams params;
    params.exposureEv = std::numeric_limits<float>::infinity();
    params.contrast = 1.1F;
    const std::optional<DevelopParamsValidationIssue> issue = findDevelopParamsValidationIssue(params);

    ASSERT_TRUE(issue.has_value());
    EXPECT_EQ(DevelopParamsValidationIssue::ExposureNotFinite, *issue);
}

}  // namespace
}  // namespace flexraw::core::develop
