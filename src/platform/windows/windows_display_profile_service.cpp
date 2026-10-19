#include "windows_display_profile_service.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

// Windows SDK contract상 wingdi.h와 winuser.h보다 windows.h가 먼저 와야 한다.
// clang-format off
#include <windows.h>
// clang-format on

namespace flexraw::platform
{
namespace
{

struct DisplayEnumerationState
{
    std::vector<DisplayTargetSnapshot> targets;
    bool failed{false};
};

struct DeviceContextDeleter
{
    // 목적: CreateDCW로 얻은 display device context 해제
    // 입력: deviceContext: 해제할 Win32 display device context
    // 출력: 유효한 handle이면 DeleteDC 호출
    void operator()(HDC deviceContext) const noexcept
    {
        if (deviceContext != nullptr)
        {
            DeleteDC(deviceContext);
        }
    }
};

using DeviceContextHandle = std::unique_ptr<std::remove_pointer_t<HDC>, DeviceContextDeleter>;

// 목적: Win32 UTF-16 display identity를 Platform opaque UTF-8 byte로 변환
// 입력: value: null-terminated UTF-16 display identity
// 출력: 변환 성공 시 UTF-8 문자열, invalid input 또는 변환 실패 시 nullopt
[[nodiscard]] std::optional<std::string> wideToUtf8(const wchar_t* value)
{
    if (value == nullptr || value[0] == L'\0')
    {
        return std::nullopt;
    }

    const int requiredBytes =
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, nullptr, 0, nullptr, nullptr);
    if (requiredBytes <= 1)
    {
        return std::nullopt;
    }

    std::string converted(static_cast<std::size_t>(requiredBytes), '\0');
    const int convertedBytes = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, converted.data(), requiredBytes, nullptr, nullptr);
    if (convertedBytes != requiredBytes)
    {
        return std::nullopt;
    }
    converted.pop_back();
    return converted;
}

// 목적: Platform opaque UTF-8 display identity를 Win32 UTF-16 device name으로 복원
// 입력: value: displayTargets가 반환한 UTF-8 identity byte
// 출력: 변환 성공 시 null 없는 UTF-16 문자열, invalid input 또는 변환 실패 시 nullopt
[[nodiscard]] std::optional<std::wstring> utf8ToWide(const std::string& value)
{
    if (value.empty() || value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
    {
        return std::nullopt;
    }

    const int inputBytes = static_cast<int>(value.size());
    const int requiredCharacters =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), inputBytes, nullptr, 0);
    if (requiredCharacters <= 0)
    {
        return std::nullopt;
    }

    std::wstring converted(static_cast<std::size_t>(requiredCharacters), L'\0');
    const int convertedCharacters = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), inputBytes, converted.data(), requiredCharacters);
    if (convertedCharacters != requiredCharacters)
    {
        return std::nullopt;
    }
    return converted;
}

// 목적: EnumDisplayMonitors callback에서 active monitor metadata를 Platform snapshot으로 누적
// 입력: monitor: current monitor handle, unused DC/rect, userData: DisplayEnumerationState pointer
// 출력: 계속 열거하면 TRUE, metadata/identity 변환 실패로 중단하면 FALSE
BOOL CALLBACK collectDisplayTarget(HMONITOR monitor, HDC, LPRECT, LPARAM userData)
{
    auto* state = reinterpret_cast<DisplayEnumerationState*>(userData);
    if (state == nullptr)
    {
        return FALSE;
    }

    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor, reinterpret_cast<LPMONITORINFO>(&info)) == 0)
    {
        state->failed = true;
        return FALSE;
    }

    const std::optional<std::string> id = wideToUtf8(info.szDevice);
    if (!id.has_value())
    {
        state->failed = true;
        return FALSE;
    }

    state->targets.push_back(
        {DisplayTargetId{*id},
         DesktopBounds{info.rcMonitor.left, info.rcMonitor.top, info.rcMonitor.right, info.rcMonitor.bottom},
         (info.dwFlags & MONITORINFOF_PRIMARY) != 0});
    return TRUE;
}

// 목적: Win32 ICC query error가 explicit profile 부재를 뜻하는지 판정
// 입력: error: GetICMProfileW 실패 직후 GetLastError 값
// 출력: profile read failure가 아니라 sRGB fallback 가능한 부재 상태이면 true
[[nodiscard]] bool isNoExplicitProfileError(DWORD error) noexcept
{
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND || error == ERROR_NOT_FOUND ||
           error == ERROR_NO_MORE_ITEMS;
}

