#pragma once

#include <cstdint>
#include <optional>

namespace flexraw::platform
{

struct SystemMemorySnapshot
{
    std::uint64_t totalBytes{0};
    std::uint64_t availableBytes{0};
};

class ISystemMemoryProbe
{
public:
    virtual ~ISystemMemoryProbe() = default;

    // 목적: 현재 host의 total/available physical memory snapshot 조회
    // 입력: 없음
    // 출력: OS 조회 성공 시 byte 단위 snapshot, 실패 또는 미지원 시 nullopt
    [[nodiscard]] virtual std::optional<SystemMemorySnapshot> snapshot() const = 0;
};

}  // namespace flexraw::platform
