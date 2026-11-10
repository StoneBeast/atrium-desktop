// Physical displays use device-interface IDs so a saved choice survives reconnection.
#pragma once

#include <windows.h>
#include "task_model.h"

#include <string>
#include <optional>
#include <vector>

namespace atrium {

struct DisplayInfo {
    HMONITOR monitor;
    std::wstring id;
    RECT bounds, work;
    bool primary;
};

std::vector<DisplayInfo> ConnectedDisplays();
// Empty or disconnected ID temporarily uses the primary display; the saved ID is retained.
HMONITOR ResolveDisplay(const std::vector<DisplayInfo>& displays, const std::wstring& id);
// No decision for other screens or empty frames; the first app on this screen decides.
std::optional<bool> WindowCoverOnMonitor(HMONITOR windowMonitor, const RECT& frame, bool maximized,
    HMONITOR monitor, const RECT& bounds);
bool AtDisplayBottom(POINT pointer, const RECT& bounds); // bottom edge, strictly inside this display
bool FilterTasksOnDisplay(const std::wstring& mode, bool primary);
std::vector<TaskItem> TaskItemsForDisplay(const std::vector<TaskItem>& items, const std::wstring& mode, bool primary,
    HMONITOR monitor, HWND foreground, const std::function<HMONITOR(HWND)>& monitorOf);

} // namespace atrium
