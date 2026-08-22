#pragma once

#include <cstdint>
#include <optional>

namespace flexraw::platform
{

struct ProcessMemorySnapshot
{
    std::uint64_t residentBytes{0};
    std::uint64_t peakResidentBytes{0};
};

class IProcessMemoryProbe
{
public:
    virtual ~IProcessMemoryProbe() = default;

    // 목적: 현재 process의 resident와 process-lifetime peak resident memory 조회
    // 입력: 없음
    // 출력: OS 조회 성공 시 byte 단위 snapshot, 실패 또는 미지원 시 nullopt
    [[nodiscard]] virtual std::optional<ProcessMemorySnapshot> snapshot() const = 0;
};

}  // namespace flexraw::platform
