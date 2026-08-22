#include "frame_parser.h"

#include <algorithm>
#include <utility>

#include "frame_codec.h"

namespace flexraw::worker::protocol
{
namespace
{

// 목적: parser가 이미 종료된 경우 반환할 안정된 오류 생성
// 입력: terminalError: 최초 protocol 위반 오류
// 출력: 원래 분류와 진단을 보존한 parser 종료 오류
[[nodiscard]] ProtocolError makeTerminatedError(const std::optional<ProtocolError>& terminalError)
{
    if (terminalError.has_value())
    {
        return {ProtocolErrorCode::ParserTerminated,
                QStringLiteral("Protocol parser is terminated after: %1").arg(terminalError->message)};
    }
    return {ProtocolErrorCode::ParserTerminated, QStringLiteral("Protocol parser is terminated.")};
}

}  // namespace

// 목적: socket에서 받은 임의 크기 fragment를 순서대로 소비
// 입력: bytes: 이번 read에서 받은 byte view
// 출력: 완성된 frame 목록과 이후 입력을 종료시키는 optional protocol 오류
ParseFramesOutcome FrameParser::append(const QByteArrayView bytes)
{
    if (m_terminalError.has_value())
    {
        return {{}, makeTerminatedError(m_terminalError)};
    }

    ParseFramesOutcome outcome;
    qsizetype inputOffset = 0;
    while (inputOffset < bytes.size())
    {
        if (!m_expectedFrameBytes.has_value())
        {
            const qsizetype headerBytesNeeded = ProtocolHeaderBytes - m_buffer.size();
            const qsizetype availableBytes = bytes.size() - inputOffset;
            const qsizetype copyBytes = std::min(headerBytesNeeded, availableBytes);
            m_buffer.append(bytes.data() + inputOffset, copyBytes);
            inputOffset += copyBytes;

            if (m_buffer.size() < ProtocolHeaderBytes)
            {
                break;
            }

            const DecodeFrameHeaderResult headerResult = decodeFrameHeader(m_buffer);
            if (headerResult.hasError())
            {
                m_terminalError = headerResult.error();
                outcome.terminalError = m_terminalError;
                return outcome;
            }
            m_expectedFrameBytes = ProtocolHeaderBytes + static_cast<qsizetype>(headerResult.value().payloadBytes);
        }

        const qsizetype frameBytesNeeded = *m_expectedFrameBytes - m_buffer.size();
        const qsizetype availableBytes = bytes.size() - inputOffset;
        const qsizetype copyBytes = std::min(frameBytesNeeded, availableBytes);
        m_buffer.append(bytes.data() + inputOffset, copyBytes);
        inputOffset += copyBytes;

        if (m_buffer.size() < *m_expectedFrameBytes)
        {
            break;
        }

        DecodeFrameResult frameResult = decodeFrame(m_buffer);
        if (frameResult.hasError())
        {
            m_terminalError = frameResult.error();
            outcome.terminalError = m_terminalError;
            return outcome;
        }
        outcome.frames.push_back(std::move(frameResult.value()));
        m_buffer.clear();
        m_expectedFrameBytes.reset();
    }

    return outcome;
}

// 목적: buffered fragment와 terminal 오류를 제거해 parser 재사용
// 입력: 없음
// 출력: 없음
void FrameParser::reset()
{
    m_buffer.clear();
    m_expectedFrameBytes.reset();
    m_terminalError.reset();
}

// 목적: 현재 보관 중인 미완성 frame byte 수 확인
// 입력: 없음
// 출력: header와 payload를 합친 buffered byte 수
qsizetype FrameParser::bufferedByteCount() const
{
    return m_buffer.size();
}

// 목적: protocol 위반 후 parser가 terminal 상태인지 확인
// 입력: 없음
// 출력: reset 전까지 추가 입력을 거부하는지 여부
bool FrameParser::isTerminated() const
{
    return m_terminalError.has_value();
}

}  // namespace flexraw::worker::protocol
