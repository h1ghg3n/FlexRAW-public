#include <QByteArray>

#include <gtest/gtest.h>

#include "health_payload_codec.h"

namespace flexraw::worker::protocol
{
namespace
{

// 목적: codec test에 사용할 valid Worker health payload 생성
// 입력: 없음
// 출력: Ready, queued 2/running 1, concurrency 4/queue 8 snapshot
[[nodiscard]] HealthResponsePayload makePayload()
{
    return {HealthServiceState::Ready, 2, 1, 4, 8};
}

TEST(HealthPayloadCodecTest, RoundTripsStableBigEndianPayload)
{
    const HealthResponsePayload original = makePayload();

    const EncodeHealthPayloadResult encoded = encodeHealthResponsePayload(original);
    ASSERT_TRUE(encoded.hasValue());
    EXPECT_EQ(QByteArray::fromHex("0001000000010000"
                                  "0000000000000002"
                                  "0000000000000001"
                                  "0000000000000004"
                                  "0000000000000008"),
              encoded.value());
    const DecodeHealthPayloadResult decoded = decodeHealthResponsePayload(encoded.value());
    ASSERT_TRUE(decoded.hasValue());
    EXPECT_EQ(original, decoded.value());
}

TEST(HealthPayloadCodecTest, RejectsUnsupportedVersionReservedFieldAndSize)
{
    const EncodeHealthPayloadResult encoded = encodeHealthResponsePayload(makePayload());
    ASSERT_TRUE(encoded.hasValue());
    QByteArray unsupportedVersion = encoded.value();
    unsupportedVersion[1] = 2;
    QByteArray reserved = encoded.value();
    reserved[7] = 1;

    const DecodeHealthPayloadResult versionResult = decodeHealthResponsePayload(unsupportedVersion);
    const DecodeHealthPayloadResult reservedResult = decodeHealthResponsePayload(reserved);
    const DecodeHealthPayloadResult sizeResult = decodeHealthResponsePayload(encoded.value().chopped(1));

    ASSERT_TRUE(versionResult.hasError());
    EXPECT_EQ(HealthPayloadErrorCode::UnsupportedVersion, versionResult.error().code);
    ASSERT_TRUE(reservedResult.hasError());
    EXPECT_EQ(HealthPayloadErrorCode::ReservedFieldSet, reservedResult.error().code);
    ASSERT_TRUE(sizeResult.hasError());
    EXPECT_EQ(HealthPayloadErrorCode::InvalidSize, sizeResult.error().code);
}

TEST(HealthPayloadCodecTest, RejectsUnknownServiceStateAndOutOfRangeLoad)
{
    const EncodeHealthPayloadResult encoded = encodeHealthResponsePayload(makePayload());
    ASSERT_TRUE(encoded.hasValue());
    QByteArray unknownState = encoded.value();
    unknownState[5] = 99;

    HealthResponsePayload invalidLoad = makePayload();
    invalidLoad.runningJobs = invalidLoad.maximumConcurrentJobs + 1;
    const EncodeHealthPayloadResult encodeResult = encodeHealthResponsePayload(invalidLoad);
    const DecodeHealthPayloadResult stateResult = decodeHealthResponsePayload(unknownState);

    ASSERT_TRUE(encodeResult.hasError());
    EXPECT_EQ(HealthPayloadErrorCode::InvalidLoad, encodeResult.error().code);
    ASSERT_TRUE(stateResult.hasError());
    EXPECT_EQ(HealthPayloadErrorCode::UnknownServiceState, stateResult.error().code);
}

}  // namespace
}  // namespace flexraw::worker::protocol
