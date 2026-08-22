#pragma once

#include <QByteArray>
#include <QByteArrayView>

#include "protocol_contracts.h"
#include "result.h"

namespace flexraw::worker::protocol
{

using EncodeFrameResult = core::types::Result<QByteArray, ProtocolError>;
using DecodeFrameHeaderResult = core::types::Result<ProtocolFrameHeader, ProtocolError>;
using DecodeFrameResult = core::types::Result<ProtocolFrame, ProtocolError>;

// 목적: protocol frame을 고정 big-endian wire representation으로 직렬화
// 입력: frame: message type, session JobId와 bounded payload
// 출력: 직렬화된 frame 또는 contract 위반 오류
[[nodiscard]] EncodeFrameResult encodeFrame(const ProtocolFrame& frame);

// 목적: 완성된 24-byte wire header를 검증하고 frame metadata 해석
// 입력: bytes: header 이상을 포함한 byte view
// 출력: 검증된 frame header 또는 protocol 오류
[[nodiscard]] DecodeFrameHeaderResult decodeFrameHeader(QByteArrayView bytes);

// 목적: 정확히 하나의 완성된 wire frame을 검증하고 역직렬화
// 입력: bytes: header와 선언된 payload만 포함한 byte view
// 출력: 역직렬화된 frame 또는 protocol 오류
[[nodiscard]] DecodeFrameResult decodeFrame(QByteArrayView bytes);

}  // namespace flexraw::worker::protocol
