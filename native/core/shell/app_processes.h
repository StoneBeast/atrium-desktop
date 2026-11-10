// Force-quit an application's processes, including its background helpers.
#pragma once

#include <span>
#include <string>

namespace atrium {

struct AppProcessIdentity {
    std::wstring exePath;
    std::wstring aumid;
};

// Shared Windows hosts cannot be ended as if they were an application.
bool CanTerminateApp(const AppProcessIdentity& app);
bool TerminateAppProcesses(std::span<const AppProcessIdentity> apps);

} // namespace atrium
