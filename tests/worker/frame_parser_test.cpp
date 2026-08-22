#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

#include <QByteArray>
#include <QByteArrayView>

#include <gtest/gtest.h>

#include "frame_codec.h"
#include "frame_parser.h"

namespace flexraw::worker::protocol
{
namespace
{

// 목적: parser test에서 사용할 encoded frame 생성
// 입력: jobId: session correlation 값, payload: frame payload
// 출력: codec 검증을 통과한 wire bytes
[[nodiscard]] QByteArray makeEncodedFrame(const JobId jobId, QByteArray payload)
{
    const EncodeFrameResult result = encodeFrame({MessageType::RenderRequest, jobId, std::move(payload)});
    EXPECT_TRUE(result.hasValue());
    return result.hasValue() ? result.value() : QByteArray{};
}

TEST(FrameParserTest, ReassemblesOneByteFragments)
{
    const QByteArray encoded = makeEncodedFrame(7, "fragmented");
    FrameParser parser;
    std::vector<ProtocolFrame> frames;

    for (const char byte : encoded)
    {
        const ParseFramesOutcome outcome = parser.append(QByteArrayView(&byte, 1));
        ASSERT_FALSE(outcome.terminalError.has_value());
        frames.insert(frames.end(), outcome.frames.cbegin(), outcome.frames.cend());
    }

    ASSERT_EQ(1U, frames.size());
    EXPECT_EQ(7U, frames.front().jobId);
    EXPECT_EQ(QByteArray("fragmented"), frames.front().payload);
    EXPECT_EQ(0, parser.bufferedByteCount());
}

TEST(FrameParserTest, ParsesCoalescedFramesInOrder)
{
    const QByteArray first = makeEncodedFrame(1, "first");
    const QByteArray second = makeEncodedFrame(2, "second");
    FrameParser parser;

    const ParseFramesOutcome outcome = parser.append(first + second);

    ASSERT_FALSE(outcome.terminalError.has_value());
    ASSERT_EQ(2U, outcome.frames.size());
    EXPECT_EQ(1U, outcome.frames[0].jobId);
    EXPECT_EQ(QByteArray("first"), outcome.frames[0].payload);
    EXPECT_EQ(2U, outcome.frames[1].jobId);
    EXPECT_EQ(QByteArray("second"), outcome.frames[1].payload);
}

TEST(FrameParserTest, PreservesIncompletePayloadAcrossReads)
{
    const QByteArray encoded = makeEncodedFrame(9, QByteArray(1024, 'p'));
    FrameParser parser;

    const ParseFramesOutcome prefix = parser.append(encoded.first(ProtocolHeaderBytes + 17));
    ASSERT_FALSE(prefix.terminalError.has_value());
    EXPECT_TRUE(prefix.frames.empty());
    EXPECT_EQ(ProtocolHeaderBytes + 17, parser.bufferedByteCount());

    const ParseFramesOutcome suffix = parser.append(encoded.sliced(ProtocolHeaderBytes + 17));
    ASSERT_FALSE(suffix.terminalError.has_value());
    ASSERT_EQ(1U, suffix.frames.size());
    EXPECT_EQ(QByteArray(1024, 'p'), suffix.frames.front().payload);
}

TEST(FrameParserTest, RejectsOversizedHeaderBeforePayloadArrives)
{
    QByteArray header = makeEncodedFrame(1, {}).first(ProtocolHeaderBytes);
    const std::uint32_t oversized = MaximumPayloadBytes + 1U;
    header[20] = static_cast<char>((oversized >> 24U) & 0xFFU);
    header[21] = static_cast<char>((oversized >> 16U) & 0xFFU);
    header[22] = static_cast<char>((oversized >> 8U) & 0xFFU);
    header[23] = static_cast<char>(oversized & 0xFFU);
    FrameParser parser;

    const ParseFramesOutcome outcome = parser.append(header);

    ASSERT_TRUE(outcome.terminalError.has_value());
    EXPECT_EQ(ProtocolErrorCode::PayloadTooLarge, outcome.terminalError->code);
    EXPECT_TRUE(outcome.frames.empty());
    EXPECT_TRUE(parser.isTerminated());
    EXPECT_EQ(ProtocolHeaderBytes, parser.bufferedByteCount());
}

TEST(FrameParserTest, RejectsInputUntilResetAfterProtocolViolation)
{
    const QByteArray valid = makeEncodedFrame(5, "valid");
    QByteArray invalid = valid;
    invalid[0] = 'X';
    FrameParser parser;
    ASSERT_TRUE(parser.append(invalid).terminalError.has_value());

    const ParseFramesOutcome terminated = parser.append(valid);
    ASSERT_TRUE(terminated.terminalError.has_value());
    EXPECT_EQ(ProtocolErrorCode::ParserTerminated, terminated.terminalError->code);
    EXPECT_TRUE(terminated.frames.empty());

    parser.reset();
    const ParseFramesOutcome recovered = parser.append(valid);
    ASSERT_FALSE(recovered.terminalError.has_value());
    ASSERT_EQ(1U, recovered.frames.size());
    EXPECT_EQ(5U, recovered.frames.front().jobId);
}

TEST(FrameParserTest, ReturnsCompletedFramesBeforeTerminalErrorInSameAppend)
{
    const QByteArray valid = makeEncodedFrame(11, "accepted-before-error");
    QByteArray invalidHeader = makeEncodedFrame(12, {}).first(ProtocolHeaderBytes);
    invalidHeader[0] = 'X';
    FrameParser parser;

    const ParseFramesOutcome outcome = parser.append(valid + invalidHeader);

    ASSERT_EQ(1U, outcome.frames.size());
    EXPECT_EQ(11U, outcome.frames.front().jobId);
    EXPECT_EQ(QByteArray("accepted-before-error"), outcome.frames.front().payload);
    ASSERT_TRUE(outcome.terminalError.has_value());
    EXPECT_EQ(ProtocolErrorCode::InvalidMagic, outcome.terminalError->code);
    EXPECT_TRUE(parser.isTerminated());
}

}  // namespace
}  // namespace flexraw::worker::protocol
