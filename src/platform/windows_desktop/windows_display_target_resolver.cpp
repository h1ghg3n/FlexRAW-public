#include "windows_display_target_resolver.h"

#include <algorithm>
#include <string>

#include <QtGui/qscreen_platform.h>

#include <QByteArray>
#include <QScreen>
#include <QString>

// Windows SDK contract상 wingdi.h와 winuser.h보다 windows.h가 먼저 와야 한다.
// clang-format off
#include <windows.h>
// clang-format on

namespace flexraw::platform
{

// 목적: Windows native display device name과 정확히 하나 일치하는 opaque target identity 선택
// 입력: deviceName: MONITORINFOEX device name, targets: 같은 profile service가 열거한 active target 목록
// 출력: unique exact match 또는 unknown/duplicate identity이면 빈 값
std::optional<DisplayTargetId> matchWindowsDisplayTargetId(std::string_view deviceName,
                                                           std::span<const DisplayTargetSnapshot> targets)
{
    const auto first = std::ranges::find(
        targets, deviceName, [](const DisplayTargetSnapshot& target) { return std::string_view(target.id.bytes); });
    if (first == targets.end())
    {
        return std::nullopt;
    }

    const auto duplicate =
        std::ranges::find(std::next(first), targets.end(), deviceName, [](const DisplayTargetSnapshot& target) {
            return std::string_view(target.id.bytes);
        });
    if (duplicate != targets.end())
    {
        return std::nullopt;
    }
    return first->id;
}

// 목적: Qt Windows screen의 native monitor를 기존 Platform display target identity로 대응
// 입력: screen: QWindow가 선택한 current screen, service: 같은 process의 display profile service
// 출력: unique target identity 또는 native handle/query/mapping 실패 시 빈 값
std::optional<DisplayTargetId> resolveWindowsDisplayTarget(const QScreen& screen, const IDisplayProfileService& service)
{
    const auto* nativeScreen = screen.nativeInterface<QNativeInterface::QWindowsScreen>();
    if (nativeScreen == nullptr || nativeScreen->handle() == nullptr)
    {
        return std::nullopt;
    }

    MONITORINFOEXW monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (GetMonitorInfoW(nativeScreen->handle(), reinterpret_cast<LPMONITORINFO>(&monitorInfo)) == 0)
    {
        return std::nullopt;
    }

    const QByteArray deviceName = QString::fromWCharArray(monitorInfo.szDevice).toUtf8();
    const std::optional<std::vector<DisplayTargetSnapshot>> targets = service.displayTargets();
    if (deviceName.isEmpty() || !targets.has_value())
    {
        return std::nullopt;
    }
    return matchWindowsDisplayTargetId(
        std::string_view(deviceName.constData(), static_cast<std::size_t>(deviceName.size())), *targets);
}

}  // namespace flexraw::platform
