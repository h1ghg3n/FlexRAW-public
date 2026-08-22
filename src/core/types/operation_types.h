#pragma once

#include <atomic>
#include <memory>

#include <QtTypes>

namespace flexraw::core::types
{

using RequestId = quint64;
using DevelopRevision = quint64;
using PreviewSequence = quint64;

class CancellationSource;

class CancellationToken final
{
public:
    // 목적: 공유 cancellation state가 취소 요청을 받았는지 확인
    // 입력: 없음
    // 출력: 취소가 요청됐으면 true
    [[nodiscard]] bool isCancellationRequested() const noexcept;

private:
    friend class CancellationSource;

    // 목적: CancellationSource가 소유한 공유 state를 읽는 token 생성
    // 입력: state: source와 공유할 atomic cancellation state
    // 출력: 읽기 전용 cancellation token
    explicit CancellationToken(std::shared_ptr<std::atomic_bool> state);

    std::shared_ptr<std::atomic_bool> m_state;
};

class CancellationSource final
{
public:
    // 목적: 취소되지 않은 새 request cancellation state 생성
    // 입력: 없음
    // 출력: 독립 cancellation source
    CancellationSource();

    // 목적: worker와 pipeline에 전달할 읽기 전용 token 생성
    // 입력: 없음
    // 출력: source와 state를 공유하는 token
    [[nodiscard]] CancellationToken token() const;

    // 목적: 공유 state에 cancellation 요청을 원자적으로 기록
    // 입력: 없음
    // 출력: 이후 모든 공유 token이 취소 상태를 관찰
    void requestCancellation() const noexcept;

private:
    std::shared_ptr<std::atomic_bool> m_state;
};

}  // namespace flexraw::core::types
