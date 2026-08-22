#pragma once

#include <utility>
#include <variant>

namespace flexraw::core::types {

template <typename T, typename E>
class Result
{
public:
    // 목적: 성공 값을 담은 Result 객체 생성
    // 입력: value: 호출 결과로 전달할 성공 값
    // 출력: 성공 상태의 Result 객체
    static Result success(T value)
    {
        return Result(std::move(value));
    }

    // 목적: 실패 값을 담은 Result 객체 생성
    // 입력: error: 호출 결과로 전달할 실패 정보
    // 출력: 실패 상태의 Result 객체
    static Result failure(E error)
    {
        return Result(std::move(error));
    }

    // 목적: Result 가 성공 값을 보유하는지 확인
    // 입력: 없음
    // 출력: 성공 값 보유 여부
    [[nodiscard]] bool hasValue() const
    {
        return std::holds_alternative<T>(m_storage);
    }

    // 목적: Result 가 실패 값을 보유하는지 확인
    // 입력: 없음
    // 출력: 실패 값 보유 여부
    [[nodiscard]] bool hasError() const
    {
        return std::holds_alternative<E>(m_storage);
    }

    // 목적: 성공 값에 대한 읽기 전용 참조 반환
    // 입력: 없음
    // 출력: Result 에 저장된 성공 값 참조
    [[nodiscard]] const T& value() const
    {
        return std::get<T>(m_storage);
    }

    // 목적: 성공 값에 대한 수정 가능 참조 반환
    // 입력: 없음
    // 출력: Result 에 저장된 성공 값 참조
    [[nodiscard]] T& value()
    {
        return std::get<T>(m_storage);
    }

    // 목적: 실패 값에 대한 읽기 전용 참조 반환
    // 입력: 없음
    // 출력: Result 에 저장된 실패 값 참조
    [[nodiscard]] const E& error() const
    {
        return std::get<E>(m_storage);
    }

private:
    // 목적: 성공 값을 내부 storage 에 저장
    // 입력: value: 호출 결과로 전달할 성공 값
    // 출력: 없음
    explicit Result(T value)
        : m_storage(std::move(value))
    {
    }

    // 목적: 실패 값을 내부 storage 에 저장
    // 입력: error: 호출 결과로 전달할 실패 정보
    // 출력: 없음
    explicit Result(E error)
        : m_storage(std::move(error))
    {
    }

    std::variant<T, E> m_storage;
};

} // namespace flexraw::core::types
