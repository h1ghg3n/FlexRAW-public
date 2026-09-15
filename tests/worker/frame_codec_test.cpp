#include <utility>

#include <QByteArray>

#include <gtest/gtest.h>

#include "frame_codec.h"

namespace flexraw::worker::protocol
{
namespace
{

// 목적: 정상 protocol frame 생성
// 입력: type: message 분류, jobId: session correlation 값, payload: binary payload
// 출력: codec test용 ProtocolFrame
[[nodiscard]] ProtocolFrame makeFrame(const MessageType type = MessageType::RenderRequest,
                                      const JobId jobId = 0x0102030405060708ULL,
                                      QByteArray payload = QByteArray("render\0request", 14))
{
    return {type, jobId, std::move(payload)};
}

TEST(FrameCodecTest, RoundTripsBinaryPayload)
{
    const ProtocolFrame original = makeFrame();

    const EncodeFrameResult encoded = encodeFrame(original);
    ASSERT_TRUE(encoded.hasValue());
    const DecodeFrameResult decoded = decodeFrame(encoded.value());

    ASSERT_TRUE(decoded.hasValue());
    EXPECT_EQ(original, decoded.value());
}

TEST(FrameCodecTest, WritesStableBigEndianHeader)
{
    const EncodeFrameResult result = encodeFrame(makeFrame(MessageType::RenderRequest, 0x0102030405060708ULL, "abc"));

    ASSERT_TRUE(result.hasValue());
    const QByteArray& bytes = result.value();
    ASSERT_EQ(ProtocolHeaderBytes + 3, bytes.size());
    const QByteArray expectedHeader =
        QByteArray("FRWK", 4) + QByteArray::fromHex("0001000200010000010203040506070800000003");
    EXPECT_EQ(expectedHeader, bytes.first(ProtocolHeaderBytes));
}

TEST(FrameCodecTest, RejectsZeroJobId)
{
    const EncodeFrameResult result = encodeFrame(makeFrame(MessageType::RenderRequest, 0));

    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(ProtocolErrorCode::InvalidJobId, result.error().code);
}

TEST(FrameCodecTest, RejectsPayloadAboveLimit)
{
    QByteArray payload(static_cast<qsizetype>(MaximumPayloadBytes) + 1, 'x');

    const EncodeFrameResult result = encodeFrame(makeFrame(MessageType::RenderRequest, 1, std::move(payload)));

    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(ProtocolErrorCode::PayloadTooLarge, result.error().code);
}

TEST(FrameCodecTest, RejectsUnsupportedVersion)
{
    const EncodeFrameResult encoded = encodeFrame(makeFrame());
    ASSERT_TRUE(encoded.hasValue());
    QByteArray bytes = encoded.value();
    bytes[5] = 2;

    const DecodeFrameResult result = decodeFrame(bytes);

    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(ProtocolErrorCode::UnsupportedVersion, result.error().code);
}

TEST(FrameCodecTest, RejectsUnknownMessageTypeAndReservedFlags)
{
    const EncodeFrameResult encoded = encodeFrame(makeFrame());
    ASSERT_TRUE(encoded.hasValue());
    QByteArray unknownType = encoded.value();
    unknownType[8] = 0x7F;
    unknownType[9] = 0x7F;
    QByteArray reservedFlags = encoded.value();
    reservedFlags[11] = 1;

    const DecodeFrameResult unknownResult = decodeFrame(unknownType);
    const DecodeFrameResult flagsResult = decodeFrame(reservedFlags);

    ASSERT_TRUE(unknownResult.hasError());
    EXPECT_EQ(ProtocolErrorCode::UnknownMessageType, unknownResult.error().code);
    ASSERT_TRUE(flagsResult.hasError());
    EXPECT_EQ(ProtocolErrorCode::ReservedFlagsSet, flagsResult.error().code);
}

TEST(FrameCodecTest, RejectsIncompleteAndTrailingBytes)
{
    const EncodeFrameResult encoded = encodeFrame(makeFrame());
    ASSERT_TRUE(encoded.hasValue());

    const DecodeFrameResult incomplete = decodeFrame(encoded.value().first(encoded.value().size() - 1));
    const DecodeFrameResult trailing = decodeFrame(encoded.value() + QByteArray(1, 'x'));

    ASSERT_TRUE(incomplete.hasError());
    EXPECT_EQ(ProtocolErrorCode::IncompleteFrame, incomplete.error().code);
    ASSERT_TRUE(trailing.hasError());
    EXPECT_EQ(ProtocolErrorCode::TrailingBytes, trailing.error().code);
}

}  // namespace
}  // namespace flexraw::worker::protocol
