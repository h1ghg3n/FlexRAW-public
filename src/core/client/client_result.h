#pragma once

#include <utility>
#include <variant>

namespace flexraw::core::client
{

template<typename T, typename E> class ClientResult final
{
public:
    // 목적: Qt-free client 성공 값을 보유한 result 생성
    // 입력: value: consumer에게 전달할 immutable 의미 값
    // 출력: 성공 상태의 ClientResult
    [[nodiscard]] static ClientResult success(T value)
    {
        return ClientResult(std::move(value));
    }

    // 목적: Qt-free client 실패 값을 보유한 result 생성
    // 입력: error: consumer가 분류·진단할 구조화된 오류
    // 출력: 실패 상태의 ClientResult
    [[nodiscard]] static ClientResult failure(E error)
    {
        return ClientResult(std::move(error));
    }

    // 목적: 성공 값 보유 여부 확인
    // 입력: 없음
    // 출력: 성공 값이 있으면 true
    [[nodiscard]] bool hasValue() const noexcept
    {
        return std::holds_alternative<T>(m_storage);
    }

    // 목적: 실패 값 보유 여부 확인
    // 입력: 없음
    // 출력: 오류 값이 있으면 true
    [[nodiscard]] bool hasError() const noexcept
    {
        return std::holds_alternative<E>(m_storage);
    }

    // 목적: 보유한 성공 값의 읽기 전용 참조 반환
    // 입력: 없음
    // 출력: 성공 값 참조
    [[nodiscard]] const T& value() const
    {
        return std::get<T>(m_storage);
    }

    // 목적: 보유한 실패 값의 읽기 전용 참조 반환
    // 입력: 없음
    // 출력: 오류 값 참조
    [[nodiscard]] const E& error() const
    {
        return std::get<E>(m_storage);
    }

private:
    // 목적: 성공 variant로 ClientResult 구성
    // 입력: value: 저장할 성공 값
    // 출력: success factory에서만 생성되는 result
    explicit ClientResult(T value) : m_storage(std::move(value)) {}

    // 목적: 실패 variant로 ClientResult 구성
    // 입력: error: 저장할 오류 값
    // 출력: failure factory에서만 생성되는 result
    explicit ClientResult(E error) : m_storage(std::move(error)) {}

    std::variant<T, E> m_storage;
};

}  // namespace flexraw::core::client
