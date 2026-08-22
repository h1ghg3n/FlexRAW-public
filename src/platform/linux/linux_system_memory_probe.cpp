#include "linux_system_memory_probe.h"

#include <charconv>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>

namespace flexraw::platform
{
namespace
{

constexpr std::uint64_t Kibibyte = 1024;

// 목적: meminfo line에서 지정 field의 KiB 정수값 파싱
// 입력: line: /proc/meminfo 한 줄, fieldName: "MemTotal:" 같은 field
// 출력: field가 일치하고 범위 내 숫자가 있으면 KiB 값
[[nodiscard]] std::optional<std::uint64_t> parseMeminfoValue(const std::string_view line,
                                                             const std::string_view fieldName)
{
    if (!line.starts_with(fieldName))
    {
        return std::nullopt;
    }

    const std::size_t firstDigit = line.find_first_of("0123456789", fieldName.size());
    if (firstDigit == std::string_view::npos)
    {
        return std::nullopt;
    }

    std::uint64_t value = 0;
    const char* const begin = line.data() + firstDigit;
    const char* const end = line.data() + line.size();
    const std::from_chars_result parsed = std::from_chars(begin, end, value);
    if (parsed.ec != std::errc{} || value > std::numeric_limits<std::uint64_t>::max() / Kibibyte)
    {
        return std::nullopt;
    }
    return value;
}

}  // namespace

// 목적: /proc/meminfo의 MemTotal/MemAvailable을 Platform snapshot으로 변환
// 입력: 없음
// 출력: 두 값이 유효하면 byte 단위 snapshot, 읽기 실패면 nullopt
std::optional<SystemMemorySnapshot> LinuxSystemMemoryProbe::snapshot() const
{
    std::ifstream meminfo("/proc/meminfo");
    if (!meminfo)
    {
        return std::nullopt;
    }

    std::optional<std::uint64_t> totalKib;
    std::optional<std::uint64_t> availableKib;
    std::string line;
    while (std::getline(meminfo, line) && (!totalKib.has_value() || !availableKib.has_value()))
    {
        const std::string_view view(line);
        if (!totalKib.has_value())
        {
            totalKib = parseMeminfoValue(view, "MemTotal:");
        }
        if (!availableKib.has_value())
        {
            availableKib = parseMeminfoValue(view, "MemAvailable:");
        }
    }

    if (!totalKib.has_value() || !availableKib.has_value() || *availableKib > *totalKib)
    {
        return std::nullopt;
    }
    return SystemMemorySnapshot{*totalKib * Kibibyte, *availableKib * Kibibyte};
}

}  // namespace flexraw::platform
