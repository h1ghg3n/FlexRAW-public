#include "frame_codec.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace flexraw::worker::protocol
{
namespace
{

constexpr qsizetype MagicOffset = 0;
constexpr qsizetype MajorVersionOffset = 4;
constexpr qsizetype MinorVersionOffset = 6;
constexpr qsizetype MessageTypeOffset = 8;
constexpr qsizetype FlagsOffset = 10;
constexpr qsizetype JobIdOffset = 12;
constexpr qsizetype PayloadSizeOffset = 20;

// 목적: 지원되는 v1 message type인지 확인
// 입력: value: wire header에서 해석한 정수 값
// 출력: 알려진 message type 여부
[[nodiscard]] bool isKnownMessageType(const std::uint16_t value)
{
    switch (static_cast<MessageType>(value))
    {
    case MessageType::RenderRequest:
    case MessageType::CancelRequest:
    case MessageType::JobAccepted:
    case MessageType::RenderSucceeded:
    case MessageType::RenderFailed:
    case MessageType::ServerBusy:
    case MessageType::ResourceBusy:
        return true;
    }
    return false;
}

// 목적: 16-bit unsigned 값을 network byte order로 기록
// 입력: destination: 2-byte 출력 위치, value: 기록할 값
// 출력: 없음
void writeUnsigned16(char* const destination, const std::uint16_t value)
{
    destination[0] = static_cast<char>((value >> 8U) & 0xFFU);
    destination[1] = static_cast<char>(value & 0xFFU);
}

// 목적: 32-bit unsigned 값을 network byte order로 기록
// 입력: destination: 4-byte 출력 위치, value: 기록할 값
// 출력: 없음
void writeUnsigned32(char* const destination, const std::uint32_t value)
{
    for (std::uint32_t index = 0; index < 4U; ++index)
    {
        const std::uint32_t shift = (3U - index) * 8U;
        destination[index] = static_cast<char>((value >> shift) & 0xFFU);
    }
}

// 목적: 64-bit unsigned 값을 network byte order로 기록
// 입력: destination: 8-byte 출력 위치, value: 기록할 값
// 출력: 없음
void writeUnsigned64(char* const destination, const std::uint64_t value)
{
    for (std::uint32_t index = 0; index < 8U; ++index)
    {
        const std::uint32_t shift = (7U - index) * 8U;
        destination[index] = static_cast<char>((value >> shift) & 0xFFU);
    }
}

// 목적: network byte order의 16-bit unsigned 값 해석
// 입력: source: 2-byte 입력 위치
// 출력: host byte order 값
[[nodiscard]] std::uint16_t readUnsigned16(const char* const source)
{
    const auto byte0 = static_cast<std::uint8_t>(source[0]);
    const auto byte1 = static_cast<std::uint8_t>(source[1]);
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(byte0) << 8U) | byte1);
}

// 목적: network byte order의 32-bit unsigned 값 해석
// 입력: source: 4-byte 입력 위치
// 출력: host byte order 값
[[nodiscard]] std::uint32_t readUnsigned32(const char* const source)
{
    std::uint32_t value = 0;
    for (std::uint32_t index = 0; index < 4U; ++index)
    {
        value = (value << 8U) | static_cast<std::uint8_t>(source[index]);
    }
    return value;
}

// 목적: network byte order의 64-bit unsigned 값 해석
// 입력: source: 8-byte 입력 위치
// 출력: host byte order 값
[[nodiscard]] std::uint64_t readUnsigned64(const char* const source)
{
    std::uint64_t value = 0;
    for (std::uint32_t index = 0; index < 8U; ++index)
    {
        value = (value << 8U) | static_cast<std::uint8_t>(source[index]);
    }
    return value;
}

// 목적: protocol 오류 Result 생성
// 입력: code: 분류 코드, message: 진단 문자열
// 출력: ProtocolError 값
[[nodiscard]] ProtocolError makeProtocolError(const ProtocolErrorCode code, QString message)
{
    return {code, std::move(message)};
}

}  // namespace

// 목적: protocol frame을 고정 big-endian wire representation으로 직렬화
// 입력: frame: message type, session JobId와 bounded payload
// 출력: 직렬화된 frame 또는 contract 위반 오류
EncodeFrameResult encodeFrame(const ProtocolFrame& frame)
{
    const auto messageTypeValue = static_cast<std::uint16_t>(frame.messageType);
    if (!isKnownMessageType(messageTypeValue))
    {
        return EncodeFrameResult::failure(
            makeProtocolError(ProtocolErrorCode::UnknownMessageType, QStringLiteral("Unknown protocol message type.")));
    }
    if (frame.jobId == 0)
    {
        return EncodeFrameResult::failure(
            makeProtocolError(ProtocolErrorCode::InvalidJobId, QStringLiteral("Protocol JobId must be non-zero.")));
    }
    if (frame.payload.size() > static_cast<qsizetype>(MaximumPayloadBytes))
    {
        return EncodeFrameResult::failure(makeProtocolError(ProtocolErrorCode::PayloadTooLarge,
                                                            QStringLiteral("Protocol payload exceeds the limit.")));
    }

    const auto payloadBytes = static_cast<std::uint32_t>(frame.payload.size());
    QByteArray encoded(ProtocolHeaderBytes + frame.payload.size(), Qt::Uninitialized);
    char* const output = encoded.data();
    std::copy(ProtocolMagic.cbegin(), ProtocolMagic.cend(), output + MagicOffset);
    writeUnsigned16(output + MajorVersionOffset, ProtocolMajorVersion);
    writeUnsigned16(output + MinorVersionOffset, ProtocolMinorVersion);
    writeUnsigned16(output + MessageTypeOffset, messageTypeValue);
    writeUnsigned16(output + FlagsOffset, 0);
    writeUnsigned64(output + JobIdOffset, frame.jobId);
    writeUnsigned32(output + PayloadSizeOffset, payloadBytes);
    if (!frame.payload.isEmpty())
    {
        std::memcpy(output + ProtocolHeaderBytes, frame.payload.constData(), static_cast<std::size_t>(payloadBytes));
    }
    return EncodeFrameResult::success(std::move(encoded));
}

