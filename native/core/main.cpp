// Atrium: the resident process. Owns the glass bar, its popups, the desktop
// widgets, the tray icon and the settings pipe. Explorer's taskbar stays
// unless the user hides it (setting hideTaskbar, shell/system_chrome.h; D37).

#include "bar.h"
#include "clock.h"
#include "log.h"
#include "resource.h"
#include "settings.h"
#include "settings_server.h"
#include "shell/app_catalog.h"
#include "shell/app_registration.h"
#include "shell/update_checker.h"
#include "shell/task_model.h"
#include "shell/notification_listener.h"
#include "shell/system_chrome.h"
#include "shell/system_status.h"
#include "shell/tray_model.h"
#include "shell/tray_broker.h"
#include "shell/win_key.h"
#include "shell/feishu_source.h"
#include "shell/wechat_source.h"
#include "ui/calendar_panel.h"
#include "ui/control_center.h"
#include "ui/desktop_widgets.h"
#include "ui/glass_menu.h"
#include "ui/window_preview.h"
#include "ui/notifications.h"
#include "ui/stage_manager.h"
#include "ui/start_menu.h"
#include "ui/tray_panel.h"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <UIAutomation.h>
#include <windowsx.h>

#include <winrt/Windows.Foundation.h>

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace {

using namespace atrium;

constexpr wchar_t kHostClass[] = L"AtriumCoreHost";
constexpr wchar_t kGuardMutex[] = L"Local\\Atrium.Guard"; // held by the guard while it runs
constexpr wchar_t kInstanceMutex[] = L"Local\\Atrium.Core";
constexpr UINT WM_TRAY = WM_APP + 2;
constexpr UINT WM_OPEN_SETTINGS = WM_APP + 3; // sent by a second Atrium.exe
constexpr UINT WM_CATALOG = WM_APP + 4;       // the app catalog has news
constexpr UINT WM_NOTICE = WM_APP + 5;        // lParam: Notice* from the listener thread
constexpr UINT WM_TRAY_ICONS = WM_APP + 6;    // the tray model has read the icons
constexpr UINT WM_VOLUME = WM_APP + 7;        // the default playback device's volume changed
constexpr UINT WM_STATUS = WM_APP + 10;       // lParam: SystemStatus*, read off the UI thread
constexpr UINT WM_UPDATE_READY = WM_APP + 11; // background package is validated and ready
constexpr UINT WM_WIN_KEY = WM_APP + 12;      // the Windows key alone (wParam 0, winKeyStart), Win + 1..9, 0 (1..10, winNumbers)
constexpr UINT_PTR kTrayTimer = 0x51;
constexpr UINT_PTR kStatusTimer = 0x52;       // the bar's status icon
constexpr UINT_PTR kTrayFocusTimer = 0x53;    // after a forwarded tray click
constexpr UINT_PTR kReadyTimer = 0x54;       // only during an update startup handshake
constexpr UINT kStatusMs = 3000;
constexpr UINT kTrayId = 1;

enum Command : UINT {
    kCmdSettings = 100, kCmdToggle, kCmdExit, kCmdNewWindow, kCmdPin, kCmdUnpin, kCmdClose, kCmdOpenLocation, kCmdDesktop, kCmdTaskManager, kCmdTerminate, kCmdTerminateConfirm,
    kCmdWidgets, kCmdStage, kCmdWidgetsModule
};

struct App {
    HINSTANCE instance = nullptr;
    HWND host = nullptr;
    UINT taskbarCreated = 0;
    bool trayAdded = false;
    std::unique_ptr<Bar> bar;
    std::vector<std::unique_ptr<Bar>> secondaryBars;
    Bar* activeBar = nullptr; // the last taskbar the pointer interacted with
    SystemStatus lastStatus;
    bool capturable = false;
    bool displaysReady = false;
    ULONGLONG switchedBarAt = 0; // switching displays must not suppress the first popup click
    std::unique_ptr<SystemStatusReader> status;
    std::unique_ptr<SettingsServer> server;
    std::unique_ptr<TaskModel> tasks;
    std::unique_ptr<AppCatalog> catalog;
    std::unique_ptr<StartMenu> start;
    std::unique_ptr<Notifications> notes;
    std::unique_ptr<NotificationListener> listener;
    std::unique_ptr<UpdateChecker> updates;
    std::unique_ptr<WeChatSource> wechat;
    std::unique_ptr<FeishuSource> feishu;
    std::unique_ptr<TrayModel> tray;
    std::unique_ptr<TrayPanel> trayPanel;
    std::unique_ptr<ControlCenter> control;
    std::unique_ptr<CalendarPanel> calendar;
    std::unique_ptr<DesktopWidgets> widgets;
    std::unique_ptr<SystemChrome> chrome;
    std::unique_ptr<GlassMenu> menu;
    std::unique_ptr<WindowPreview> preview;
    std::unique_ptr<StageManager> stage;
    std::unique_ptr<WinKey> winKey;
    HANDLE guard = nullptr; // the guard process (EnsureGuard)
    bool winKeyStart = false, winNumbers = false; // what it watches
    bool widgetsOn = false;
    std::wstring desktopDisplay; // effective setting, also available during display broadcasts
    Settings displaySettings;
    std::optional<bool> barOn; // the bar module, as last applied
    int brightness = -1; // the built-in screen's, from GUID_VIDEO_CURRENT_MONITOR_BRIGHTNESS
    HPOWERNOTIFY brightnessNotify = nullptr;
};

App g_app;
std::wstring g_readyEvent;

Bar& ActiveBar() { return *g_app.activeBar; }

template<class Action> void ForBars(Action action)
{
    action(*g_app.bar);
    for (const auto& bar : g_app.secondaryBars)
        action(*bar);
}

void ConfigureBar(Bar& bar);
void SyncDisplayBars(const Settings& settings, bool refresh = false);

// GUID_VIDEO_CURRENT_MONITOR_BRIGHTNESS: Windows reports every change of the
// built-in screen's brightness (keys, quick settings, our slider), and the
// current level once on registration.
constexpr GUID kBrightnessSetting = {0x8ffee2c6, 0x2d01, 0x46be, {0xad, 0xb9, 0x39, 0x8a, 0xdd, 0xc5, 0xb4, 0xff}};

std::wstring ExeDir()
{
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring p(path);
    return p.substr(0, p.find_last_of(L'\\'));
}

bool SameCoreExecutable(HWND window)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    wchar_t other[32768]{}, current[32768]{};
    DWORD length = DWORD(std::size(other));
    bool same = process && QueryFullProcessImageNameW(process, 0, other, &length) &&
                GetModuleFileNameW(nullptr, current, DWORD(std::size(current))) && _wcsicmp(other, current) == 0;
    if (process) CloseHandle(process);
    return same;
}

// page: the settings app opens on it ("widgets"); an open window just comes to the front.
void OpenSettings(const wchar_t* page = nullptr)
{
    std::wstring dir = ExeDir();
    std::vector<std::wstring> candidates = {dir + L"\\Settings\\Atrium.Settings.exe", dir + L"\\Atrium.Settings.exe"};
    // A development build (native\build\win-x64\Release): the packaged settings app of the same repository.
    for (std::wstring up = dir; up.size() > 3;) {
        up = up.substr(0, up.find_last_of(L'\\'));
        candidates.push_back(up + (IsDevelopmentBuild() ? L"\\dist\\Development\\Atrium\\Settings\\Atrium.Settings.exe"
                                                       : L"\\dist\\Atrium\\Settings\\Atrium.Settings.exe"));
    }
    for (const std::wstring& exe : candidates) {
        if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES)
            continue;
        // The settings app keeps a single instance itself and activates it.
        if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", exe.c_str(), page ? (std::wstring(L"--page ") + page).c_str() : nullptr, nullptr, SW_SHOWNORMAL)) > 32)
            return;
    }
    Log(L"settings: Atrium.Settings.exe not found next to %s", dir.c_str());
    MessageBoxW(nullptr, L"没有找到 Atrium.Settings.exe。请确认安装目录中的 Settings 文件夹完整。", L"Atrium", MB_ICONWARNING);
}

