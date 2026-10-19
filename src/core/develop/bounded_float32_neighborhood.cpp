#include "bounded_float32_neighborhood.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#include "bounded_float32_neighborhood_detail.h"

namespace flexraw::core::develop
{
namespace
{

constexpr int RgbaChannelCount = 4;
constexpr int StripeRowCount = 64;
constexpr float RedLuminanceWeight = 0.2126F;
constexpr float GreenLuminanceWeight = 0.7152F;
constexpr float BlueLuminanceWeight = 0.0722F;
constexpr float MinimumLuminanceForScaling = 0.00001F;
constexpr std::size_t SrgbChannelValueCount = 256;
constexpr int MaximumSharpeningKernelRadius = 3;

using detail::Float32Region;
using detail::LinearRgb;

// 목적: 8-bit sRGB channel 전체의 linear-light lookup table 생성
// 입력: 없음
// 출력: channel 값을 index로 사용하는 256-entry table
[[nodiscard]] std::array<float, SrgbChannelValueCount> buildSrgbToLinearLookup()
{
    std::array<float, SrgbChannelValueCount> lookup{};
    for (std::size_t channel = 0; channel < lookup.size(); ++channel)
    {
        const float encoded = static_cast<float>(channel) / 255.0F;
        lookup[channel] = encoded <= 0.04045F ? encoded / 12.92F : std::pow((encoded + 0.055F) / 1.055F, 2.4F);
    }
    return lookup;
}

const std::array<float, SrgbChannelValueCount> SrgbToLinearLookup = buildSrgbToLinearLookup();

// 목적: linear-light channel을 normalized sRGB 값으로 변환
// 입력: linearValue — 변환할 linear-light 값
// 출력: normalized sRGB 값
[[nodiscard]] float linearToSrgb(float linearValue)
{
    return linearValue <= 0.0031308F ? linearValue * 12.92F : 1.055F * std::pow(linearValue, 1.0F / 2.4F) - 0.055F;
}

// 목적: normalized sRGB 값을 8-bit channel로 투영
// 입력: value — 변환할 normalized 값
// 출력: clamp와 반올림이 적용된 0~255 값
[[nodiscard]] unsigned char toChannelValue(float value)
{
    return static_cast<unsigned char>(std::clamp(value, 0.0F, 1.0F) * 255.0F + 0.5F);
}

// 목적: image geometry에 필요한 pixel 수를 overflow 없이 계산
// 입력: width/rowCount — region geometry
// 출력: row-major pixel 수
[[nodiscard]] std::size_t checkedPixelCount(int width, int rowCount)
{
    if (width <= 0 || rowCount <= 0)
    {
        throw std::invalid_argument("Float32 neighborhood region geometry must be positive.");
    }

    const std::size_t unsignedWidth = static_cast<std::size_t>(width);
    const std::size_t unsignedRows = static_cast<std::size_t>(rowCount);
    if (unsignedRows > std::numeric_limits<std::size_t>::max() / unsignedWidth)
    {
        throw std::length_error("Float32 neighborhood region geometry overflows size_t.");
    }
    return unsignedWidth * unsignedRows;
}

// 목적: global row 좌표를 사용하는 Float32 region pixel offset 계산
// 입력: region과 global row/column 좌표
// 출력: region vector의 pixel index
[[nodiscard]] std::size_t regionOffset(const Float32Region& region, int row, int column)
{
    return static_cast<std::size_t>(row - region.firstRow) * static_cast<std::size_t>(region.width) +
           static_cast<std::size_t>(column);
}

// 목적: immutable Float32 region pixel 조회
// 입력: region과 global row/column 좌표
// 출력: 해당 linear RGB pixel 참조
[[nodiscard]] const LinearRgb& regionPixel(const Float32Region& region, int row, int column)
{
    return region.pixels[regionOffset(region, row, column)];
}

// 목적: mutable Float32 region pixel 조회
// 입력: region과 global row/column 좌표
// 출력: 해당 linear RGB pixel 참조
[[nodiscard]] LinearRgb& regionPixel(Float32Region& region, int row, int column)
{
    return region.pixels[regionOffset(region, row, column)];
}

// 목적: RGBA8 source의 지정 global row 첫 주소 조회
// 입력: view와 global row
// 출력: source row byte pointer
[[nodiscard]] const unsigned char* sourceRow(const Rgba8NeighborhoodView& view, int row)
{
    return view.sourcePixels + static_cast<std::ptrdiff_t>(row) * view.sourceBytesPerLine;
}

// 목적: RGBA8 destination의 지정 global row 첫 주소 조회
// 입력: view와 global row
// 출력: destination row byte pointer
[[nodiscard]] unsigned char* destinationRow(const Rgba8NeighborhoodView& view, int row)
{
    return view.destinationPixels + static_cast<std::ptrdiff_t>(row) * view.destinationBytesPerLine;
}

// 목적: RGBA8 source의 bounded row 범위를 Float32 linear RGB region으로 변환
// 입력: view와 half-open global row 범위
// 출력: 요청 row만 보유한 Float32 region
[[nodiscard]] Float32Region makeSourceRegion(const Rgba8NeighborhoodView& view, int firstRow, int lastRow)
{
    Float32Region result;
    result.width = view.width;
    result.firstRow = firstRow;
    result.rowCount = lastRow - firstRow;
    result.pixels.resize(checkedPixelCount(result.width, result.rowCount));

    for (int row = firstRow; row < lastRow; ++row)
    {
        const unsigned char* pixels = sourceRow(view, row);
        for (int column = 0; column < view.width; ++column)
        {
            const unsigned char* pixel = pixels + column * RgbaChannelCount;
            regionPixel(result, row, column) = {
                SrgbToLinearLookup[pixel[0]],
                SrgbToLinearLookup[pixel[1]],
                SrgbToLinearLookup[pixel[2]],
            };
        }
    }
    return result;
}

// 목적: linear RGB pixel의 Rec.709 luminance 계산
// 입력: pixel — linear RGB 값
// 출력: linear-light luminance
[[nodiscard]] float luminance(const LinearRgb& pixel)
{
    return pixel.red * RedLuminanceWeight + pixel.green * GreenLuminanceWeight + pixel.blue * BlueLuminanceWeight;
}

// 목적: source halo에서 지정 row 범위의 Clarity 결과 계산
// 입력: source region, 전체 image 높이, half-open output row 범위와 Clarity 강도
// 출력: 중간 양자화가 없는 Float32 Clarity region
[[nodiscard]] Float32Region applyClarity(
    const Float32Region& source, int imageHeight, int firstRow, int lastRow, float clarity)
{
    Float32Region result;
    result.width = source.width;
    result.firstRow = firstRow;
    result.rowCount = lastRow - firstRow;
    result.pixels.resize(checkedPixelCount(result.width, result.rowCount));

    for (int row = firstRow; row < lastRow; ++row)
    {
        for (int column = 0; column < result.width; ++column)
        {
            const LinearRgb& sourcePixel = regionPixel(source, row, column);
            const float sourceLuminance = luminance(sourcePixel);
            float localLuminance = 0.0F;
            for (int rowOffset = -1; rowOffset <= 1; ++rowOffset)
            {
                const int sampleRow = std::clamp(row + rowOffset, 0, imageHeight - 1);
                for (int columnOffset = -1; columnOffset <= 1; ++columnOffset)
                {
                    const int sampleColumn = std::clamp(column + columnOffset, 0, result.width - 1);
                    localLuminance += luminance(regionPixel(source, sampleRow, sampleColumn));
                }
            }

            localLuminance /= 9.0F;
            const float midtoneWeight = 1.0F - std::abs(sourceLuminance * 2.0F - 1.0F);
            const float adjustedLuminance =
                std::clamp(sourceLuminance + (sourceLuminance - localLuminance) * clarity * midtoneWeight, 0.0F, 1.0F);
            const float luminanceScale =
                sourceLuminance > MinimumLuminanceForScaling ? adjustedLuminance / sourceLuminance : 1.0F;
            regionPixel(result, row, column) = {
                sourcePixel.red * luminanceScale,
                sourcePixel.green * luminanceScale,
                sourcePixel.blue * luminanceScale,
            };
        }
    }
    return result;
}

// 목적: linear Float32 region을 양자화 없이 normalized sRGB Float32로 변환
// 입력: source — linear RGB region
// 출력: geometry가 같은 encoded Float32 region
[[nodiscard]] Float32Region makeEncodedRegion(const Float32Region& source)
{
    Float32Region result;
    result.width = source.width;
    result.firstRow = source.firstRow;
    result.rowCount = source.rowCount;
    result.pixels.resize(source.pixels.size());
    for (int row = source.firstRow; row < source.firstRow + source.rowCount; ++row)
    {
        for (int column = 0; column < source.width; ++column)
        {
            const LinearRgb& pixel = regionPixel(source, row, column);
            regionPixel(result, row, column) = {
                std::clamp(linearToSrgb(pixel.red), 0.0F, 1.0F),
                std::clamp(linearToSrgb(pixel.green), 0.0F, 1.0F),
                std::clamp(linearToSrgb(pixel.blue), 0.0F, 1.0F),
            };
        }
    }
    return result;
}

// 목적: normalized sRGB neighborhood의 최대 channel 차이로 sharpening edge weight 계산
// 입력: source, imageHeight, pixel 좌표와 masking 제어값
// 출력: 0~1 sharpening edge weight
[[nodiscard]] float calculateEncodedEdgeWeight(
    const Float32Region& source, int imageHeight, int row, int column, float masking)
{
    if (masking == 0.0F)
    {
        return 1.0F;
    }

    const int left = std::max(column - 1, 0);
    const int right = std::min(column + 1, source.width - 1);
    const int top = std::max(row - 1, 0);
    const int bottom = std::min(row + 1, imageHeight - 1);
    const LinearRgb& center = regionPixel(source, row, column);
    const std::array<LinearRgb, 4> neighbors{
        regionPixel(source, row, left),
        regionPixel(source, row, right),
        regionPixel(source, top, column),
        regionPixel(source, bottom, column),
    };
    float maximumDifference = 0.0F;
    for (const LinearRgb& neighbor : neighbors)
    {
        maximumDifference = std::max(maximumDifference, std::abs(center.red - neighbor.red));
        maximumDifference = std::max(maximumDifference, std::abs(center.green - neighbor.green));
        maximumDifference = std::max(maximumDifference, std::abs(center.blue - neighbor.blue));
    }
    return 1.0F - masking * (1.0F - maximumDifference);
}

// 목적: linear Float32 core row를 최종 RGBA8 destination에 투영
// 입력: source region, image view와 half-open core row 범위
// 출력: non-overlapping destination core row 갱신
void writeLinearResult(const Float32Region& source, const Rgba8NeighborhoodView& view, int firstRow, int lastRow)
{
    for (int row = firstRow; row < lastRow; ++row)
    {
        const unsigned char* originalPixels = sourceRow(view, row);
        unsigned char* destinationPixels = destinationRow(view, row);
        for (int column = 0; column < view.width; ++column)
        {
            const LinearRgb& sourcePixel = regionPixel(source, row, column);
            const unsigned char* originalPixel = originalPixels + column * RgbaChannelCount;
            unsigned char* destinationPixel = destinationPixels + column * RgbaChannelCount;
            destinationPixel[0] = toChannelValue(linearToSrgb(sourcePixel.red));
            destinationPixel[1] = toChannelValue(linearToSrgb(sourcePixel.green));
            destinationPixel[2] = toChannelValue(linearToSrgb(sourcePixel.blue));
            destinationPixel[3] = originalPixel[3];
        }
    }
}

// 목적: normalized sRGB Float32 source에 controlled Sharpening을 적용해 RGBA8 core row에 투영
// 입력: encoded source, image view, core row 범위와 Sharpening parameter
// 출력: non-overlapping destination core row 갱신
void writeSharpeningResult(const Float32Region& source,
                           const Rgba8NeighborhoodView& view,
                           int firstRow,
                           int lastRow,
                           int kernelRadius,
                           const Float32NeighborhoodParameters& params)
{
    const int kernelDiameter = kernelRadius * 2 + 1;
    const int broadSampleCount = kernelDiameter * kernelDiameter;
    const float detailWeight = (params.sharpeningDetail + 1.0F) * 0.5F;
    for (int row = firstRow; row < lastRow; ++row)
    {
        const unsigned char* originalPixels = sourceRow(view, row);
        unsigned char* destinationPixels = destinationRow(view, row);
        for (int column = 0; column < view.width; ++column)
        {
            const float edgeWeight =
                calculateEncodedEdgeWeight(source, view.height, row, column, params.sharpeningMasking);
            const LinearRgb& center = regionPixel(source, row, column);
            std::array<float, 3> broadSum{};
            std::array<float, 3> fineSum{};
            for (int rowOffset = -kernelRadius; rowOffset <= kernelRadius; ++rowOffset)
            {
                const int sampleRow = std::clamp(row + rowOffset, 0, view.height - 1);
                for (int columnOffset = -kernelRadius; columnOffset <= kernelRadius; ++columnOffset)
                {
                    const int sampleColumn = std::clamp(column + columnOffset, 0, view.width - 1);
                    const LinearRgb& sample = regionPixel(source, sampleRow, sampleColumn);
                    broadSum[0] += sample.red;
                    broadSum[1] += sample.green;
                    broadSum[2] += sample.blue;
                    if (kernelRadius > 1 && std::abs(rowOffset) <= 1 && std::abs(columnOffset) <= 1)
                    {
                        fineSum[0] += sample.red;
                        fineSum[1] += sample.green;
                        fineSum[2] += sample.blue;
                    }
                }
            }

            const std::array<float, 3> sourceChannels{center.red, center.green, center.blue};
            const unsigned char* originalPixel = originalPixels + column * RgbaChannelCount;
            unsigned char* destinationPixel = destinationPixels + column * RgbaChannelCount;
            for (std::size_t channel = 0; channel < sourceChannels.size(); ++channel)
            {
                const float broadSignal =
                    sourceChannels[channel] - broadSum[channel] / static_cast<float>(broadSampleCount);
                const float fineSignal = sourceChannels[channel] - fineSum[channel] / 9.0F;
                const float signal =
                    kernelRadius == 1 ? broadSignal : broadSignal + (fineSignal - broadSignal) * detailWeight;
                destinationPixel[channel] =
                    toChannelValue(sourceChannels[channel] + signal * params.sharpeningAmount * edgeWeight);
            }
            destinationPixel[3] = originalPixel[3];
        }
    }
}

// 목적: radius 1의 불변 geometry 계산을 제거하되 sample 덧셈 순서 보존
// 입력: encoded source, image view, core row 범위와 Sharpening parameter
// 출력: generic radius 1 경로와 bit-exact한 RGBA8 core row
void writeSharpeningRadiusOne(const Float32Region& source,
                              const Rgba8NeighborhoodView& view,
                              int firstRow,
                              int lastRow,
                              const Float32NeighborhoodParameters& params)
{
    for (int row = firstRow; row < lastRow; ++row)
    {
        const int top = std::max(row - 1, 0);
        const int bottom = std::min(row + 1, view.height - 1);
        const std::array<const LinearRgb*, 3> rows{
            &regionPixel(source, top, 0), &regionPixel(source, row, 0), &regionPixel(source, bottom, 0)};
        const unsigned char* originalPixels = sourceRow(view, row);
        unsigned char* destinationPixels = destinationRow(view, row);
        for (int column = 0; column < view.width; ++column)
        {
            const int left = std::max(column - 1, 0);
            const int right = std::min(column + 1, view.width - 1);
            const std::array<int, 3> columns{left, column, right};
            const float edgeWeight =
                calculateEncodedEdgeWeight(source, view.height, row, column, params.sharpeningMasking);
            const LinearRgb& center = rows[1][column];
            std::array<float, 3> broadSum{};
            for (const LinearRgb* sampleRow : rows)
            {
                for (const int sampleColumn : columns)
                {
                    const LinearRgb& sample = sampleRow[sampleColumn];
                    broadSum[0] += sample.red;
                    broadSum[1] += sample.green;
                    broadSum[2] += sample.blue;
                }
            }

            const std::array<float, 3> sourceChannels{center.red, center.green, center.blue};
            const unsigned char* originalPixel = originalPixels + column * RgbaChannelCount;
            unsigned char* destinationPixel = destinationPixels + column * RgbaChannelCount;
            for (std::size_t channel = 0; channel < sourceChannels.size(); ++channel)
            {
                const float signal = sourceChannels[channel] - broadSum[channel] / 9.0F;
                destinationPixel[channel] =
                    toChannelValue(sourceChannels[channel] + signal * params.sharpeningAmount * edgeWeight);
            }
            destinationPixel[3] = originalPixel[3];
        }
    }
}

// 목적: 한 core stripe에서 NR→Clarity→Sharpening 순서와 dynamic halo 유지
// 입력: RGBA8 view, parameter와 half-open core row 범위
// 출력: 해당 destination core row 갱신
void processStripe(const Rgba8NeighborhoodView& view,
                   const Float32NeighborhoodParameters& params,
                   int firstRow,
                   int lastRow)
{
    const bool noiseEnabled = params.luminanceNoiseReduction != 0.0F || params.colorNoiseReduction != 0.0F;
    const bool clarityEnabled = params.clarity != 0.0F;
    const bool sharpeningEnabled = params.sharpeningAmount != 0.0F;
    const int sharpeningRadius = sharpeningEnabled ? static_cast<int>(std::lround(params.sharpeningRadius)) : 0;
    const int clarityFirstRow = std::max(0, firstRow - sharpeningRadius);
    const int clarityLastRow = std::min(view.height, lastRow + sharpeningRadius);
    const int noiseFirstRow = std::max(0, clarityFirstRow - (clarityEnabled ? 1 : 0));
    const int noiseLastRow = std::min(view.height, clarityLastRow + (clarityEnabled ? 1 : 0));
    const int sourceFirstRow = std::max(0, noiseFirstRow - (noiseEnabled ? 1 : 0));
    const int sourceLastRow = std::min(view.height, noiseLastRow + (noiseEnabled ? 1 : 0));

    Float32Region current = makeSourceRegion(view, sourceFirstRow, sourceLastRow);
    if (noiseEnabled)
    {
        current = detail::applyNoiseReduction(current,
                                              view.height,
                                              noiseFirstRow,
                                              noiseLastRow,
                                              params.luminanceNoiseReduction,
                                              params.colorNoiseReduction);
    }

    if (clarityEnabled)
    {
        current = applyClarity(current, view.height, clarityFirstRow, clarityLastRow, params.clarity);
    }

    if (!sharpeningEnabled)
    {
        writeLinearResult(current, view, firstRow, lastRow);
        return;
    }

    current = makeEncodedRegion(current);
    if (sharpeningRadius == 1)
    {
        writeSharpeningRadiusOne(current, view, firstRow, lastRow, params);
    }
    else
    {
        writeSharpeningResult(current, view, firstRow, lastRow, sharpeningRadius, params);
    }
}

// 목적: caller-owned RGBA8 view와 execution bound 불변조건 검증
// 입력: view와 Float32 neighborhood parameter
// 출력: 없음, 위반 시 invalid_argument 예외
void validateInputs(const Rgba8NeighborhoodView& view, const Float32NeighborhoodParameters& params)
{
    if (view.sourcePixels == nullptr || view.destinationPixels == nullptr || view.width <= 0 || view.height <= 0)
    {
        throw std::invalid_argument("Float32 neighborhood image view is invalid.");
    }
    if (view.sourcePixels == view.destinationPixels)
    {
        throw std::invalid_argument("Float32 neighborhood source and destination must not alias.");
    }
    if (view.width > std::numeric_limits<int>::max() / RgbaChannelCount)
    {
        throw std::invalid_argument("Float32 neighborhood image width is too large.");
    }

    const std::ptrdiff_t minimumBytesPerLine =
        static_cast<std::ptrdiff_t>(view.width) * static_cast<std::ptrdiff_t>(RgbaChannelCount);
    if (view.sourceBytesPerLine < minimumBytesPerLine || view.destinationBytesPerLine < minimumBytesPerLine)
    {
        throw std::invalid_argument("Float32 neighborhood image stride is invalid.");
    }
    if (params.grantedCpuPermits <= 0)
    {
        throw std::invalid_argument("Float32 neighborhood CPU permit count must be positive.");
    }
    if (params.sharpeningAmount != 0.0F &&
        (!std::isfinite(params.sharpeningRadius) || static_cast<int>(std::lround(params.sharpeningRadius)) < 1 ||
         static_cast<int>(std::lround(params.sharpeningRadius)) > MaximumSharpeningKernelRadius))
    {
        throw std::invalid_argument("Float32 neighborhood sharpening radius is invalid.");
    }
}

}  // namespace

int boundedFloat32NeighborhoodTaskCount(int imageHeight)
{
    if (imageHeight <= 0)
    {
        throw std::invalid_argument("Float32 neighborhood image height must be positive.");
    }
    return 1 + (imageHeight - 1) / StripeRowCount;
}

void applyBoundedFloat32Neighborhood(const Rgba8NeighborhoodView& view, const Float32NeighborhoodParameters& params)
{
    validateInputs(view, params);
    if (params.luminanceNoiseReduction == 0.0F && params.colorNoiseReduction == 0.0F && params.clarity == 0.0F &&
        params.sharpeningAmount == 0.0F)
    {
        throw std::invalid_argument("Float32 neighborhood requires an enabled stage.");
    }

    const int stripeCount = boundedFloat32NeighborhoodTaskCount(view.height);
    const int activeWorkers = std::min(params.grantedCpuPermits, stripeCount);
    std::atomic<int> nextStripe{0};
    std::atomic<bool> failed{false};
    std::mutex errorMutex;
    std::exception_ptr firstError;

    const auto processNextStripes = [&] {
        while (!failed.load(std::memory_order_relaxed))
        {
            const int stripe = nextStripe.fetch_add(1, std::memory_order_relaxed);
            if (stripe >= stripeCount)
            {
                return;
            }

            try
            {
                const int firstRow = stripe * StripeRowCount;
                const int lastRow = std::min(view.height, firstRow + StripeRowCount);
                processStripe(view, params, firstRow, lastRow);
            }
            catch (...)
            {
                {
                    const std::lock_guard lock(errorMutex);
                    if (firstError == nullptr)
                    {
                        firstError = std::current_exception();
                    }
                }
                failed.store(true, std::memory_order_relaxed);
                return;
            }
        }
    };

    if (activeWorkers == 1)
    {
        processNextStripes();
    }
    else
    {
        std::vector<std::jthread> workers;
        workers.reserve(static_cast<std::size_t>(activeWorkers));
        for (int worker = 0; worker < activeWorkers; ++worker)
        {
            workers.emplace_back(processNextStripes);
        }
    }

    if (firstError != nullptr)
    {
        std::rethrow_exception(firstError);
    }
}

}  // namespace flexraw::core::develop
