#include "shell/app_processes.h"

#include <windows.h>
#include <tlhelp32.h>

#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

using namespace atrium;
namespace fs = std::filesystem;

namespace {
using Handle = std::unique_ptr<void, decltype(&CloseHandle)>;

void Require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

Handle Spawn(const fs::path& exe, const std::vector<std::wstring>& arguments)
{
    std::wstring command = L"\"" + exe.wstring() + L"\"";
    for (const auto& argument : arguments)
        command += L" \"" + argument + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    Require(CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
        nullptr, nullptr, &startup, &process) != FALSE, "start isolated probe process");
    CloseHandle(process.hThread);
    return Handle(process.hProcess, CloseHandle);
}

Handle FindProcess(const fs::path& exe)
{
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0), CloseHandle);
    Require(snapshot.get() != INVALID_HANDLE_VALUE, "snapshot probe processes");
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL more = Process32FirstW(snapshot.get(), &entry); more; more = Process32NextW(snapshot.get(), &entry)) {
        Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE, FALSE, entry.th32ProcessID), CloseHandle);
        wchar_t path[32768]{};
        DWORD length = ARRAYSIZE(path);
        if (process && WaitForSingleObject(process.get(), 0) == WAIT_TIMEOUT &&
            QueryFullProcessImageNameW(process.get(), 0, path, &length) && _wcsicmp(path, exe.c_str()) == 0)
            return process;
    }
    return Handle(nullptr, CloseHandle);
}

struct Fixture {
    fs::path root;
    std::vector<fs::path> copies;
    std::vector<Handle> children;

    Fixture()
    {
        wchar_t temporary[MAX_PATH]{};
        Require(GetTempPathW(ARRAYSIZE(temporary), temporary) != 0, "read temporary directory");
        root = fs::path(temporary) / (L"Atrium-process-tests-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        fs::create_directory(root);
    }

    fs::path Copy(const fs::path& source, const wchar_t* directory, const wchar_t* name)
    {
        const auto folder = root / directory;
        fs::create_directory(folder);
        const auto destination = folder / name;
        fs::copy_file(source, destination);
        copies.push_back(destination);
        return destination;
    }

    ~Fixture()
    {
        // Cleanup only disposable probe copies, even if a check failed halfway through.
        for (auto& process : children) {
            TerminateProcess(process.get(), ERROR_PROCESS_ABORTED);
            WaitForSingleObject(process.get(), 5000);
        }
        for (const auto& copy : copies)
            while (auto process = FindProcess(copy)) {
                TerminateProcess(process.get(), ERROR_PROCESS_ABORTED);
                if (WaitForSingleObject(process.get(), 5000) != WAIT_OBJECT_0)
                    break;
            }
        std::error_code ignored;
        for (const auto& copy : copies) {
            fs::remove(copy, ignored);
            fs::remove(copy.parent_path(), ignored);
        }
        fs::remove(root, ignored);
    }
};

void CheckTermination(const fs::path& self)
{
    Require(!CanTerminateApp({}), "missing identity is disabled");
    Require(!CanTerminateApp({L"C:\\Windows\\EXPLORER.EXE", {}}), "Explorer is protected regardless of case");
    Require(!CanTerminateApp({L"C:\\Windows\\System32\\ApplicationFrameHost.exe", L"app!id"}), "shared UWP host is protected");
    Require(CanTerminateApp({{}, L"package!app"}), "packaged apps can use AUMID alone");

    Fixture fixture;
    const auto app = fixture.Copy(self, L"app", L"probe.exe");
    // The app starts a helper from its own folder, which starts another program from elsewhere.
    const auto helper = fixture.Copy(self, L"app\\bin", L"helper.exe");
    const auto grandchild = fixture.Copy(self, L"game", L"background.exe");
    const auto unrelated = fixture.Copy(self, L"unrelated", L"probe.exe");
    const auto shell = fixture.Copy(self, L"shell", L"explorer.exe");
    const auto eventName = L"Local\\AtriumProcessTests-" + std::to_wstring(GetCurrentProcessId());
    Handle ready(CreateEventW(nullptr, TRUE, FALSE, eventName.c_str()), CloseHandle);
    Require(bool(ready), "create probe readiness event");
    fixture.children.push_back(Spawn(app, {L"--probe", eventName, helper.wstring(), grandchild.wstring()}));
    Require(WaitForSingleObject(ready.get(), 5000) == WAIT_OBJECT_0, "helper tree is ready");
    fixture.children.push_back(FindProcess(helper));
    fixture.children.push_back(FindProcess(grandchild));
    Require(bool(fixture.children[1]) && bool(fixture.children[2]), "capture helper tree handles");
    fixture.children.push_back(Spawn(app, {L"--probe", eventName}));
    fixture.children.push_back(Spawn(unrelated, {L"--probe", eventName}));
    fixture.children.push_back(Spawn(shell, {L"--probe", eventName}));

    std::wstring upperPath = app.wstring();
    CharUpperBuffW(upperPath.data(), DWORD(upperPath.size()));
    const AppProcessIdentity target{upperPath, {}};
    Require(TerminateAppProcesses({&target, 1}), "end application and background processes");
    for (size_t i : {0, 1, 3})
        Require(WaitForSingleObject(fixture.children[i].get(), 5000) == WAIT_OBJECT_0, "app, its helper and background instance exit");
    Require(WaitForSingleObject(fixture.children[2].get(), 0) == WAIT_TIMEOUT, "a program the app started from elsewhere stays alive");
    Require(WaitForSingleObject(fixture.children[4].get(), 0) == WAIT_TIMEOUT, "same filename in another directory stays alive");

    const AppProcessIdentity shellTarget{shell.wstring(), {}};
    Require(TerminateAppProcesses({&shellTarget, 1}), "protected hosts are skipped");
    Require(WaitForSingleObject(fixture.children[5].get(), 0) == WAIT_TIMEOUT, "Explorer process is never terminated");
    const AppProcessIdentity selfTarget{self.wstring(), {}};
    Require(TerminateAppProcesses({&selfTarget, 1}), "current process is protected");
    Require(WaitForSingleObject(fixture.children[4].get(), 0) == WAIT_TIMEOUT, "current process descendants stay alive when it is skipped");
}
} // namespace

int wmain(int argc, wchar_t** argv)
{
    try {
        if (argc >= 3 && std::wstring(argv[1]) == L"--probe") {
            if (argc > 3) {
                std::vector<std::wstring> arguments{L"--probe", argv[2]};
                for (int i = 4; i < argc; ++i)
                    arguments.push_back(argv[i]);
                auto child = Spawn(argv[3], arguments);
            } else {
                Handle ready(OpenEventW(EVENT_MODIFY_STATE, FALSE, argv[2]), CloseHandle);
                Require(ready && SetEvent(ready.get()), "signal probe readiness");
            }
            Sleep(INFINITE);
            return 0;
        }
        wchar_t self[32768]{};
        Require(GetModuleFileNameW(nullptr, self, ARRAYSIZE(self)) != 0, "read test executable path");
        CheckTermination(self);
        std::cout << "PASS: application termination, background helpers and process isolation\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