void AddTrayIcon()
{
    NOTIFYICONDATAW nid{sizeof(nid)};
    nid.hWnd = g_app.host;
    nid.uID = kTrayId;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = static_cast<HICON>(LoadImageW(g_app.instance, MAKEINTRESOURCEW(IDI_ATRIUM), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                              GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    wcscpy_s(nid.szTip, L"Atrium");
    g_app.trayAdded = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
    DestroyIcon(nid.hIcon);
    if (!g_app.trayAdded) {
        Log(L"tray: could not add Atrium's notification icon; will retry when the taskbar is ready");
        return;
    }
    nid.uVersion = NOTIFYICON_VERSION_4;
    if (!Shell_NotifyIconW(NIM_SETVERSION, &nid))
        Log(L"tray: could not set notification icon version");
    Log(L"tray: Atrium notification icon added");
}

void RemoveTrayIcon()
{
    NOTIFYICONDATAW nid{sizeof(nid)};
    nid.hWnd = g_app.host;
    nid.uID = kTrayId;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    g_app.trayAdded = false;
}

// Win+<key> (or Win+Ctrl+<key>), a public system shortcut (D11).
void SendWinShortcut(WORD key, bool ctrl = false)
{
    std::vector<INPUT> in;
    auto add = [&](WORD vk, bool up) {
        INPUT i{};
        i.type = INPUT_KEYBOARD;
        i.ki.wVk = vk;
        i.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
        in.push_back(i);
    };
    add(VK_LWIN, false);
    if (ctrl)
        add(VK_CONTROL, false);
    add(key, false);
    add(key, true);
    if (ctrl)
        add(VK_CONTROL, true);
    add(VK_LWIN, true);
    SendInput(UINT(in.size()), in.data(), sizeof(INPUT));
}

// The system tray's hidden icons. Win+B (public shortcut) puts the keyboard
// focus on the system taskbar's notification area; if that focus is the
// "show hidden icons" button, it is invoked through UI Automation. Otherwise
// (no hidden icons, other shell) the tray simply keeps the focus.
void OpenTray()
{
    SendWinShortcut('B');
    SetTimer(g_app.host, kTrayTimer, 350, nullptr);
}

void OpenTrayStep2()
{
    KillTimer(g_app.host, kTrayTimer);
    winrt::com_ptr<IUIAutomation> uia;
    winrt::com_ptr<IUIAutomationElement> focus;
    if (FAILED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(uia.put()))) ||
        FAILED(uia->GetFocusedElement(focus.put())) || !focus) {
        Log(L"tray: no focused element after Win+B");
        return;
    }
    BSTR id = nullptr;
    focus->get_CurrentAutomationId(&id);
    std::wstring automationId = id ? id : L"";
    SysFreeString(id);
    winrt::com_ptr<IUIAutomationInvokePattern> invoke;
    if (automationId == L"SystemTrayIcon" &&
        SUCCEEDED(focus->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(invoke.put()))) && invoke && SUCCEEDED(invoke->Invoke())) {
        Log(L"tray: opened the hidden icons");
        return;
    }
    Log(L"tray: focus is on '%s', left there (no hidden-icons button)", automationId.c_str());
}

void OpenSystem(const std::wstring& what)
{
    if (what == L"network" || what == L"volume" || what == L"battery") {
        g_app.control->Open(ActiveBar().EntryRect(L"#status"), ActiveBar().BarRect());
        g_app.notes->PanelsChanged();
        Log(L"start: system entry %s (control centre)", what.c_str());
        return;
    }
    if (what == L"lock") {
        LockWorkStation();
        return;
    }
    if (what == L"tray") {
        g_app.trayPanel->Open(ActiveBar().EntryRect(L"#tray"), ActiveBar().BarRect(), ActiveBar().PreviousForeground());
        g_app.notes->PanelsChanged();
        Log(L"start: system entry tray (tray panel)");
        return;
    }
    if (what == L"account")
        ShellExecuteW(nullptr, L"open", L"ms-settings:yourinfo", nullptr, nullptr, SW_SHOWNORMAL);
    Log(L"start: system entry %s", what.c_str());
}

void ToggleTray(RECT);
void ToggleControl(RECT);
void ToggleCalendar();
void UpdateStatus();
void UpdateStatus()
{
    if (g_app.status)
        g_app.status->Refresh();
}

// Atrium's tray panel and control centre open above their entries on the
// bar; a click on the entry while one is open closes it (and must not reopen
// it: the press that deactivated it is the same click).
void ToggleTray(RECT)
{
    if (g_app.trayPanel->Visible()) {
        g_app.trayPanel->Close();
        return;
    }
    if (GetTickCount64() - g_app.trayPanel->LastClosed() < 400 && g_app.trayPanel->LastClosed() > g_app.switchedBarAt)
        return;
    g_app.trayPanel->Open(ActiveBar().EntryRect(L"#tray"), ActiveBar().BarRect(), ActiveBar().PreviousForeground());
    g_app.notes->PanelsChanged();
}

void ToggleControl(RECT)
{
    if (g_app.control->Visible()) {
        g_app.control->Close();
        return;
    }
    if (GetTickCount64() - g_app.control->LastClosed() < 400 && g_app.control->LastClosed() > g_app.switchedBarAt)
        return;
    g_app.control->Open(ActiveBar().EntryRect(L"#status"), ActiveBar().BarRect());
    g_app.notes->PanelsChanged();
}

void ToggleCalendar()
{
    if (g_app.calendar->Visible()) {
        g_app.calendar->Close();
        return;
    }
    if (GetTickCount64() - g_app.calendar->LastClosed() < 400 && g_app.calendar->LastClosed() > g_app.switchedBarAt)
        return;
    g_app.calendar->Open(ActiveBar().EntryRect(L"#clock"), ActiveBar().BarRect());
    g_app.notes->PanelsChanged();
}

void ToggleStart(RECT)
{
    if (g_app.start->Visible()) {
        g_app.start->Close(false);
        return;
    }
    // A click on the logo while the menu is open first deactivates (closes) it;
    // that click must not open it again.
    if (GetTickCount64() - g_app.start->LastClosed() < 400 && g_app.start->LastClosed() > g_app.switchedBarAt)
        return;
    g_app.start->Open(ActiveBar().BarRect(), ActiveBar().EntryRect(L"#start"), ActiveBar().PreviousForeground());
    g_app.notes->PanelsChanged();
}

// The bar module on or off (its menu, the tray icon's). Off, its popups close and Windows' own
// taskbar and banners take over again (ApplySettings).
void SetBarModule(bool on)
{
    g_app.server->Modify([on](Settings& s) { s.bar = on; });
}

void SetWidgetsModule(bool on)
{
    g_app.server->Modify([on](Settings& s) { s.widgets = on; });
}

// Stage Manager from the bar (its menu, the control centre); the widgets go to the other side.
void SetStageManager(bool on)
{
    g_app.server->Modify([on](Settings& s) {
        s.stageManager = on;
        if (on)
            s.widgetsOnRight = !s.stageOnRight;
    });
}

