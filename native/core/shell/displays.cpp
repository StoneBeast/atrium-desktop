#include "displays.h"

#include <algorithm>

namespace atrium {

std::vector<DisplayInfo> ConnectedDisplays()
{
    std::vector<DisplayInfo> displays;
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR monitor, HDC, LPRECT, LPARAM data) -> BOOL {
        MONITORINFOEXW info{};
        info.cbSize = sizeof(info);
        if (!GetMonitorInfoW(monitor, &info))
            return TRUE;
        DISPLAY_DEVICEW device{};
        device.cb = sizeof(device);
        std::wstring id = info.szDevice;
        if (EnumDisplayDevicesW(info.szDevice, 0, &device, EDD_GET_DEVICE_INTERFACE_NAME) && device.DeviceID[0])
            id = device.DeviceID;
        auto& list = *reinterpret_cast<std::vector<DisplayInfo>*>(data);
        list.push_back({monitor, std::move(id), info.rcMonitor, info.rcWork, (info.dwFlags & MONITORINFOF_PRIMARY) != 0});
        return TRUE;
    }, reinterpret_cast<LPARAM>(&displays));
    return displays;
}

HMONITOR ResolveDisplay(const std::vector<DisplayInfo>& displays, const std::wstring& id)
{
    auto selected = std::find_if(displays.begin(), displays.end(), [&](const DisplayInfo& d) { return !id.empty() && d.id == id; });
    if (selected != displays.end())
        return selected->monitor;
    return MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
}

bool FilterTasksOnDisplay(const std::wstring& mode, bool primary)
{
    return mode == L"window" || (mode == L"primaryAndWindow" && !primary);
}

std::optional<bool> WindowCoverOnMonitor(HMONITOR windowMonitor, const RECT& frame, bool maximized,
    HMONITOR monitor, const RECT& bounds)
{
    if (windowMonitor != monitor || IsRectEmpty(&frame))
        return std::nullopt;
    return maximized || (frame.left <= bounds.left && frame.top <= bounds.top && frame.right >= bounds.right && frame.bottom >= bounds.bottom);
}

bool AtDisplayBottom(POINT pointer, const RECT& bounds)
{
    return PtInRect(&bounds, pointer) && pointer.y >= bounds.bottom - 2;
}

std::vector<TaskItem> TaskItemsForDisplay(const std::vector<TaskItem>& items, const std::wstring& mode, bool primary,
    HMONITOR monitor, HWND foreground, const std::function<HMONITOR(HWND)>& monitorOf)
{
    auto result = items;
    if (!FilterTasksOnDisplay(mode, primary))
        return result;
    // Keep pins, but restrict running dots, previews and active state to this display.
    for (TaskItem& item : result) {
        std::erase_if(item.windows, [&](HWND window) { return monitorOf(window) != monitor; });
        item.active = std::find(item.windows.begin(), item.windows.end(), foreground) != item.windows.end();
        if (item.windows.empty())
            item.attention = false;
    }
    std::erase_if(result, [](const TaskItem& item) { return !item.pinned && item.windows.empty(); });
    return result;
}

} // namespace atrium
