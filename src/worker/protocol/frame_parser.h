#pragma once

#include <optional>
#include <vector>

#include <QByteArray>
#include <QByteArrayView>

#include "protocol_contracts.h"
namespace flexraw::worker::protocol
{

struct ParseFramesOutcome
{
    std::vector<ProtocolFrame> frames;
    std::optional<ProtocolError> terminalError;
};

class FrameParser final
{
public:
    // 목적: 빈 incremental protocol parser 생성
    // 입력: 없음
    // 출력: frame header 또는 payload fragment를 받을 수 있는 parser
    FrameParser() = default;

    // 목적: socket에서 받은 임의 크기 fragment를 순서대로 소비
    // 입력: bytes: 이번 read에서 받은 byte view
    // 출력: 완성된 frame 목록과 이후 입력을 종료시키는 optional protocol 오류
    [[nodiscard]] ParseFramesOutcome append(QByteArrayView bytes);

    // 목적: buffered fragment와 terminal 오류를 제거해 parser 재사용
    // 입력: 없음
    // 출력: 없음
    void reset();

    // 목적: 현재 보관 중인 미완성 frame byte 수 확인
    // 입력: 없음
    // 출력: header와 payload를 합친 buffered byte 수
    [[nodiscard]] qsizetype bufferedByteCount() const;

    // 목적: protocol 위반 후 parser가 terminal 상태인지 확인
    // 입력: 없음
    // 출력: reset 전까지 추가 입력을 거부하는지 여부
    [[nodiscard]] bool isTerminated() const;

private:
    QByteArray m_buffer;
    std::optional<qsizetype> m_expectedFrameBytes;
    std::optional<ProtocolError> m_terminalError;
};

}  // namespace flexraw::worker::protocol