void ShowMenu(POINT pt)
{
    std::vector<MenuEntry> entries = {{L"外观设置…", kCmdSettings}, {}, {L"显示桌面", kCmdDesktop}, {L"任务管理器", kCmdTaskManager}};
    if (g_app.widgetsOn)
        entries.push_back({L"添加小组件…", kCmdWidgets});
    // The three modules, each on its own.
    entries.push_back({});
    entries.push_back({L"任务栏", kCmdToggle, g_app.barOn.value_or(true)});
    entries.push_back({L"桌面小组件", kCmdWidgetsModule, g_app.widgetsOn});
    entries.push_back({L"台前调度", kCmdStage, g_app.stage->Enabled()});
    entries.push_back({});
    entries.push_back({L"退出 Atrium", kCmdExit});
    g_app.menu->Open(std::move(entries), pt, MenuPlace::Above, [](int cmd) {
        switch (cmd) {
        case kCmdSettings:
            OpenSettings();
            break;
        case kCmdWidgets:
            OpenSettings(L"widgets");
            break;
        case kCmdToggle:
            SetBarModule(!g_app.barOn.value_or(true));
            break;
        case kCmdWidgetsModule:
            SetWidgetsModule(!g_app.widgetsOn);
            break;
        case kCmdDesktop:
            SendWinShortcut('D');
            break;
        case kCmdStage: {
            SetStageManager(!g_app.stage->Enabled());
            break;
        }
        case kCmdTaskManager:
            ShellExecuteW(nullptr, L"open", L"taskmgr.exe", nullptr, nullptr, SW_SHOWNORMAL);
            break;
        case kCmdExit:
            DestroyWindow(g_app.host);
            break;
        }
    });
}

// The tray icon of a notice's sender: its program's file name or the start of
// its tooltip is the sender's name or id ("QQ" -> qq.exe, "QQ: 12345").
const TrayIcon* TrayIconOf(const Notice& n)
{
    static std::vector<TrayIcon> icons;
    icons = g_app.tray->Icons();
    auto lower = [](std::wstring s) {
        for (auto& c : s)
            c = wchar_t(towlower(c));
        return s;
    };
    std::vector<std::wstring> names;
    for (const std::wstring& s : {n.app, n.appId}) {
        std::wstring v = lower(s);
        if (size_t bang = v.find(L'!'); bang != std::wstring::npos) // packaged: "publisher.app_hash!App"
            v = v.substr(0, bang);
        if (size_t slash = v.find_last_of(L"\\/"); slash != std::wstring::npos) // a path
            v = v.substr(slash + 1);
        if (v.size() > 4 && v.ends_with(L".exe"))
            v.resize(v.size() - 4);
        if (v.size() >= 2)
            names.push_back(v);
    }
    for (const TrayIcon& icon : icons) {
        std::wstring stem = lower(icon.exe.substr(icon.exe.find_last_of(L'\\') + 1));
        if (stem.size() > 4 && stem.ends_with(L".exe"))
            stem.resize(stem.size() - 4);
        std::wstring tip = lower(TrayModel::Title(icon.tip));
        for (const std::wstring& name : names) {
            bool tipNames = tip.rfind(name, 0) == 0 && (tip.size() == name.size() || !iswalnum(tip[name.size()]));
            if ((!icon.exe.empty() && stem == name) || tipNames)
                return &icon;
        }
    }
    return nullptr;
}

// What clicking a notification does: open its link, or bring its app to the
// front (start it if it is not running).
bool RunNoticeAction(const Notice& n)
{
    if (n.action == L"settings:updates") {
        OpenSettings(L"about");
        return true;
    }
    if (n.action.rfind(L"uri:", 0) == 0) {
        std::wstring target = n.action.substr(4);
        return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
    }
    if (n.action.rfind(L"app:", 0) == 0) {
        std::wstring aumid = n.action.substr(4), key = aumid;
        for (auto& c : key)
            c = wchar_t(towlower(c));
        // An app living in the tray (QQ, WeChat, Feishu): its own tray click
        // opens what is new, the chat of the unread message. Starting it again
        // would open a second instance's login window instead.
        if (const TrayIcon* icon = TrayIconOf(n)) {
            AllowSetForegroundWindow(ASFW_ANY);
            g_app.tray->Click(*icon, false);
            Log(L"notify: action through the tray icon of %s", n.app.c_str());
            return true;
        }
        for (const TaskItem& t : g_app.tasks->Snapshot())
            if (t.key == key && !t.windows.empty()) {
                g_app.tasks->Show(key);
                return true;
            }
        // A program path (WeChat): starting it again shows its running instance.
        bool exe = aumid.size() > 2 && aumid[1] == L':';
        std::wstring path = exe ? aumid : L"shell:AppsFolder\\" + aumid;
        return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
    }
    return true; // nothing to run: the click just dismisses it
}

// Applies settings to the bar and the task model.
void EnsureGuard();

void ApplySettings(const Settings& s)
{
    g_app.desktopDisplay = s.desktopDisplay;
    g_app.displaySettings = s;
    // Popups share the bar's material and corner radius (concentric inside).
    Theme::SetGlass(s.tint, s.transparency);
    float radius = float(std::clamp(s.cornerRadius, 8.0, std::max(8.0, s.height / 2)));
    g_app.start->SetRadius(radius);
    g_app.trayPanel->SetRadius(radius);
    g_app.control->SetRadius(radius);
    g_app.control->SetBarHeight(float(s.height));
    g_app.calendar->SetRadius(radius);
    g_app.calendar->SetBarHeight(float(s.height));
    g_app.start->SetBarHeight(float(s.height)); // its status capsule is shaped like the bar
    g_app.start->SetShowTray(s.startTray);
    g_app.start->SetShowStatus(s.startStatus);
    g_app.start->SetPreferredSize(s.startSize);
    g_app.notes->SetRadius(radius);
    g_app.notes->SetOptions(s.notifyTimeout, s.notifyHideBody, s.notifyPlacement);
    // Notification cards belong to the bar (its bell): without it, Windows shows its own banners.
    if (s.bar && s.notifyListener && !g_app.listener)
        g_app.listener = std::make_unique<NotificationListener>(g_app.host, WM_NOTICE);
    else if (!(s.bar && s.notifyListener) && g_app.listener)
        g_app.listener.reset();
    if (s.bar && s.notifyWeChat && !g_app.wechat)
        g_app.wechat = std::make_unique<WeChatSource>(g_app.host, WM_NOTICE);
    else if (!(s.bar && s.notifyWeChat) && g_app.wechat)
        g_app.wechat.reset();
    if (s.bar && s.notifyFeishu && !g_app.feishu)
        g_app.feishu = std::make_unique<FeishuSource>(g_app.host, WM_NOTICE);
    else if (!(s.bar && s.notifyFeishu) && g_app.feishu)
        g_app.feishu.reset();
    g_app.tasks->SetPins(s.pins.value_or(std::vector<PinnedApp>{}));
    g_app.tasks->SetShowRunning(s.showRunning);
    SyncDisplayBars(s);
    if (g_app.barOn != s.bar) {
        g_app.barOn = s.bar;
        if (!s.bar) {
            // The bar's popups go with it.
            g_app.start->Close(false);
            g_app.trayPanel->Close();
            g_app.control->Close();
            g_app.calendar->Close();
            if (g_app.notes->CenterVisible())
                g_app.notes->ToggleCenter();
        }
        // The bar's windows go, its render thread stays: it draws the widgets' and menus' glass.
        ForBars([&](Bar& bar) { bar.SetVisible(s.bar); });
        Log(L"modules: bar %s", s.bar ? L"on" : L"off");
    }
    // The desktop stays on its own display when the shared Dock moves.
    HMONITOR desktop = ResolveDisplay(ConnectedDisplays(), s.desktopDisplay);
    g_app.widgets->SetMonitor(desktop);
    // With Stage Manager on, the widgets keep to the other side of the screen.
    g_app.widgets->SetOnRight(s.stageManager ? !s.stageOnRight : s.widgetsOnRight);
    g_app.stage->SetMonitor(desktop);
    g_app.stage->SetOnRight(s.stageOnRight);
    g_app.stage->SetAutoHide(s.stageAutoHide);
    g_app.stage->SetMinimizeToStrip(s.stageMinimize);
    g_app.stage->SetEnabled(s.stageManager);
    g_app.control->SetStage(s.stageManager, s.stageOnRight);
    g_app.widgets->SetMaterial(s.widgetMaterial == L"dusk" ? WidgetMaterial::Dusk : s.widgetMaterial == L"color" ? WidgetMaterial::Color : WidgetMaterial::Glass);
    g_app.widgets->SetEnabled(s.widgets);
    g_app.widgetsOn = s.widgets;
    // The Windows key opens the start menu and the bar's apps, which belong to the bar.
    const bool keyStart = s.bar && s.winKeyStart, keyNumbers = s.bar && s.winNumbers;
    if (!(keyStart == g_app.winKeyStart && keyNumbers == g_app.winNumbers && bool(g_app.winKey) == (keyStart || keyNumbers))) {
        g_app.winKey.reset();
        if (keyStart || keyNumbers)
            g_app.winKey = std::make_unique<WinKey>(g_app.host, WM_WIN_KEY, keyStart, keyNumbers);
        g_app.winKeyStart = keyStart;
        g_app.winNumbers = keyNumbers;
    }
    if (g_app.chrome) {
        // Without the bar, Windows' own taskbar and banners are never hidden.
        g_app.chrome->SetTaskbarHidden(s.bar && s.hideTaskbar);
        g_app.chrome->SetBannersHidden(s.bar && s.hideBanners);
        if (s.bar && (s.hideTaskbar || s.hideBanners))
            EnsureGuard();
    }
}

