#include "linux_process_memory_probe.h"

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

// 목적: /proc status line에서 지정 field의 KiB 값을 byte 단위로 파싱
// 입력: line: /proc/self/status 한 줄, fieldName: "VmRSS:" 또는 "VmHWM:"
// 출력: field와 정수값이 유효하면 byte 수, 아니면 nullopt
[[nodiscard]] std::optional<std::uint64_t> parseProcessMemoryBytes(const std::string_view line,
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

    std::uint64_t valueKib = 0;
    const char* const begin = line.data() + firstDigit;
    const char* const end = line.data() + line.size();
    const std::from_chars_result parsed = std::from_chars(begin, end, valueKib);
    if (parsed.ec != std::errc{} || valueKib > std::numeric_limits<std::uint64_t>::max() / Kibibyte)
    {
        return std::nullopt;
    }
    return valueKib * Kibibyte;
}

}  // namespace

// 목적: Linux current process의 /proc/self/status memory 값을 Platform snapshot으로 변환
// 입력: 없음
// 출력: VmRSS/VmHWM 조회 성공 시 resident/peak resident byte snapshot
std::optional<ProcessMemorySnapshot> LinuxProcessMemoryProbe::snapshot() const
{
    std::ifstream status("/proc/self/status");
    if (!status)
    {
        return std::nullopt;
    }

    std::optional<std::uint64_t> residentBytes;
    std::optional<std::uint64_t> peakResidentBytes;
    std::string line;
    while (std::getline(status, line) && (!residentBytes.has_value() || !peakResidentBytes.has_value()))
    {
        const std::string_view view(line);
        if (!residentBytes.has_value())
        {
            residentBytes = parseProcessMemoryBytes(view, "VmRSS:");
        }
        if (!peakResidentBytes.has_value())
        {
            peakResidentBytes = parseProcessMemoryBytes(view, "VmHWM:");
        }
    }

    if (!residentBytes.has_value() || !peakResidentBytes.has_value() || *residentBytes == 0 ||
        *peakResidentBytes < *residentBytes)
    {
        return std::nullopt;
    }
    return ProcessMemorySnapshot{*residentBytes, *peakResidentBytes};
}

}  // namespace flexraw::platform
