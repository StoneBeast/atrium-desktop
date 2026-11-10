#include "shell/system_chrome.h"

#include "log.h"
#include "settings.h"

#include <shellapi.h>
#include <objbase.h>

#include <vector>

namespace atrium {

namespace {

// The shell's Do Not Disturb service (undocumented; the same one the Windows
// notification centre's button uses). After IUnknown: the selected profile as
// a CoTaskMemAlloc'd string, and setting it.
constexpr CLSID kQuietHoursSettings = {0xF53321FA, 0x34F8, 0x4B7F, {0xB9, 0xA3, 0x36, 0x18, 0x77, 0xCB, 0x94, 0xCF}};
constexpr IID kIQuietHoursSettings = {0x6BFF4732, 0x81EC, 0x4FFB, {0xAE, 0x67, 0xB6, 0xC1, 0xBC, 0x29, 0x63, 0x1F}};
struct IQuietHoursSettings : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE get_UserSelectedProfile(LPWSTR* profile) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_UserSelectedProfile(LPCWSTR profile) = 0;
};

constexpr wchar_t kUnrestricted[] = L"Microsoft.QuietHoursProfile.Unrestricted";
// Only alarms break through: no app's banner shows (priority apps would).
constexpr wchar_t kQuiet[] = L"Microsoft.QuietHoursProfile.AlarmsOnly";

SystemChrome* g_chrome = nullptr;

std::wstring StatePath()
{
    std::wstring p = SettingsPath();
    return p.substr(0, p.find_last_of(L'\\')) + L"\\system.json";
}

std::vector<HWND> Taskbars()
{
    std::vector<HWND> out;
    if (HWND main = FindWindowW(L"Shell_TrayWnd", nullptr))
        out.push_back(main);
    for (HWND w = nullptr; (w = FindWindowExW(nullptr, w, L"Shell_SecondaryTrayWnd", nullptr)) != nullptr;)
        out.push_back(w);
    return out;
}

bool IsTaskbar(HWND w)
{
    wchar_t cls[32] = {};
    GetClassNameW(w, cls, 32);
    return wcscmp(cls, L"Shell_TrayWnd") == 0 || wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0;
}

bool AutoHide()
{
    APPBARDATA abd{sizeof(abd)};
    return (SHAppBarMessage(ABM_GETSTATE, &abd) & ABS_AUTOHIDE) != 0;
}

void SetAutoHide(bool on)
{
    APPBARDATA abd{sizeof(abd)};
    abd.hWnd = FindWindowW(L"Shell_TrayWnd", nullptr);
    abd.lParam = on ? ABS_AUTOHIDE : ABS_ALWAYSONTOP;
    SHAppBarMessage(ABM_SETSTATE, &abd);
}

void ShowTaskbars()
{
    // Also used by the exception filter: enumerate without allocating.
    if (HWND w = FindWindowW(L"Shell_TrayWnd", nullptr))
        ShowWindow(w, SW_SHOWNA);
    for (HWND w = nullptr; (w = FindWindowExW(nullptr, w, L"Shell_SecondaryTrayWnd", nullptr)) != nullptr;)
        ShowWindow(w, SW_SHOWNA);
}

} // namespace

std::wstring QuietProfile()
{
    IQuietHoursSettings* qh = nullptr;
    if (FAILED(CoCreateInstance(kQuietHoursSettings, nullptr, CLSCTX_ALL, kIQuietHoursSettings, reinterpret_cast<void**>(&qh))) || !qh)
        return {};
    LPWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(qh->get_UserSelectedProfile(&p)) && p)
        out = p;
    CoTaskMemFree(p);
    qh->Release();
    return out;
}

bool SetQuietProfile(const wchar_t* profile)
{
    IQuietHoursSettings* qh = nullptr;
    if (FAILED(CoCreateInstance(kQuietHoursSettings, nullptr, CLSCTX_ALL, kIQuietHoursSettings, reinterpret_cast<void**>(&qh))) || !qh)
        return false;
    bool ok = SUCCEEDED(qh->put_UserSelectedProfile(profile));
    qh->Release();
    return ok && QuietProfile() == profile;
}

SystemChrome::SystemChrome()
{
    g_chrome = this;
    // Left over from a run that did not end cleanly: put it back first (the
    // settings then hide again what they ask for).
    UndoLeftover();
}

bool SystemChrome::UndoLeftover()
{
    std::wstring left = ReadTextFile(StatePath());
    if (left.empty())
        return false;
    Log(L"chrome: undoing the last run's changes: %s", left.c_str());
    if (left.find(L"\"autoHide\":true") != std::wstring::npos)
        SetAutoHide(false);
    ShowTaskbars();
    if (left.find(L"\"quiet\":true") != std::wstring::npos && QuietProfile() == kQuiet)
        SetQuietProfile(kUnrestricted);
    DeleteFileW(StatePath().c_str());
    return true;
}