// 목적: 완성된 24-byte wire header를 검증하고 frame metadata 해석
// 입력: bytes: header 이상을 포함한 byte view
// 출력: 검증된 frame header 또는 protocol 오류
DecodeFrameHeaderResult decodeFrameHeader(const QByteArrayView bytes)
{
    if (bytes.size() < ProtocolHeaderBytes)
    {
        return DecodeFrameHeaderResult::failure(
            makeProtocolError(ProtocolErrorCode::IncompleteFrame, QStringLiteral("Protocol header is incomplete.")));
    }

    const char* const input = bytes.data();
    if (!std::equal(ProtocolMagic.cbegin(), ProtocolMagic.cend(), input + MagicOffset))
    {
        return DecodeFrameHeaderResult::failure(
            makeProtocolError(ProtocolErrorCode::InvalidMagic, QStringLiteral("Protocol magic does not match.")));
    }

    const std::uint16_t majorVersion = readUnsigned16(input + MajorVersionOffset);
    const std::uint16_t minorVersion = readUnsigned16(input + MinorVersionOffset);
    if (majorVersion != ProtocolMajorVersion || minorVersion != ProtocolMinorVersion)
    {
        return DecodeFrameHeaderResult::failure(makeProtocolError(
            ProtocolErrorCode::UnsupportedVersion, QStringLiteral("Protocol version is not supported.")));
    }

    const std::uint16_t messageTypeValue = readUnsigned16(input + MessageTypeOffset);
    if (!isKnownMessageType(messageTypeValue))
    {
        return DecodeFrameHeaderResult::failure(
            makeProtocolError(ProtocolErrorCode::UnknownMessageType, QStringLiteral("Unknown protocol message type.")));
    }
    if (readUnsigned16(input + FlagsOffset) != 0)
    {
        return DecodeFrameHeaderResult::failure(makeProtocolError(
            ProtocolErrorCode::ReservedFlagsSet, QStringLiteral("Reserved protocol flags must be zero.")));
    }

    const JobId jobId = readUnsigned64(input + JobIdOffset);
    if (jobId == 0)
    {
        return DecodeFrameHeaderResult::failure(
            makeProtocolError(ProtocolErrorCode::InvalidJobId, QStringLiteral("Protocol JobId must be non-zero.")));
    }

    const std::uint32_t payloadBytes = readUnsigned32(input + PayloadSizeOffset);
    if (payloadBytes > MaximumPayloadBytes)
    {
        return DecodeFrameHeaderResult::failure(makeProtocolError(
            ProtocolErrorCode::PayloadTooLarge, QStringLiteral("Protocol payload exceeds the limit.")));
    }

    return DecodeFrameHeaderResult::success({static_cast<MessageType>(messageTypeValue), jobId, payloadBytes});
}

// 목적: 정확히 하나의 완성된 wire frame을 검증하고 역직렬화
// 입력: bytes: header와 선언된 payload만 포함한 byte view
// 출력: 역직렬화된 frame 또는 protocol 오류
DecodeFrameResult decodeFrame(const QByteArrayView bytes)
{
    const DecodeFrameHeaderResult headerResult = decodeFrameHeader(bytes);
    if (headerResult.hasError())
    {
        return DecodeFrameResult::failure(headerResult.error());
    }

    const ProtocolFrameHeader& header = headerResult.value();
    const qsizetype expectedBytes = ProtocolHeaderBytes + static_cast<qsizetype>(header.payloadBytes);
    if (bytes.size() < expectedBytes)
    {
        return DecodeFrameResult::failure(
            makeProtocolError(ProtocolErrorCode::IncompleteFrame, QStringLiteral("Protocol payload is incomplete.")));
    }
    if (bytes.size() > expectedBytes)
    {
        return DecodeFrameResult::failure(makeProtocolError(ProtocolErrorCode::TrailingBytes,
                                                            QStringLiteral("Protocol frame contains trailing bytes.")));
    }

    QByteArray payload;
    if (header.payloadBytes > 0)
    {
        payload = QByteArray(bytes.data() + ProtocolHeaderBytes, static_cast<qsizetype>(header.payloadBytes));
    }
    return DecodeFrameResult::success({header.messageType, header.jobId, std::move(payload)});
}

}  // namespace flexraw::worker::protocol
