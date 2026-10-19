#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace flexraw::platform
{

inline constexpr std::size_t MaximumDisplayProfileBytes = 16U * 1024U * 1024U;

struct DisplayTargetId
{
    std::string bytes;

    // 목적: 같은 Platform service가 열거한 process-local display target identity 비교
    // 입력: other: 비교할 display target identity
    // 출력: opaque identity byte가 같으면 true
    [[nodiscard]] bool operator==(const DisplayTargetId&) const noexcept = default;
};

struct DesktopBounds
{
    std::int32_t left{0};
    std::int32_t top{0};
    std::int32_t right{0};
    std::int32_t bottom{0};
};

struct DisplayTargetSnapshot
{
    DisplayTargetId id;
    DesktopBounds desktopBounds;
    bool primary{false};
};

enum class DisplayProfileUnavailableReason
{
    NoExplicitProfile,
    TargetUnavailable,
    QueryFailed,
    ReadFailed,
    PayloadTooLarge,
};

struct DisplayProfileSnapshot
{
    DisplayTargetId targetId;
    std::vector<std::uint8_t> iccProfile;
};

struct DisplayProfileUnavailable
{
    DisplayProfileUnavailableReason reason{DisplayProfileUnavailableReason::QueryFailed};
};

using DisplayProfileResult = std::variant<DisplayProfileSnapshot, DisplayProfileUnavailable>;

class IDisplayProfileService
{
public:
    virtual ~IDisplayProfileService() = default;

    // 목적: 현재 active display target을 process-local identity와 desktop bounds로 열거
    // 입력: 없음
    // 출력: 열거 성공 시 target snapshot 목록, OS query 실패 시 nullopt
    [[nodiscard]] virtual std::optional<std::vector<DisplayTargetSnapshot>> displayTargets() const = 0;

    // 목적: 열거된 display target의 현재 SDR ICC profile byte snapshot 조회
    // 입력: targetId: 같은 service의 최근 displayTargets 호출에서 받은 opaque identity
    // 출력: bounded ICC payload 또는 fallback과 diagnostics에 사용할 unavailable reason
    [[nodiscard]] virtual DisplayProfileResult displayProfile(const DisplayTargetId& targetId) const = 0;
};

}  // namespace flexraw::platform
