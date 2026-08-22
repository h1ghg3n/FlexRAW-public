#pragma once

#include <array>
#include <cstdint>

#include <QByteArray>
#include <QString>

#include "job_id.h"

namespace flexraw::worker::protocol
{

inline constexpr std::array<char, 4> ProtocolMagic{'F', 'R', 'W', 'K'};
inline constexpr std::uint16_t ProtocolMajorVersion = 1;
inline constexpr std::uint16_t ProtocolMinorVersion = 1;
inline constexpr std::uint32_t MaximumPayloadBytes = 1024U * 1024U;
inline constexpr qsizetype ProtocolHeaderBytes = 24;

enum class MessageType : std::uint16_t
{
    RenderRequest = 1,
    CancelRequest = 2,
    JobAccepted = 3,
    RenderSucceeded = 4,
    RenderFailed = 5,
    ServerBusy = 6,
    ResourceBusy = 7,
};

struct ProtocolFrameHeader
{
    MessageType messageType{MessageType::RenderRequest};
    JobId jobId{0};
    std::uint32_t payloadBytes{0};
};

struct ProtocolFrame
{
    MessageType messageType{MessageType::RenderRequest};
    JobId jobId{0};
    QByteArray payload;

    // 목적: test와 adapter에서 frame value의 완전 일치 여부 비교
    // 입력: 비교할 ProtocolFrame 값
    // 출력: message type, JobId와 payload가 모두 같은지 여부
    [[nodiscard]] bool operator==(const ProtocolFrame&) const = default;
};

enum class ProtocolErrorCode
{
    InvalidMagic,
    UnsupportedVersion,
    UnknownMessageType,
    ReservedFlagsSet,
    InvalidJobId,
    PayloadTooLarge,
    IncompleteFrame,
    TrailingBytes,
    ParserTerminated,
};

struct ProtocolError
{
    ProtocolErrorCode code{ProtocolErrorCode::IncompleteFrame};
    QString message;
};

}  // namespace flexraw::worker::protocol