// While Windows' taskbar or its banners are hidden, a second Atrium.exe (`--guard`, RunGuard)
// watches this one: however it ends (a crash past the exception filter, abort, being killed),
// what it changed in Windows is put back at once; while it does not respond, the taskbar shows.
void EnsureGuard()
{
    if (g_app.guard && WaitForSingleObject(g_app.guard, 0) == WAIT_TIMEOUT)
        return;
    if (g_app.guard) {
        CloseHandle(g_app.guard);
        g_app.guard = nullptr;
    }
    wchar_t exe[MAX_PATH * 2];
    if (!GetModuleFileNameW(nullptr, exe, DWORD(std::size(exe))))
        return;
    std::wstring command = L"\"" + std::wstring(exe) + L"\" --guard " + std::to_wstring(GetCurrentProcessId());
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        Log(L"guard: could not start (%lu)", GetLastError());
        return;
    }
    CloseHandle(pi.hThread);
    g_app.guard = pi.hProcess;
    Log(L"guard: started (%lu)", pi.dwProcessId);
}

int RunGuard(DWORD pid)
{
    HANDLE held = CreateMutexW(nullptr, TRUE, kGuardMutex);
    HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process)
        return 1;
    HWND host = nullptr;
    int silent = 0; // checks in a row without an answer
    bool shown = false;
    while (WaitForSingleObject(process, 2000) == WAIT_TIMEOUT) {
        if (!host || !IsWindow(host)) {
            host = nullptr;
            for (HWND w = nullptr; (w = FindWindowExW(nullptr, w, kHostClass, nullptr)) != nullptr;) {
                DWORD owner = 0;
                GetWindowThreadProcessId(w, &owner);
                if (owner == pid)
                    host = w;
            }
        }
        DWORD_PTR answer = 0;
        const bool alive = !host || SendMessageTimeoutW(host, WM_NULL, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 3000, &answer);
        silent = alive ? 0 : silent + 1;
        if (silent >= 4 && !shown) {
            // About 15 s without an answer: the taskbar back (Atrium hides it again if it recovers).
            Log(L"guard: Atrium (%lu) is not responding: Windows' taskbar shown", pid);
            SystemChrome::ShowTaskbarsNow();
            shown = true;
        } else if (alive && shown) {
            Log(L"guard: Atrium (%lu) responds again", pid);
            shown = false;
        }
    }
    DWORD code = 0;
    GetExitCodeProcess(process, &code);
    CloseHandle(process);
    // A clean exit put everything back (system.json is gone); anything else is put back here.
    if (SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        if (SystemChrome::UndoLeftover())
            Log(L"guard: Atrium (%lu) ended without putting Windows back (exit 0x%08lX): done here", pid, code);
        CoUninitialize();
    }
    if (held) {
        ReleaseMutex(held);
        CloseHandle(held);
    }
    return 0;
}

void ShowItemMenu(const std::wstring& key, POINT pt)
{
    std::vector<TaskItem> items = ActiveBar().Items();
    auto it = std::find_if(items.begin(), items.end(), [&](const TaskItem& t) { return t.key == key; });
    if (it == items.end())
        return;
    std::vector<MenuEntry> entries = {{it->name, 0, false, false, true}, {L"", 0}};
    entries.push_back({it->windows.empty() || !it->multipleWindows ? L"打开" : L"打开新窗口", kCmdNewWindow});
    std::wstring location = g_app.tasks->FileLocation(key);
    if (!location.empty())
        entries.push_back({L"打开文件所在的位置", kCmdOpenLocation});
    entries.push_back({it->pinned ? L"从 Atrium 取消固定" : L"固定到 Atrium", int(it->pinned ? kCmdUnpin : kCmdPin)});
    if (!it->windows.empty())
        entries.push_back({it->windows.size() > 1 ? L"关闭所有窗口" : L"关闭窗口", kCmdClose});
    entries.push_back({L"关闭程序", kCmdTerminate, false, !g_app.tasks->CanTerminate(key)});
    int pinIndex = it->pinIndex;
    auto windows = it->windows;
    std::wstring name = it->name;
    g_app.menu->Open(std::move(entries), pt, MenuPlace::AboveCentered, [key, location, pinIndex, windows, name, pt](int cmd) {
        switch (cmd) {
        case kCmdNewWindow:
            g_app.tasks->Launch(key);
            break;
        case kCmdPin: {
            PinnedApp pin = g_app.tasks->PinFor(key);
            g_app.server->Modify([&](Settings& s) {
                auto pins = s.pins.value_or(std::vector<PinnedApp>{});
                pins.push_back(pin);
                s.pins = std::move(pins);
            });
            break;
        }
        case kCmdUnpin:
            g_app.server->Modify([&](Settings& s) {
                auto pins = s.pins.value_or(std::vector<PinnedApp>{});
                if (pinIndex >= 0 && pinIndex < int(pins.size()))
                    pins.erase(pins.begin() + pinIndex);
                s.pins = std::move(pins);
            });
            break;
        case kCmdClose:
            for (HWND window : windows)
                TaskModel::CloseWindow(window);
            break;
        case kCmdTerminate: {
            // Ending the processes loses what is not saved: asked once more, in place.
            std::vector<MenuEntry> confirm = {{L"强制关闭“" + name + L"”？未保存的内容会丢失", 0, false, false, true},
                                              {L"", 0},
                                              {L"强制关闭", kCmdTerminateConfirm},
                                              {L"取消", 0}};
            g_app.menu->Open(std::move(confirm), pt, MenuPlace::AboveCentered, [key](int answer) {
                if (answer == kCmdTerminateConfirm)
                    g_app.tasks->Terminate(key);
            });
            break;
        }
        case kCmdOpenLocation:
            if (PIDLIST_ABSOLUTE id = ILCreateFromPathW(location.c_str())) {
                SHOpenFolderAndSelectItems(id, 0, nullptr, 0);
                ILFree(id);
            }
            break;
        }
    });
}

