#pragma once

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>

#include "path_identity_service.h"

namespace flexraw::test
{

class TestPathIdentityService final : public platform::IPathIdentityService
{
public:
    // 목적: test에서 prospective leaf의 exact 또는 ASCII case-insensitive policy 선택
    // 입력: caseSensitive: leaf 대소문자를 구분하면 true
    // 출력: immutable deterministic test service
    explicit TestPathIdentityService(const bool caseSensitive) noexcept : m_caseSensitive(caseSensitive) {}

    // 목적: 기존 parent의 canonical path와 test case policy로 transient key 생성
    // 입력: absolutePath: 기존 parent와 prospective leaf를 가진 절대 경로
    // 출력: deterministic key, 입력이나 parent를 resolve할 수 없으면 nullopt
    [[nodiscard]] std::optional<platform::TransientPathKey> comparisonKey(
        const std::filesystem::path& absolutePath) const override
    {
        const std::filesystem::path normalizedPath = absolutePath.lexically_normal();
        if (!normalizedPath.is_absolute() || normalizedPath.filename().empty())
        {
            return std::nullopt;
        }

        std::error_code error;
        const std::filesystem::path parent = std::filesystem::canonical(normalizedPath.parent_path(), error);
        if (error)
        {
            return std::nullopt;
        }

        const std::u8string parentUtf8 = parent.generic_u8string();
        const std::u8string leafUtf8 = normalizedPath.filename().generic_u8string();
        platform::TransientPathKey key;
        key.bytes.assign(reinterpret_cast<const char*>(parentUtf8.data()), parentUtf8.size());
        key.bytes.push_back('\0');
        key.bytes.append(reinterpret_cast<const char*>(leafUtf8.data()), leafUtf8.size());
        if (!m_caseSensitive)
        {
            std::transform(key.bytes.begin(), key.bytes.end(), key.bytes.begin(), [](const unsigned char value) {
                return static_cast<char>(std::tolower(value));
            });
        }
        return key;
    }

private:
    bool m_caseSensitive{true};
};

// 목적: 일반 test에서 exact prospective path 의미를 공유
// 입력: 없음
// 출력: process lifetime 동안 유효한 immutable test service
[[nodiscard]] inline const platform::IPathIdentityService& testPathIdentityService()
{
    static const TestPathIdentityService service(true);
    return service;
}

// 목적: case-insensitive filesystem 충돌 회귀 test용 service 공유
// 입력: 없음
// 출력: process lifetime 동안 유효한 immutable test service
[[nodiscard]] inline const platform::IPathIdentityService& caseInsensitiveTestPathIdentityService()
{
    static const TestPathIdentityService service(false);
    return service;
}

}  // namespace flexraw::test
