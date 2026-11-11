#include "shell/app_processes.h"

#include "log.h"

#include <windows.h>
#include <appmodel.h>
#include <tlhelp32.h>

#include <algorithm>
#include <memory>
#include <vector>

namespace atrium {
namespace {
using Handle = std::unique_ptr<void, decltype(&CloseHandle)>;

bool SharedHost(const std::wstring& path)
{
    const auto slash = path.find_last_of(L"\\/");
    const auto name = path.substr(slash == std::wstring::npos ? 0 : slash + 1);
    for (const auto* host : {L"explorer.exe", L"ApplicationFrameHost.exe", L"RuntimeBroker.exe", L"dllhost.exe", L"svchost.exe"})
        if (_wcsicmp(name.c_str(), host) == 0)
            return true;
    return false;
}

struct Process {
    Handle handle{nullptr, CloseHandle};
    std::wstring path;
    DWORD pid, parent;
    ULONGLONG created;
    bool selected;
};

bool Matches(HANDLE process, const std::wstring& path, std::span<const AppProcessIdentity> apps)
{
    wchar_t aumid[APPLICATION_USER_MODEL_ID_MAX_LENGTH]{};
    UINT32 length = ARRAYSIZE(aumid);
    const bool packaged = GetApplicationUserModelId(process, &length, aumid) == ERROR_SUCCESS;
    return std::any_of(apps.begin(), apps.end(), [&](const AppProcessIdentity& app) {
        if (!CanTerminateApp(app))
            return false;
        if (packaged && !app.aumid.empty())
            return _wcsicmp(aumid, app.aumid.c_str()) == 0;
        return !app.exePath.empty() && _wcsicmp(path.c_str(), app.exePath.c_str()) == 0;
    });
}
} // namespace

bool CanTerminateApp(const AppProcessIdentity& app)
{
    return (!app.exePath.empty() || !app.aumid.empty()) && !SharedHost(app.exePath);
}

bool TerminateAppProcesses(std::span<const AppProcessIdentity> apps)
{
    DWORD session = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &session))
        return false;
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0), CloseHandle);
    if (snapshot.get() == INVALID_HANDLE_VALUE) {
        snapshot.release();
        Log(L"tasks: process snapshot failed (%lu)", GetLastError());
        return false;
    }
    std::vector<Process> processes;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!Process32FirstW(snapshot.get(), &entry)) {
        Log(L"tasks: reading process snapshot failed (%lu)", GetLastError());
        return false;
    }
    do {
        DWORD processSession = 0;
        if (entry.th32ProcessID == GetCurrentProcessId() || !ProcessIdToSessionId(entry.th32ProcessID, &processSession) || processSession != session)
            continue;
        Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, entry.th32ProcessID), CloseHandle);
        if (!process)
            continue;
        std::wstring path(32768, L'\0');
        DWORD length = DWORD(path.size());
        if (!QueryFullProcessImageNameW(process.get(), 0, path.data(), &length))
            continue;
        path.resize(length);
        if (SharedHost(path))
            continue;
        FILETIME created{}, exited{}, kernel{}, user{};
        if (!GetProcessTimes(process.get(), &created, &exited, &kernel, &user))
            continue;
        const bool selected = Matches(process.get(), path, apps);
        processes.push_back({std::move(process), path, entry.th32ProcessID, entry.th32ParentProcessID,
            (ULONGLONG(created.dwHighDateTime) << 32) | created.dwLowDateTime, selected});
    } while (Process32NextW(snapshot.get(), &entry));
    // The app's folders: its helpers are the processes it started from them.
    std::vector<std::wstring> folders;
    for (const auto& process : processes)
        if (process.selected)
            folders.push_back(process.path.substr(0, process.path.find_last_of(L"\\/") + 1));
    auto inside = [&](const std::wstring& path) {
        return std::any_of(folders.begin(), folders.end(), [&](const std::wstring& folder) {
            return !folder.empty() && path.size() > folder.size() && _wcsnicmp(path.c_str(), folder.c_str(), folder.size()) == 0;
        });
    };
    // Select helpers before ending parents; creation times reject reused parent PIDs.
    bool changed;
    do {
        changed = false;
        for (auto& child : processes) {
            if (child.selected)
                continue;
            for (const auto& parent : processes)
                if (parent.selected && child.parent == parent.pid && child.created >= parent.created && inside(child.path)) {
                    child.selected = changed = true;
                    break;
                }
        }
    } while (changed);

    bool success = true;
    for (const auto& process : processes) {
        if (!process.selected || WaitForSingleObject(process.handle.get(), 0) == WAIT_OBJECT_0)
            continue;
        // Query handles stay alive until the end, keeping captured PIDs from being reused.
        Handle target(OpenProcess(PROCESS_TERMINATE, FALSE, process.pid), CloseHandle);
        if (!target || !TerminateProcess(target.get(), ERROR_PROCESS_ABORTED)) {
            Log(L"tasks: ending process %lu failed (%lu)", process.pid, GetLastError());
            success = false;
        } else {
            Log(L"tasks: ended app process %lu", process.pid);
        }
    }
    return success;
}

} // namespace atrium