// The overflow entry: the apps that did not fit, as a menu above the entry.
void ShowOverflow(const std::vector<std::wstring>& keys, RECT anchor)
{
    std::vector<TaskItem> items = ActiveBar().Items();
    std::vector<MenuEntry> entries;
    const int px = int(std::lround(16 * PopupScale(anchor)));
    for (size_t i = 0; i < keys.size(); ++i) {
        auto it = std::find_if(items.begin(), items.end(), [&](const TaskItem& t) { return t.key == keys[i]; });
        std::wstring name = it != items.end() ? it->name : keys[i];
        if (it != items.end() && !it->windows.empty())
            name += it->windows.size() > 1 ? L"  \u00B7 " + std::to_wstring(it->windows.size()) : L"  \u00B7";
        entries.push_back({name, int(1000 + i), false, false, false, g_app.tasks->Icon(keys[i], px)});
    }
    g_app.menu->Open(std::move(entries), POINT{(anchor.left + anchor.right) / 2, anchor.top}, MenuPlace::AboveCentered, [keys](int cmd) {
        if (cmd >= 1000 && cmd < 1000 + int(keys.size()))
            g_app.tasks->Activate(keys[size_t(cmd - 1000)], ActiveBar().TaskMonitor());
    });
}

// An app dropped at visible position `to` (in the list without it). Pinned
// apps reorder the pins; a running app dropped among the pins gets pinned
// there; a pinned app dropped among the running ones goes to the end of the
// pins (dragging never unpins, as on the Windows taskbar).
void Reorder(const std::wstring& key, int to)
{
    std::vector<TaskItem> items = ActiveBar().Items();
    auto it = std::find_if(items.begin(), items.end(), [&](const TaskItem& t) { return t.key == key; });
    if (it == items.end())
        return;
    TaskItem dragged = *it;
    items.erase(it);
    to = std::clamp(to, 0, int(items.size()));
    int pinnedLeft = int(std::count_if(items.begin(), items.end(), [](const TaskItem& t) { return t.pinned; }));
    if (!dragged.pinned && to >= pinnedLeft) {
        // Translate the visible insertion point into the shared running-app order.
        const auto all = g_app.tasks->Snapshot();
        const std::wstring before = to < int(items.size()) ? items[size_t(to)].key : L"";
        int running = 0;
        for (const TaskItem& item : all) {
            if (item.key == before)
                break;
            if (!item.pinned && item.key != key)
                ++running;
        }
        g_app.tasks->MoveRunning(key, running);
        return;
    }
    // Pin index (in the current list) the item goes in front of.
    int at = to < pinnedLeft ? items[size_t(to)].pinIndex : pinnedLeft ? items[size_t(pinnedLeft - 1)].pinIndex + 1 : 0;
    PinnedApp fresh = dragged.pinned ? PinnedApp{} : g_app.tasks->PinFor(key);
    g_app.server->Modify([&](Settings& s) {
        auto pins = s.pins.value_or(std::vector<PinnedApp>{});
        PinnedApp pin = fresh;
        int dest = at;
        if (dragged.pinned) {
            if (dragged.pinIndex < 0 || dragged.pinIndex >= int(pins.size()))
                return;
            pin = pins[size_t(dragged.pinIndex)];
            pins.erase(pins.begin() + dragged.pinIndex);
            if (dest > dragged.pinIndex)
                --dest;
        }
        pins.insert(pins.begin() + std::clamp(dest, 0, int(pins.size())), pin);
        s.pins = std::move(pins);
    });
    Log(L"bar: %s moved to %d%s", key.c_str(), to, dragged.pinned ? L"" : L" and pinned");
}

void CloseBarPopups()
{
    g_app.start->Close(false);
    g_app.trayPanel->Close();
    g_app.control->Close();
    g_app.calendar->Close();
    g_app.menu->Close();
    g_app.preview->Close();
    if (g_app.notes->CenterVisible())
        g_app.notes->ToggleCenter();
}

void ConfigureBar(Bar& bar)
{
    bar.onInteract = [&bar] {
        if (g_app.activeBar == &bar)
            return;
        CloseBarPopups();
        g_app.activeBar = &bar;
        g_app.switchedBarAt = GetTickCount64();
        g_app.notes->PanelsChanged();
    };
    bar.iconProvider = [](const std::wstring& key, int px) { return g_app.tasks->Icon(key, px); };
    bar.onActivate = [&bar](const std::wstring& key) { g_app.tasks->Activate(key, bar.TaskMonitor()); };
    bar.onLaunch = [](const std::wstring& key) { g_app.tasks->Launch(key); };
    bar.onItemMenu = ShowItemMenu;
    bar.onClockClick = ToggleCalendar;
    bar.onReorder = Reorder;
    bar.onOverflow = ShowOverflow;
    bar.onStartClick = ToggleStart;
    bar.onNotifyClick = [](RECT) { g_app.notes->ToggleCenter(); };
    bar.onTrayClick = ToggleTray;
    bar.onStatusClick = ToggleControl;
    bar.keepShown = [&bar] {
        return g_app.activeBar == &bar && (g_app.start->Visible() || g_app.trayPanel->Visible() || g_app.control->Visible() ||
            g_app.calendar->Visible() || g_app.notes->CenterVisible() || g_app.menu->Visible());
    };
    bar.onStatusWheel = [](int notches) { AdjustVolume(2 * notches); UpdateStatus(); };
    bar.onFileDragHover = [](const std::wstring& key) { g_app.tasks->Show(key); };
    bar.canOpenFiles = [](const std::wstring& key) { return g_app.tasks->CanOpenFiles(key); };
    bar.onDropFiles = [](const std::wstring& key, const std::vector<std::wstring>& files) { g_app.tasks->OpenFiles(key, files); };
    bar.onContextMenu = ShowMenu;
    bar.onPreview = [](const std::wstring& key, const std::vector<HWND>& windows, POINT anchor) {
        g_app.preview->Show(key, windows, anchor, g_app.tasks->Icon(key, 64));
    };
    bar.onPreviewLeave = [&bar] { if (g_app.activeBar == &bar) g_app.preview->Leave(); };
    bar.onPreviewClose = [&bar] { if (g_app.activeBar == &bar) g_app.preview->Close(); };
    bar.previewOpen = [&bar] { return g_app.activeBar == &bar && g_app.preview->Visible(); };
    bar.onMonitorChanged = [&bar] {
        if (g_app.activeBar == &bar) {
            CloseBarPopups();
            g_app.notes->PanelsChanged();
        }
    };
    bar.onItemsChanged = [&bar] {
        if (g_app.activeBar == &bar)
            g_app.preview->Refresh(bar.Items());
    };
    bar.onCloseRequest = [] { PostMessageW(g_app.host, WM_CLOSE, 0, 0); };
}

void SyncDisplayBars(const Settings& settings, bool refresh)
{
    const auto displays = ConnectedDisplays();
    // Keep existing renderers when changing appearance; only retire disconnected or unused bars.
    std::erase_if(g_app.secondaryBars, [&](const auto& bar) {
        const bool connected = settings.bar && settings.dockDisplayMode == L"all" && std::any_of(displays.begin(), displays.end(),
            [&](const DisplayInfo& display) { return !display.primary && display.id == bar->DisplayOverride(); });
        if (connected)
            return false;
        if (g_app.activeBar == bar.get()) {
            CloseBarPopups();
            g_app.activeBar = g_app.bar.get();
        }
        bar->SetVisible(false);
        // Same bounded shutdown as the primary renderer: restore the Windows shell if a driver hangs.
        if (!bar->StopRendering(3000)) {
            g_app.chrome->RestoreForExit();
            Log(L"core: secondary display renderer stuck during removal");
            TerminateProcess(GetCurrentProcess(), 0);
        }
        return true;
    });
    if (settings.bar && settings.dockDisplayMode == L"all")
        for (const DisplayInfo& display : displays) {
            if (display.primary || std::any_of(g_app.secondaryBars.begin(), g_app.secondaryBars.end(),
                    [&](const auto& bar) { return bar->DisplayOverride() == display.id; }))
                continue;
            auto bar = std::make_unique<Bar>(g_app.instance, g_app.capturable, display.id);
            ConfigureBar(*bar);
            bar->Apply(settings);
            bar->SetStatus(g_app.lastStatus);
            bar->SetUnread(g_app.notes->Unread());
            g_app.secondaryBars.push_back(std::move(bar));
        }
    const auto items = g_app.tasks->Snapshot();
    ForBars([&](Bar& bar) {
        if (refresh)
            bar.RefreshDisplays();
        bar.Apply(settings);
        bar.SetItems(items);
        bar.SetVisible(settings.bar);
    });
    g_app.notes->PanelsChanged();
}