void SystemChrome::ShowTaskbarsNow() { ShowTaskbars(); }

SystemChrome::~SystemChrome()
{
    RestoreForExit();
    g_chrome = nullptr;
}

void SystemChrome::RestoreForExit()
{
    if (exiting_) return;
    exiting_ = true;
    SetTaskbarHidden(false);
    SetBannersHidden(false);
}

void SystemChrome::SetTaskbarHidden(bool hidden)
{
    if (hidden && exiting_) return;
    if (hidden == taskbarHidden_)
        return;
    taskbarHidden_ = hidden;
    if (hidden) {
        // Auto-hide first: a hidden taskbar that is not auto-hide would still
        // keep its strip of the work area.
        if (!AutoHide()) {
            SetAutoHide(true);
            autoHideOurs_ = true;
        }
        Save();
        HideTaskbars();
        Watch();
    } else {
        if (hook_)
            UnhookWinEvent(hook_);
        hook_ = nullptr;
        hookedPid_ = 0;
        ShowTaskbars();
        if (autoHideOurs_)
            SetAutoHide(false);
        autoHideOurs_ = false;
        Save();
    }
    Log(L"chrome: system taskbar %s", hidden ? L"hidden" : L"shown");
}

void SystemChrome::SetBannersHidden(bool hidden)
{
    if (hidden && exiting_) return;
    if (hidden == quietOurs_)
        return;
    if (hidden) {
        std::wstring now = QuietProfile();
        if (now.empty()) {
            Log(L"chrome: Do Not Disturb is not available; banners stay");
            return;
        }
        if (now != kUnrestricted) {
            Log(L"chrome: Do Not Disturb is on already (%s); left as it is", now.c_str());
            return; // the user's own: not ours to turn off later
        }
        quietOurs_ = SetQuietProfile(kQuiet);
        Log(L"chrome: Do Not Disturb %s", quietOurs_ ? L"on (banners hidden)" : L"could not be turned on");
    } else {
        if (QuietProfile() == kQuiet)
            SetQuietProfile(kUnrestricted);
        quietOurs_ = false;
        Log(L"chrome: Do Not Disturb off (banners shown)");
    }
    Save();
}

void SystemChrome::Enforce()
{
    if (exiting_) return;
    if (taskbarHidden_) {
        HideTaskbars();
        Watch(); // Explorer may have been restarted
    }
    if (quietOurs_ && QuietProfile() != kQuiet) {
        // The user turned it off (or chose another mode): theirs from now on.
        Log(L"chrome: Do Not Disturb changed by the user; no longer Atrium's");
        quietOurs_ = false;
        Save();
    }
}

void SystemChrome::EmergencyRestore()
{
    if (!g_chrome || !g_chrome->taskbarHidden_.exchange(false))
        return;
    // Stop OnShow/Enforce hiding Explorer again before making it visible.
    // The exception can be on a worker, so these flags are atomic. Do not
    // log or save here; system.json still restores Do Not Disturb next start.
    if (g_chrome->autoHideOurs_.exchange(false))
        SetAutoHide(false);
    ShowTaskbars();
}

void SystemChrome::HideTaskbars()
{
    for (HWND w : Taskbars())
        if (IsWindowVisible(w))
            ShowWindow(w, SW_HIDE);
}

void SystemChrome::Watch()
{
    // Explorer shows its taskbar again by itself now and then: hide it the
    // moment it does.
    DWORD pid = 0;
    GetWindowThreadProcessId(FindWindowW(L"Shell_TrayWnd", nullptr), &pid);
    if (!pid || pid == hookedPid_)
        return;
    if (hook_)
        UnhookWinEvent(hook_);
    hook_ = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, nullptr, OnShow, pid, 0, WINEVENT_OUTOFCONTEXT);
    hookedPid_ = hook_ ? pid : 0;
}

void CALLBACK SystemChrome::OnShow(HWINEVENTHOOK, DWORD, HWND w, LONG obj, LONG, DWORD, DWORD)
{
    if (obj == OBJID_WINDOW && g_chrome && g_chrome->taskbarHidden_ && w && IsTaskbar(w)) {
        ShowWindow(w, SW_HIDE);
        Log(L"chrome: Explorer showed its taskbar; hidden again");
    }
}

void SystemChrome::Save() const
{
    if (!taskbarHidden_ && !quietOurs_) {
        DeleteFileW(StatePath().c_str());
        return;
    }
    std::wstring json = L"{\"taskbarHidden\":";
    json += taskbarHidden_ ? L"true" : L"false";
    json += L",\"autoHide\":";
    json += autoHideOurs_ ? L"true" : L"false";
    json += L",\"quiet\":";
    json += quietOurs_ ? L"true" : L"false";
    json += L"}";
    WriteTextFile(StatePath(), json);
}

} // namespace atrium
