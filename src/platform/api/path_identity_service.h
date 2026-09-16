#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace flexraw::platform
{

struct TransientPathKey
{
    std::string bytes;

    // 목적: 같은 Platform service가 만든 process-local path key 비교
    // 입력: other: 비교할 transient key
    // 출력: 부모 identity와 leaf 비교 표현이 같으면 true
    [[nodiscard]] bool operator==(const TransientPathKey&) const noexcept = default;
};

class IPathIdentityService
{
public:
    virtual ~IPathIdentityService() = default;

    // 목적: 기존 부모 아래 file locator를 process-local 충돌 비교 key로 변환
    // 입력: absolutePath: 기존 parent directory와 leaf 이름을 가진 절대 경로
    // 출력: 부모 identity와 해당 directory의 이름 비교 규칙을 반영한 key, 판정 불가 시 nullopt
    [[nodiscard]] virtual std::optional<TransientPathKey> comparisonKey(
        const std::filesystem::path& absolutePath) const = 0;
};

}  // namespace flexraw::platform
