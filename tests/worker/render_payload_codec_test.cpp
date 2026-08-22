#include <chrono>
#include <limits>

#include <QByteArray>
#include <QString>

#include <gtest/gtest.h>

#include "protocol_contracts.h"
#include "render_payload_codec.h"

namespace flexraw::worker::runtime
{
namespace
{

// 목적: request codec의 모든 value 종류를 통과시키는 대표 payload 생성
// 입력: 없음
// 출력: Unicode path, non-default develop/output option을 가진 payload
[[nodiscard]] RenderRequestPayload makeRequestPayload()
{
    RenderRequestPayload payload;
    payload.sourceRelativePath = QStringLiteral("originals/사진.CR3");
    payload.outputRelativePath = QStringLiteral("exports/사진-result.tiff");
    payload.developParams.exposureEv = 0.35F;
    payload.developParams.contrast = 0.2F;
    payload.developParams.highlights = -0.25F;
    payload.developParams.shadows = 0.25F;
    payload.developParams.whiteBalanceMode = core::types::WhiteBalanceMode::Custom;
    payload.developParams.whiteBalanceTemperatureKelvin = 6200.0F;
    payload.developParams.whiteBalanceTint = 0.05F;
    payload.developParams.clarity = 0.15F;
    payload.developParams.dehaze = 0.1F;
    payload.developParams.sharpeningAmount = 0.25F;
    payload.developParams.sharpeningRadius = 1.2F;
    payload.developParams.luminanceNoiseReduction = 0.15F;
    payload.developParams.colorNoiseReduction = 0.15F;
    payload.developParams.pointCurveMidtones = 0.05F;
    payload.outputOptions.format = core::export_::RasterExportFormat::Tiff;
    payload.outputOptions.tiffCompression = core::export_::TiffCompression::None;
    payload.outputOptions.maximumDimension = 4096;
    payload.outputOptions.outputColorSpace = core::export_::RasterOutputColorSpace::DisplayP3;
    payload.outputOptions.includeMetadata = false;
    return payload;
}

// 목적: 두 raster output option의 모든 field 일치 여부 검증
// 입력: expected/actual: 비교할 option 값
// 출력: Google Test assertion 결과
void expectOutputOptionsEqual(const core::export_::RasterExportOptions& expected,
                              const core::export_::RasterExportOptions& actual)
{
    EXPECT_EQ(expected.format, actual.format);
    EXPECT_EQ(expected.jpegQuality, actual.jpegQuality);
    EXPECT_EQ(expected.pngCompression, actual.pngCompression);
    EXPECT_EQ(expected.tiffCompression, actual.tiffCompression);
    EXPECT_EQ(expected.maximumDimension, actual.maximumDimension);
    EXPECT_EQ(expected.outputColorSpace, actual.outputColorSpace);
    EXPECT_EQ(expected.includeMetadata, actual.includeMetadata);
}

// 목적: 두 RenderStats의 모든 fixed stage timing 일치 여부 검증
// 입력: expected/actual: 비교할 timing 값
// 출력: Google Test assertion 결과
void expectStatsEqual(const core::measurement::RenderStats& expected, const core::measurement::RenderStats& actual)
{
    EXPECT_EQ(expected.decodeNanoseconds, actual.decodeNanoseconds);
    EXPECT_EQ(expected.sourceConversionNanoseconds, actual.sourceConversionNanoseconds);
    EXPECT_EQ(expected.developNanoseconds, actual.developNanoseconds);
    EXPECT_EQ(expected.outputNanoseconds, actual.outputNanoseconds);
    EXPECT_EQ(expected.totalNanoseconds, actual.totalNanoseconds);
}

TEST(RenderPayloadCodecTest, RoundTripsRenderRequestWithUnicodePaths)
{
    const RenderRequestPayload original = makeRequestPayload();

    const EncodePayloadResult encoded = encodeRenderRequestPayload(original);
    ASSERT_TRUE(encoded.hasValue());
    const DecodeRenderRequestResult decoded = decodeRenderRequestPayload(encoded.value());

    ASSERT_TRUE(decoded.hasValue());
    EXPECT_EQ(original.sourceRelativePath, decoded.value().sourceRelativePath);
    EXPECT_EQ(original.outputRelativePath, decoded.value().outputRelativePath);
    EXPECT_EQ(original.developParams, decoded.value().developParams);
    expectOutputOptionsEqual(original.outputOptions, decoded.value().outputOptions);
}

TEST(RenderPayloadCodecTest, WritesStablePayloadVersionPrefix)
{
    const EncodePayloadResult encoded = encodeRenderRequestPayload(makeRequestPayload());

    ASSERT_TRUE(encoded.hasValue());
    ASSERT_GE(encoded.value().size(), 4);
    EXPECT_EQ(QByteArray::fromHex("00010000"), encoded.value().first(4));
}

TEST(RenderPayloadCodecTest, RejectsUnsupportedVersionInvalidUtf8AndTrailingBytes)
{
    const EncodePayloadResult encoded = encodeRenderRequestPayload(makeRequestPayload());
    ASSERT_TRUE(encoded.hasValue());

    QByteArray unsupportedVersion = encoded.value();
    unsupportedVersion[1] = 2;
    const DecodeRenderRequestResult versionResult = decodeRenderRequestPayload(unsupportedVersion);
    ASSERT_TRUE(versionResult.hasError());
    EXPECT_EQ(PayloadErrorCode::UnsupportedVersion, versionResult.error().code);

    QByteArray invalidUtf8 = encoded.value();
    invalidUtf8[8] = static_cast<char>(0xFF);
    const DecodeRenderRequestResult utf8Result = decodeRenderRequestPayload(invalidUtf8);
    ASSERT_TRUE(utf8Result.hasError());
    EXPECT_EQ(PayloadErrorCode::InvalidUtf8, utf8Result.error().code);

    const DecodeRenderRequestResult trailingResult = decodeRenderRequestPayload(encoded.value() + QByteArray(1, 'x'));
    ASSERT_TRUE(trailingResult.hasError());
    EXPECT_EQ(PayloadErrorCode::TrailingBytes, trailingResult.error().code);
}

TEST(RenderPayloadCodecTest, RejectsInvalidDevelopAndOutputValues)
{
    RenderRequestPayload invalidDevelop = makeRequestPayload();
    invalidDevelop.developParams.exposureEv = std::numeric_limits<float>::infinity();
    const EncodePayloadResult developResult = encodeRenderRequestPayload(invalidDevelop);
    ASSERT_TRUE(developResult.hasError());
    EXPECT_EQ(PayloadErrorCode::InvalidDevelopParams, developResult.error().code);

    RenderRequestPayload invalidOutput = makeRequestPayload();
    invalidOutput.outputOptions.maximumDimension = -1;
    const EncodePayloadResult outputResult = encodeRenderRequestPayload(invalidOutput);
    ASSERT_TRUE(outputResult.hasError());
    EXPECT_EQ(PayloadErrorCode::InvalidOutputOptions, outputResult.error().code);
}

TEST(RenderPayloadCodecTest, PreservesInactiveSignedOutputOptions)
{
    RenderRequestPayload original = makeRequestPayload();
    original.outputOptions.format = core::export_::RasterExportFormat::Tiff;
    original.outputOptions.jpegQuality = -7;
    original.outputOptions.pngCompression = -2;

    const EncodePayloadResult encoded = encodeRenderRequestPayload(original);
    ASSERT_TRUE(encoded.hasValue());
    const DecodeRenderRequestResult decoded = decodeRenderRequestPayload(encoded.value());

    ASSERT_TRUE(decoded.hasValue());
    expectOutputOptionsEqual(original.outputOptions, decoded.value().outputOptions);
}

TEST(RenderPayloadCodecTest, RejectsInvalidWireEnumAndTruncatedRequest)
{
    const RenderRequestPayload original = makeRequestPayload();
    const EncodePayloadResult encoded = encodeRenderRequestPayload(original);
    ASSERT_TRUE(encoded.hasValue());

    const qsizetype whiteBalanceOffset =
        4 + 4 + original.sourceRelativePath.toUtf8().size() + 4 + original.outputRelativePath.toUtf8().size();
    QByteArray invalidEnum = encoded.value();
    invalidEnum[whiteBalanceOffset] = static_cast<char>(0x7F);
    const DecodeRenderRequestResult enumResult = decodeRenderRequestPayload(invalidEnum);
    ASSERT_TRUE(enumResult.hasError());
    EXPECT_EQ(PayloadErrorCode::InvalidEnumValue, enumResult.error().code);

    const DecodeRenderRequestResult truncatedResult = decodeRenderRequestPayload(encoded.value().chopped(1));
    ASSERT_TRUE(truncatedResult.hasError());
    EXPECT_EQ(PayloadErrorCode::MalformedPayload, truncatedResult.error().code);
}

TEST(RenderPayloadCodecTest, RoundTripsSuccessFailureAndBusyPayloads)
{
    const core::measurement::RenderStats stats{11, 22, 33, 44, 55};
    const RenderSucceededPayload succeeded{QStringLiteral("exports/result.jpg"), 9876543210ULL, stats};
    const EncodePayloadResult encodedSuccess = encodeRenderSucceededPayload(succeeded);
    ASSERT_TRUE(encodedSuccess.hasValue());
    const DecodeRenderSucceededResult decodedSuccess = decodeRenderSucceededPayload(encodedSuccess.value());
    ASSERT_TRUE(decodedSuccess.hasValue());
    EXPECT_EQ(succeeded.outputRelativePath, decodedSuccess.value().outputRelativePath);
    EXPECT_EQ(succeeded.byteSize, decodedSuccess.value().byteSize);
    expectStatsEqual(stats, decodedSuccess.value().stats);

    const RenderFailedPayload failed{{core::types::ErrorCode::DecodeFailed, QStringLiteral("decode failed")}, stats};
    const EncodePayloadResult encodedFailure = encodeRenderFailedPayload(failed);
    ASSERT_TRUE(encodedFailure.hasValue());
    const DecodeRenderFailedResult decodedFailure = decodeRenderFailedPayload(encodedFailure.value());
    ASSERT_TRUE(decodedFailure.hasValue());
    EXPECT_EQ(failed.cause.code, decodedFailure.value().cause.code);
    EXPECT_EQ(failed.cause.message, decodedFailure.value().cause.message);
    expectStatsEqual(stats, decodedFailure.value().stats);

    const ServerBusyPayload busy{QStringLiteral("queue is full")};
    const EncodePayloadResult encodedBusy = encodeServerBusyPayload(busy);
    ASSERT_TRUE(encodedBusy.hasValue());
    const DecodeServerBusyResult decodedBusy = decodeServerBusyPayload(encodedBusy.value());
    ASSERT_TRUE(decodedBusy.hasValue());
    EXPECT_EQ(busy.message, decodedBusy.value().message);

    const ResourceBusyPayload resourceBusy{QStringLiteral("INSUFFICIENT_MEMORY"), std::chrono::milliseconds(3000)};
    const EncodePayloadResult encodedResourceBusy = encodeResourceBusyPayload(resourceBusy);
    ASSERT_TRUE(encodedResourceBusy.hasValue());
    const DecodeResourceBusyResult decodedResourceBusy = decodeResourceBusyPayload(encodedResourceBusy.value());
    ASSERT_TRUE(decodedResourceBusy.hasValue());
    EXPECT_EQ(resourceBusy.message, decodedResourceBusy.value().message);
    ASSERT_TRUE(decodedResourceBusy.value().retryAfter.has_value());
    EXPECT_EQ(resourceBusy.retryAfter, decodedResourceBusy.value().retryAfter);
}

TEST(RenderPayloadCodecTest, RejectsNegativeResourceBusyRetryDelay)
{
    const ResourceBusyPayload busy{QStringLiteral("busy"), std::chrono::milliseconds(-1)};

    const EncodePayloadResult result = encodeResourceBusyPayload(busy);

    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(PayloadErrorCode::MalformedPayload, result.error().code);
}

TEST(RenderPayloadCodecTest, RejectsOversizedDiagnosticMessage)
{
    const ServerBusyPayload busy{QString(static_cast<qsizetype>(MaximumWireMessageBytes) + 1, QLatin1Char('x'))};

    const EncodePayloadResult result = encodeServerBusyPayload(busy);

    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(PayloadErrorCode::StringTooLong, result.error().code);

    const QByteArray oversizedWire(static_cast<qsizetype>(protocol::MaximumPayloadBytes) + 1, 0);
    const DecodeServerBusyResult decodeResult = decodeServerBusyPayload(oversizedWire);
    ASSERT_TRUE(decodeResult.hasError());
    EXPECT_EQ(PayloadErrorCode::PayloadTooLarge, decodeResult.error().code);
}

}  // namespace
}  // namespace flexraw::worker::runtime