// 목적: display device context에서 현재 output ICC profile path 조회
// 입력: deviceName: active Windows display device name
// 출력: profile path 또는 query 상태를 구분한 unavailable reason
[[nodiscard]] std::variant<std::filesystem::path, DisplayProfileUnavailable> queryProfilePath(
    const std::wstring& deviceName)
{
    DeviceContextHandle deviceContext(CreateDCW(L"DISPLAY", deviceName.c_str(), nullptr, nullptr));
    if (deviceContext == nullptr)
    {
        return DisplayProfileUnavailable{DisplayProfileUnavailableReason::TargetUnavailable};
    }

    DWORD requiredCharacters = 0;
    SetLastError(ERROR_SUCCESS);
    const BOOL sizeResult = GetICMProfileW(deviceContext.get(), &requiredCharacters, nullptr);
    const DWORD sizeError = GetLastError();
    if (sizeResult == 0 && !(sizeError == ERROR_INSUFFICIENT_BUFFER && requiredCharacters > 0))
    {
        const DisplayProfileUnavailableReason reason = isNoExplicitProfileError(sizeError)
                                                           ? DisplayProfileUnavailableReason::NoExplicitProfile
                                                           : DisplayProfileUnavailableReason::QueryFailed;
        return DisplayProfileUnavailable{reason};
    }
    if (requiredCharacters == 0)
    {
        return DisplayProfileUnavailable{DisplayProfileUnavailableReason::NoExplicitProfile};
    }

    std::vector<wchar_t> profilePath(requiredCharacters, L'\0');
    if (GetICMProfileW(deviceContext.get(), &requiredCharacters, profilePath.data()) == 0)
    {
        const DWORD pathError = GetLastError();
        const DisplayProfileUnavailableReason reason = isNoExplicitProfileError(pathError)
                                                           ? DisplayProfileUnavailableReason::NoExplicitProfile
                                                           : DisplayProfileUnavailableReason::QueryFailed;
        return DisplayProfileUnavailable{reason};
    }
    if (profilePath.front() == L'\0')
    {
        return DisplayProfileUnavailable{DisplayProfileUnavailableReason::NoExplicitProfile};
    }
    return std::filesystem::path(profilePath.data());
}

// 목적: ICC profile file을 contract maximum 이하의 owned byte snapshot으로 읽기
// 입력: targetId: 결과 identity, profilePath: Win32가 반환한 profile file path
// 출력: bounded ICC payload 또는 read/size failure reason
[[nodiscard]] DisplayProfileResult readProfile(const DisplayTargetId& targetId,
                                               const std::filesystem::path& profilePath)
{
    std::error_code sizeError;
    const std::uintmax_t profileSize = std::filesystem::file_size(profilePath, sizeError);
    if (sizeError || profileSize == 0)
    {
        return DisplayProfileUnavailable{DisplayProfileUnavailableReason::ReadFailed};
    }
    if (profileSize > MaximumDisplayProfileBytes)
    {
        return DisplayProfileUnavailable{DisplayProfileUnavailableReason::PayloadTooLarge};
    }

    std::ifstream stream(profilePath, std::ios::binary);
    if (!stream)
    {
        return DisplayProfileUnavailable{DisplayProfileUnavailableReason::ReadFailed};
    }

    std::vector<std::uint8_t> profile(static_cast<std::size_t>(profileSize));
    stream.read(reinterpret_cast<char*>(profile.data()), static_cast<std::streamsize>(profile.size()));
    if (!stream || stream.gcount() != static_cast<std::streamsize>(profile.size()))
    {
        return DisplayProfileUnavailable{DisplayProfileUnavailableReason::ReadFailed};
    }
    return DisplayProfileSnapshot{targetId, std::move(profile)};
}

}  // namespace

// 목적: Windows active monitor를 process-local display target snapshot으로 열거
// 입력: 없음
// 출력: 열거 성공 시 target snapshot 목록, Win32 query 실패 시 nullopt
std::optional<std::vector<DisplayTargetSnapshot>> WindowsDisplayProfileService::displayTargets() const
{
    DisplayEnumerationState state;
    if (EnumDisplayMonitors(nullptr, nullptr, collectDisplayTarget, reinterpret_cast<LPARAM>(&state)) == 0 ||
        state.failed)
    {
        return std::nullopt;
    }

    std::ranges::sort(state.targets, {}, [](const DisplayTargetSnapshot& target) {
        return std::tuple{target.desktopBounds.left, target.desktopBounds.top, target.id.bytes};
    });
    return state.targets;
}

// 목적: Windows display device context의 현재 SDR ICC profile을 bounded byte snapshot으로 조회
// 입력: targetId: 같은 service가 열거한 Windows display target identity
// 출력: ICC payload 또는 profile/target/query/read 상태를 구분한 unavailable reason
DisplayProfileResult WindowsDisplayProfileService::displayProfile(const DisplayTargetId& targetId) const
{
    const std::optional<std::vector<DisplayTargetSnapshot>> targets = displayTargets();
    if (!targets.has_value())
    {
        return DisplayProfileUnavailable{DisplayProfileUnavailableReason::QueryFailed};
    }

    const auto target = std::ranges::find(*targets, targetId, &DisplayTargetSnapshot::id);
    if (target == targets->end())
    {
        return DisplayProfileUnavailable{DisplayProfileUnavailableReason::TargetUnavailable};
    }

    const std::optional<std::wstring> deviceName = utf8ToWide(targetId.bytes);
    if (!deviceName.has_value())
    {
        return DisplayProfileUnavailable{DisplayProfileUnavailableReason::TargetUnavailable};
    }

    const auto profilePath = queryProfilePath(*deviceName);
    if (const auto* unavailable = std::get_if<DisplayProfileUnavailable>(&profilePath))
    {
        return *unavailable;
    }
    return readProfile(targetId, std::get<std::filesystem::path>(profilePath));
}

}  // namespace flexraw::platform