LRESULT CALLBACK HostProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (g_app.tasks && g_app.tasks->HandleMessage(m, w, l))
        return 0;
    if (m == g_app.taskbarCreated && m != 0) {
        g_app.trayAdded = false;
        AddTrayIcon(); // Explorer restarted
        if (g_app.tray)
            g_app.tray->Refresh();
        if (g_app.chrome)
            g_app.chrome->Enforce(); // its new taskbar
        return 0;
    }
    switch (m) {
    case WM_UPDATE_READY:
        if (g_app.updates && g_app.notes) g_app.updates->NotifyPrepared(*g_app.notes);
        return 0;
    case WM_TRAY:
        switch (LOWORD(l)) {
        case WM_CONTEXTMENU:
        case WM_RBUTTONUP: {
            // Explorer gives the icon's place; with the system taskbar hidden (and from Atrium's
            // tray panel) that is a hidden button at the screen's edge: the pointer then.
            POINT at{GET_X_LPARAM(w), GET_Y_LPARAM(w)}, cursor;
            if (GetCursorPos(&cursor) && (std::abs(at.x - cursor.x) > 48 || std::abs(at.y - cursor.y) > 48))
                at = cursor;
            ShowMenu(at);
            return 0;
        }
        case NIN_SELECT:
        case NIN_KEYSELECT:
        case WM_LBUTTONDBLCLK:
            OpenSettings();
            return 0;
        }
        return 0;
    case WM_OPEN_SETTINGS:
        OpenSettings();
        return 0;
    case WM_WIN_KEY:
        if (!g_app.barOn.value_or(true))
            return 0;
        if (w) {
            // Win + digit: the bar's app at that place, as a click on it. The keys never reached a
            // window of ours (the foreground lock holds): share the foreground thread's input state.
            HWND fg = GetForegroundWindow();
            DWORD fgThread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0, me = GetCurrentThreadId();
            bool attached = fgThread && fgThread != me && AttachThreadInput(me, fgThread, TRUE);
            AllowSetForegroundWindow(ASFW_ANY);
            g_app.bar->ActivateIndex(int(w) - 1);
            if (attached)
                AttachThreadInput(me, fgThread, FALSE);
            return 0;
        }
        if (g_app.start->Visible()) {
            g_app.start->Close(false);
        } else {
            // The keyboard's: whatever was in front gets the focus back when it closes. The key
            // never reached a window of ours, so the foreground lock holds: share the foreground
            // thread's input state for the moment the menu takes the focus (as the tray does).
            HWND fg = GetForegroundWindow();
            DWORD fgThread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0, me = GetCurrentThreadId();
            bool attached = fgThread && fgThread != me && AttachThreadInput(me, fgThread, TRUE);
            g_app.start->Open(g_app.bar->BarRect(), g_app.bar->EntryRect(L"#start"), fg);
            if (attached)
                AttachThreadInput(me, fgThread, FALSE);
            g_app.notes->PanelsChanged();
            if (GetForegroundWindow() != g_app.start->Hwnd())
                Log(L"winkey: the start menu did not get the focus");
        }
        return 0;
    case WM_NOTICE:
        if (auto* n = reinterpret_cast<Notice*>(l)) {
            // Only from sources still turned on (a notice may arrive just after).
            bool on = n->source == L"atrium" ? g_app.barOn.value_or(true) : n->source == L"wechat" ? bool(g_app.wechat)
                       : n->source == L"feishu" ? bool(g_app.feishu) : bool(g_app.listener);
            if (g_app.notes && on)
                g_app.notes->Post(*n);
            delete n;
        }
        return 0;
    case WM_CATALOG:
        if (g_app.start)
            g_app.start->Refresh();
        return 0;
    case WM_VOLUME:
        UpdateStatus();
        return 0;
    case WM_STATUS: {
        std::unique_ptr<SystemStatus> status(reinterpret_cast<SystemStatus*>(l));
        status->brightness = g_app.brightness;
        g_app.lastStatus = *status;
        ForBars([&](Bar& bar) { bar.SetStatus(*status); });
        g_app.control->SetStatus(*status);
        g_app.start->SetStatus(*status);
        g_app.widgets->SetStatus(*status);
        return 0;
    }
    case WM_POWERBROADCAST:
        if (w == PBT_POWERSETTINGCHANGE) {
            const auto* s = reinterpret_cast<const POWERBROADCAST_SETTING*>(l);
            if (s && s->PowerSetting == kBrightnessSetting && s->DataLength == sizeof(DWORD)) {
                g_app.brightness = int(std::min<DWORD>(*reinterpret_cast<const DWORD*>(s->Data), 100));
                g_app.control->BrightnessChanged(g_app.brightness);
                UpdateStatus();
            }
        }
        return TRUE;
    case WM_TRAY_ICONS:
        if (w == 1) // a click was forwarded: see where the focus ends up
            SetTimer(h, kTrayFocusTimer, 600, nullptr);
        else {
            if (g_app.tray)
                g_app.tray->LiveRefresh(); // the broker has newer icons than the snapshot
            if (g_app.trayPanel)
                g_app.trayPanel->Refresh();
        }
        return 0;
    case WM_TIMER:
        if (w == kReadyTimer) {
            if (g_app.server && g_app.server->Ready()) {
                if (HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, g_readyEvent.c_str())) {
                    SetEvent(event);
                    CloseHandle(event);
                }
                KillTimer(h, kReadyTimer);
            }
            return 0;
        }
        if (w == kTrayTimer) {
            OpenTrayStep2();
            return 0;
        }
        if (w == kStatusTimer) {
            if (!g_app.trayAdded)
                AddTrayIcon();
            if (g_app.guard && WaitForSingleObject(g_app.guard, 0) == WAIT_OBJECT_0) {
                Log(L"guard: it went away; started again");
                EnsureGuard();
            }
            UpdateStatus();
            if (g_app.chrome)
                g_app.chrome->Enforce();
            return 0;
        }
        if (w == kTrayFocusTimer) {
            // Explorer gives the focus to its hidden overflow window when a
            // tray button is invoked. The app usually takes it for its window
            // or menu; if not, it goes back to where it was before the panel.
            // Left there, the hidden window holds the system taskbar up and,
            // as a light-dismiss popup, swallows the next click elsewhere.
            KillTimer(h, kTrayFocusTimer);
            HWND fg = GetForegroundWindow();
            wchar_t cls[64] = {};
            GetClassNameW(fg, cls, 64);
            if (wcscmp(cls, L"TopLevelWindowForOverflowXamlIsland") == 0 && !IsWindowVisible(fg)) {
                HWND back = g_app.trayPanel->Previous();
                if (!back || !IsWindow(back))
                    back = FindWindowW(L"Progman", nullptr);
                // The focus belongs to Explorer's thread: share its input state
                // for the moment of the switch (the usual way to hand it on).
                DWORD fgThread = GetWindowThreadProcessId(fg, nullptr), me = GetCurrentThreadId();
                bool attached = fgThread && fgThread != me && AttachThreadInput(me, fgThread, TRUE);
                bool ok = SetForegroundWindow(back) != FALSE;
                DWORD error = ok ? 0 : GetLastError();
                if (attached)
                    AttachThreadInput(me, fgThread, FALSE);
                const wchar_t* how = L"attached";
                if (!ok) {
                    // A key press of our own lifts the foreground lock (Alt alone
                    // does nothing in the hidden window that has the keyboard).
                    INPUT alt[2] = {};
                    alt[0].type = alt[1].type = INPUT_KEYBOARD;
                    alt[0].ki.wVk = alt[1].ki.wVk = VK_MENU;
                    alt[1].ki.dwFlags = KEYEVENTF_KEYUP;
                    SendInput(2, alt, sizeof(INPUT));
                    ok = SetForegroundWindow(back) != FALSE;
                    how = L"after a key press";
                }
                Log(L"tray: the hidden overflow window had the focus; given back %s (%s, error %lu)", how, ok && GetForegroundWindow() == back ? L"ok" : L"failed",
                    error);
            }
            return 0;
        }
        break;
    case WM_ATRIUM_ADD_WIDGET:
        return w <= WPARAM(WidgetKind::Bluetooth) && l >= 0 && l <= LPARAM(WidgetSize::Large) &&
               g_app.widgetsOn && g_app.widgets->Add(WidgetKind(w), WidgetSize(l)) ? 1 : 0;
    case WM_ATRIUM_SETTINGS:
        ApplySettings(g_app.server->Current());
        return 0;
    case WM_DISPLAYCHANGE:
    case WM_DPICHANGED:
        if (!g_app.displaysReady)
            return 0;
        if (g_app.chrome)
            g_app.chrome->Enforce(); // a new display's taskbar
        SyncDisplayBars(g_app.displaySettings, true);
        {
            HMONITOR desktop = ResolveDisplay(ConnectedDisplays(), g_app.desktopDisplay);
            g_app.widgets->SetMonitor(desktop, true);
            g_app.stage->SetMonitor(desktop, true);
            g_app.notes->PanelsChanged();
        }
        return 0;
    case WM_SETTINGCHANGE:
        if (w == SPI_SETWORKAREA && g_app.displaysReady) {
            SyncDisplayBars(g_app.displaySettings, true);
            HMONITOR desktop = ResolveDisplay(ConnectedDisplays(), g_app.desktopDisplay);
            g_app.widgets->SetMonitor(desktop, true);
            g_app.stage->SetMonitor(desktop, true);
        }
        return 0;
    case WM_CLOSE:
        if (g_app.chrome) g_app.chrome->RestoreForExit();
        DestroyWindow(h);
        return 0;
    case WM_ENDSESSION:
        if (w) {
            Log(L"core: session ending");
            if (g_app.chrome) g_app.chrome->RestoreForExit(); // no later message may hide Windows again
        }
        return 0;
    case WM_DESTROY:
        if (g_app.brightnessNotify)
            UnregisterPowerSettingNotification(g_app.brightnessNotify);
        RemoveTrayIcon();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR cmdLine, int)
{
    // Never in the install folder (started from it in Explorer, or by the updater): the apps started
    // from the bar would inherit it, and a folder some process is in cannot be replaced by an update.
    if (PWSTR profile = nullptr; SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &profile))) {
        SetCurrentDirectoryW(profile);
        CoTaskMemFree(profile);
    }
    int argumentCount = 0;
    auto startupArguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    for (int i = 1; startupArguments && i + 1 < argumentCount; ++i)
        if (wcscmp(startupArguments[i], L"--ready-event") == 0 && std::wstring_view(startupArguments[i + 1]).starts_with(L"Local\\Atrium.Update.Core."))
            g_readyEvent = startupArguments[i + 1];
    LocalFree(startupArguments);
    if (wcscmp(cmdLine, L"--uninstall") == 0) {
        if (MessageBoxW(nullptr, L"卸载 Atrium？个人设置和桌面小组件布局将会保留。", L"Atrium",
                        MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2 | MB_SETFOREGROUND) != IDYES)
            return 0;
        wchar_t system[MAX_PATH]{};
        GetSystemDirectoryW(system, MAX_PATH);
        std::wstring powershell = std::wstring(system) + L"\\WindowsPowerShell\\v1.0\\powershell.exe";
        std::wstring arguments = L"-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File \"" + ExeDir() + L"\\Uninstall.ps1\" -Confirmed";
        // The launcher exits before the script removes this executable.
        return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", powershell.c_str(), arguments.c_str(), nullptr, SW_HIDE)) > 32 ? 0 : 1;
    }
    // `Atrium.exe --guard <pid>`: the guard of a run hiding Windows' taskbar (EnsureGuard).
    if (const wchar_t* guard = wcsstr(cmdLine, L"--guard ")) {
        const DWORD pid = wcstoul(guard + 8, nullptr, 10);
        return pid ? RunGuard(pid) : 1;
    }
    // `Atrium.exe --exit` asks a running instance to quit (installer, updater).
    if (wcsstr(cmdLine, L"--exit")) {
        HWND other = FindWindowW(kHostClass, nullptr);
        if (!other)
            return 0;
        if (!SameCoreExecutable(other)) return 1;
        DWORD pid = 0;
        GetWindowThreadProcessId(other, &pid);
        HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
        PostMessageW(other, WM_CLOSE, 0, 0);
        DWORD wait = process ? WaitForSingleObject(process, 10000) : WAIT_FAILED;
        if (process)
            CloseHandle(process);
        // And its guard, which runs from the same file (the updater moves the folder next).
        if (HANDLE guard = OpenMutexW(SYNCHRONIZE, FALSE, kGuardMutex)) {
            DWORD done = WaitForSingleObject(guard, 5000);
            if (done == WAIT_OBJECT_0 || done == WAIT_ABANDONED)
                ReleaseMutex(guard);
            CloseHandle(guard);
        }
        return wait == WAIT_OBJECT_0 ? 0 : 1;
    }
    HANDLE mutex = CreateMutexW(nullptr, TRUE, kInstanceMutex);
    if (!mutex) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // Already running: launching it again opens the settings instead.
        if (HWND other = FindWindowW(kHostClass, nullptr)) {
            if (SameCoreExecutable(other)) PostMessageW(other, WM_OPEN_SETTINGS, 0, 0);
            else MessageBoxW(nullptr, L"另一位置的 Atrium 正在运行。请先退出该实例，再启动本目录中的版本。", L"Atrium", MB_OK | MB_ICONINFORMATION);
        }
        CloseHandle(mutex);
        return 0;
    }
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    SetCurrentProcessExplicitAppUserModelID(kAppId);
    g_app.instance = instance;
    Log(L"core: starting");
    const bool registeredHere = IsRegisteredApplication(ExeDir());
    RegisterApplication(ExeDir());
    // A crash must not leave the user without a taskbar.
    SetUnhandledExceptionFilter([](EXCEPTION_POINTERS*) -> LONG {
        SystemChrome::EmergencyRestore();
        return EXCEPTION_CONTINUE_SEARCH;
    });
    g_app.chrome = std::make_unique<SystemChrome>();

    WNDCLASSEXW wc{sizeof(wc)};
    wc.hInstance = instance;
    wc.lpfnWndProc = HostProc;
    wc.lpszClassName = kHostClass;
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_ATRIUM));
    RegisterClassExW(&wc);
    // A hidden top-level window (not message-only) so TaskbarCreated arrives.
    g_app.host = CreateWindowExW(WS_EX_TOOLWINDOW, kHostClass, L"Atrium", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
    g_app.taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    ChangeWindowMessageFilterEx(g_app.host, g_app.taskbarCreated, MSGFLT_ALLOW, nullptr);

    Settings settings = LoadSettings();
    // Also on every start while it is on: an update from a version that started from the
    // Run key moves to the logon task, unless that entry was turned off in Task Manager.
    // Off this thread: Task Scheduler is slow while the session is starting.
    if (IsRegisteredApplication(ExeDir()) && (!registeredHere || (settings.startOnLogin && !StartupTurnedOff())))
        std::thread([on = settings.startOnLogin] {
            if (!SetStartOnLogin(on))
                Log(L"startup: could not update the Windows logon entry");
        }).detach();
    if (!settings.pins) {
        // First run: start from the user's Windows taskbar pins (saved on the next save).
        settings.pins = TaskModel::ImportWindowsPins();
        Log(L"core: imported %zu Windows taskbar pins", settings.pins->size());
    }
    const bool capturable = wcsstr(cmdLine, L"--debug-capturable") != nullptr;
    g_app.capturable = capturable;
    g_app.bar = std::make_unique<Bar>(instance, capturable);
    g_app.activeBar = g_app.bar.get();
    // The bar's render thread draws the popups' glass from its capture.
    Surface::SetGlassHost(g_app.bar.get());
    Surface::SetCapturable(capturable);
    g_app.menu = std::make_unique<GlassMenu>(instance);
    g_app.tasks = std::make_unique<TaskModel>(g_app.host, [] {
        const double t0 = ClockMs();
        if (g_app.stage)
            g_app.stage->Update(g_app.tasks->Snapshot(true));
        const double t1 = ClockMs();
        auto items = g_app.tasks->Snapshot();
        const double t2 = ClockMs();
        ForBars([&](Bar& bar) { bar.SetItems(items); });
        if (g_app.preview)
            g_app.preview->Refresh(ActiveBar().Items());
        const double t3 = ClockMs();
        if (g_app.start)
            g_app.start->Refresh();
        const double t4 = ClockMs();
        if (t4 - t0 > 20)
            Log(L"tasks: changes took %.0f ms (stage %.0f, preview %.0f, bar %.0f, start %.0f)", t4 - t0, t1 - t0, t2 - t1, t3 - t2, t4 - t3);
    }, [](const std::wstring& exe) {
        return g_app.tray->OpenExisting(exe);
    });
    g_app.catalog = std::make_unique<AppCatalog>(g_app.host, WM_CATALOG);
    g_app.start = std::make_unique<StartMenu>(instance, *g_app.tasks, *g_app.catalog);
    {
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
        g_app.catalog->PrefetchStartIcons(StartMenu::TileIconPx(PopupScale(mi.rcWork)));
    }
    g_app.start->onSystem = OpenSystem;
    g_app.start->menu = g_app.menu.get();
    g_app.notes = std::make_unique<Notifications>(instance);
    g_app.tray = std::make_unique<TrayModel>(g_app.host, WM_TRAY_ICONS, [] {
        std::vector<PinnedApp> names;
        for (const auto& app : g_app.catalog->Apps()) names.push_back(AppCatalog::PinFor(app));
        return names;
    });
    g_app.trayPanel = std::make_unique<TrayPanel>(instance, *g_app.tray);
    g_app.trayPanel->onSystemTray = OpenTray;
    g_app.control = std::make_unique<ControlCenter>(instance);
    g_app.control->onChanged = UpdateStatus;
    g_app.start->onStatusRequest = UpdateStatus;
    g_app.calendar = std::make_unique<CalendarPanel>(instance);
    g_app.widgets = std::make_unique<DesktopWidgets>(instance);
    g_app.stage = std::make_unique<StageManager>(instance, *g_app.tasks);
    g_app.control->onStage = SetStageManager;
    g_app.widgets->onStatusRequest = UpdateStatus;
    g_app.status = std::make_unique<SystemStatusReader>(g_app.host, WM_STATUS, WM_VOLUME);
    g_app.widgets->menu = g_app.menu.get();
    g_app.widgets->onAddWidgets = [] { OpenSettings(L"widgets"); };
    g_app.widgets->onCalendar = [] {
        if (!g_app.calendar->Visible())
            ToggleCalendar();
    };
    g_app.notes->bellRect = [] { return ActiveBar().EntryRect(L"#notify"); };
    g_app.notes->barRect = [] { return ActiveBar().BarRect(); };
    g_app.notes->onUnread = [](int n) { ForBars([&](Bar& bar) { bar.SetUnread(n); }); };
    g_app.notes->onActivate = RunNoticeAction;
    g_app.notes->quietIsOurs = [] { return g_app.chrome && g_app.chrome->QuietIsOurs(); };
    // New cards wait while one of Atrium's panels is open (they would overlap it).
    g_app.notes->held = [] {
        return g_app.start->Visible() || g_app.trayPanel->Visible() || g_app.control->Visible() || g_app.calendar->Visible();
    };
    // Pointer on an app with windows: its windows, previewed above it (as the Windows taskbar).
    g_app.preview = std::make_unique<WindowPreview>(instance);
    ConfigureBar(*g_app.bar);
    g_app.displaysReady = true;
    ApplySettings(settings);
    g_app.bar->SetItems(g_app.tasks->Snapshot());
    g_app.server = std::make_unique<SettingsServer>(g_app.host, settings);
    // The start menu's size is a setting (the settings show it); kept in start.json before.
    g_app.start->onSize = [](const std::wstring& name) { g_app.server->Modify([&](Settings& s) { s.startSize = name; }); };
    if (settings.startSize.empty())
        g_app.start->onSize(g_app.start->SizeName());
    // The start menu shows the Windows start menu's pins; from there an app
    // can be pinned to the bar (the bar's own pins are separate).
    g_app.start->onPin = [](const PinnedApp& pin) {
        g_app.server->Modify([&](Settings& s) {
            auto pins = s.pins.value_or(std::vector<PinnedApp>{});
            if (std::find(pins.begin(), pins.end(), pin) == pins.end() && pins.size() < 64)
                pins.push_back(pin);
            s.pins = std::move(pins);
        });
    };
    // The tray broker takes the notification-icon messages on their way to Explorer and
    // forwards every one of them, so the system tray behaves as before while Atrium sees
    // the live icons (and clicks with the program's own callback message).
    Broker().Start(g_app.host, WM_TRAY_ICONS);
    AddTrayIcon();
    UpdateStatus();
    g_app.brightnessNotify = RegisterPowerSettingNotification(g_app.host, &kBrightnessSetting, DEVICE_NOTIFY_WINDOW_HANDLE);
    SetTimer(g_app.host, kStatusTimer, kStatusMs, nullptr);
    g_app.tray->Refresh(); // the panel opens with the icons already read
    g_app.updates = std::make_unique<UpdateChecker>(g_app.host);
    if (!g_readyEvent.empty()) SetTimer(g_app.host, kReadyTimer, 50, nullptr);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    g_app.displaysReady = false;
    Broker().Stop(); // its window belongs to this thread; Explorer gets the icons back
    // First the render thread, while every window it draws for is still there; a driver call stuck
    // in the desktop duplication (it holds the shared device) must not keep Atrium from exiting:
    // the system taskbar comes back and the process ends.
    if (g_app.chrome) g_app.chrome->RestoreForExit();
    g_app.stage->RestoreAll(); // the windows Stage Manager put away come back
    g_app.winKey.reset();
    g_app.status.reset();
    g_app.preview->Close();
    bool renderingStopped = true;
    ForBars([&](Bar& bar) { renderingStopped &= bar.StopRendering(3000); });
    if (!renderingStopped) {
        g_app.chrome.reset();
        Log(L"core: exited (render thread stuck)");
        ReleaseMutex(mutex);
        Sleep(150); // the log line reaches the file
        // Not ExitProcess: unloading the graphics DLLs would wait for the same stuck call.
        TerminateProcess(GetCurrentProcess(), 0);
    }
    g_app.server.reset();
    g_app.updates.reset();
    g_app.listener.reset();
    g_app.wechat.reset();
    g_app.feishu.reset();
    g_app.notes.reset();
    g_app.widgets.reset();
    g_app.calendar.reset();
    g_app.control.reset();
    g_app.trayPanel.reset();
    g_app.tray.reset();
    g_app.start.reset();
    g_app.menu.reset();
    g_app.preview.reset();
    g_app.stage.reset();
    Surface::SetGlassHost(nullptr);
    g_app.catalog.reset();
    g_app.tasks.reset();
    g_app.secondaryBars.clear();
    g_app.bar.reset();
    g_app.chrome.reset();
    Log(L"core: exited");
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return 0;
}
