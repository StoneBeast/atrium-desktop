#include "settings.h"
#include "shell/displays.h"
#include "ui/notification_layout.h"
#include "motion.h"

#include <winrt/Windows.Foundation.h>
#include <iostream>
#include <stdexcept>

using namespace atrium;

namespace {
void Require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void CheckSettings()
{
    Settings settings, parsed;
    settings.dockDisplayMode = L"all";
    settings.dockDisplayDelay = 750;
    settings.dockDisplay = L"\\\\?\\DISPLAY#external";
    settings.desktopDisplay = L"disconnected-screen";
    settings.taskbarApps = L"primaryAndWindow";
    settings.taskbarRightPrimaryOnly = true;
    settings.notifyPlacement = L"topRight";
    settings.hoverRise = false;
    Require(Settings::FromJson(settings.ToJson(), Settings{}, parsed), "settings round trip parses");
    Require(!parsed.hoverRise, "disabled hover rise survives saving");
    Require(Settings::FromJson(LR"({})", Settings{}, parsed) && parsed.hoverRise, "old settings keep hover rise enabled");
    Require(Settings::FromJson(LR"({})", settings, parsed) && !parsed.hoverRise, "partial updates retain disabled hover rise");
    Require(Settings::FromJson(LR"({"hoverRise":"bad"})", settings, parsed) && !parsed.hoverRise,
        "malformed hover rise retains the current value");
    Require(Settings::FromJson(LR"({"hoverRise":true})", settings, parsed) && parsed.hoverRise && parsed.magnify == settings.magnify,
        "hover rise toggles without changing icon magnification");
    Require(Settings::FromJson(settings.ToJson(), Settings{}, parsed), "restore settings round trip");
    Require(parsed.dockDisplayMode == L"all" && parsed.dockDisplayDelay == 750 && parsed.dockDisplay == settings.dockDisplay &&
            parsed.desktopDisplay == settings.desktopDisplay && parsed.taskbarApps == settings.taskbarApps && parsed.taskbarRightPrimaryOnly &&
            parsed.notifyPlacement == L"topRight", "all display and notification preferences survive saving");
    Require(Settings::FromJson(LR"({"taskbarRightPrimaryOnly":false})", settings, parsed) && !parsed.taskbarRightPrimaryOnly &&
            parsed.showTray && parsed.showStatus && parsed.showClock && parsed.showNotifications, "disabling the primary-only group retains individual content switches");
    Require(Settings::FromJson(LR"({"taskbarRightPrimaryOnly":"bad"})", settings, parsed) && parsed.taskbarRightPrimaryOnly,
        "malformed primary-only visibility retains the current value");
    Require(Settings::FromJson(LR"({})", settings, parsed) && parsed.taskbarRightPrimaryOnly, "partial updates retain primary-only visibility");
    for (const wchar_t* mode : {L"follow", L"primary", L"fixed", L"all"}) {
        settings.dockDisplayMode = mode;
        Require(Settings::FromJson(settings.ToJson(), Settings{}, parsed) && parsed.dockDisplayMode == mode, "every Dock mode survives saving");
    }
    for (const wchar_t* mode : {L"all", L"primaryAndWindow", L"window"}) {
        settings.taskbarApps = mode;
        Require(Settings::FromJson(settings.ToJson(), Settings{}, parsed) && parsed.taskbarApps == mode, "every task distribution mode survives saving");
    }
    Require(Settings::FromJson(LR"({"height":64})", Settings{}, parsed) && parsed.dockDisplayMode == L"follow" &&
            parsed.taskbarApps == L"all" && parsed.dockDisplayDelay == 450 && !parsed.taskbarRightPrimaryOnly &&
            parsed.notifyPlacement == L"dock", "old settings retain right-side elements and default dock notifications");
    Require(Settings::FromJson(LR"({"notifyPlacement":"bad"})", Settings{}, parsed) && parsed.notifyPlacement == L"dock", "invalid notification placement uses the default");
    Require(Settings::FromJson(LR"({"notifyPlacement":false})", settings, parsed) && parsed.notifyPlacement == L"topRight", "malformed notification placement retains the current value");
    Require(Settings::FromJson(LR"({"notifyPlacement":"dock"})", settings, parsed) && parsed.notifyPlacement == L"dock", "notification placement can return to the default");
    Require(Settings::FromJson(LR"({"dockDisplayMode":"bad","taskbarApps":"bad","dockDisplayDelay":99999})", Settings{}, parsed) &&
            parsed.dockDisplayMode == L"follow" && parsed.taskbarApps == L"all" && parsed.dockDisplayDelay == 2000, "invalid values are normalized");
    Require(Settings::FromJson(LR"({"dockDisplayDelay":-1})", Settings{}, parsed) && parsed.dockDisplayDelay == 100, "delay has a lower bound");
    Require(Settings::FromJson(LR"({"dockDisplayDelay":"bad","dockDisplay":false})", settings, parsed) &&
            parsed.dockDisplayDelay == 750 && parsed.dockDisplay == settings.dockDisplay, "malformed keys retain current values");
}

void CheckDisplays()
{
    HMONITOR primary = MonitorFromPoint(POINT{}, MONITOR_DEFAULTTOPRIMARY);
    HMONITOR external = reinterpret_cast<HMONITOR>(INT_PTR(2));
    std::vector<DisplayInfo> displays{{primary, L"primary", {0, 0, 1920, 1080}, {0, 0, 1920, 1040}, true},
        {external, L"external", {-2560, -200, 0, 1240}, {-2560, -200, 0, 1200}, false}};
    Require(ResolveDisplay(displays, L"external") == external, "select an external display with negative coordinates");
    Require(ResolveDisplay(displays, L"unplugged") == primary, "disconnected display falls back to system primary");
    displays.pop_back();
    Require(ResolveDisplay(displays, L"external") == primary, "unplug falls back without changing the saved ID");
    displays.push_back({external, L"external", {}, {}, false});
    Require(ResolveDisplay(displays, L"external") == external, "reconnection restores the selected display");
    auto actual = ConnectedDisplays();
    Require(!actual.empty(), "enumerate connected physical displays");
    for (const DisplayInfo& display : actual)
        Require(!display.id.empty() && ResolveDisplay(actual, display.id) == display.monitor, "enumerated device IDs resolve to their monitor");
}

void CheckTaskDistribution()
{
    HMONITOR primary = reinterpret_cast<HMONITOR>(INT_PTR(1)), external = reinterpret_cast<HMONITOR>(INT_PTR(2));
    HWND first = reinterpret_cast<HWND>(INT_PTR(11)), second = reinterpret_cast<HWND>(INT_PTR(22));
    auto monitorOf = [&](HWND window) { return window == first ? primary : external; };
    TaskItem pinned, shared, other;
    pinned.key = L"pin"; pinned.pinned = true; pinned.windows = {first}; pinned.active = pinned.attention = true;
    shared.key = L"shared"; shared.windows = {first, second}; shared.active = true;
    other.key = L"external"; other.windows = {second};
    std::vector<TaskItem> items{pinned, shared, other};
    auto all = TaskItemsForDisplay(items, L"all", false, external, first, monitorOf);
    Require(all.size() == 3 && all[1].windows.size() == 2, "all taskbars include all app windows");
    auto main = TaskItemsForDisplay(items, L"primaryAndWindow", true, primary, first, monitorOf);
    Require(main.size() == 3 && main[1].windows.size() == 2, "primary taskbar includes every app");
    auto local = TaskItemsForDisplay(items, L"primaryAndWindow", false, external, first, monitorOf);
    Require(local.size() == 3 && local[0].windows.empty() && !local[0].active && !local[0].attention, "remote pins stay without running or active indicators");
    Require(local[1].windows == std::vector<HWND>{second} && !local[1].active, "window previews only include local windows");
    local = TaskItemsForDisplay(items, L"window", true, primary, first, monitorOf);
    Require(local.size() == 2 && local[1].windows == std::vector<HWND>{first} && local[1].active, "window-only mode also filters the primary taskbar");
    auto moved = TaskItemsForDisplay(items, L"window", false, external, second, [&](HWND) { return external; });
    Require(moved.size() == 3 && moved[0].windows.size() == 1 && moved[1].windows.size() == 2 && moved[1].active, "moving windows across screens updates membership and active state");
    Require(items[1].windows.size() == 2 && items[0].active, "per-display filtering leaves the shared model intact");
}

void CheckFullscreenCoverage()
{
    const HMONITOR primary = reinterpret_cast<HMONITOR>(INT_PTR(1)), external = reinterpret_cast<HMONITOR>(INT_PTR(2));
    const RECT screen{0, 0, 1920, 1080}, leftScreen{-2560, -200, 0, 1240};
    struct Window { HMONITOR monitor; RECT frame; bool maximized = false; };
    const Window fullscreen{primary, screen}, normal{primary, {100, 100, 900, 700}}, other{external, {-2400, 0, -1000, 900}};
    auto covered = [&](std::initializer_list<Window> windows) {
        for (const Window& window : windows)
            if (auto decision = WindowCoverOnMonitor(window.monitor, window.frame, window.maximized, primary, screen))
                return *decision;
        return false;
    };
    Require(covered({fullscreen}), "focused full-screen app hides its bar");
    Require(covered({other, fullscreen}), "focus on another monitor must not reveal the full-screen monitor's bar");
    Require(covered({{external, leftScreen}, fullscreen}), "each full-screen monitor hides its own bar independently");
    Require(!covered({normal, fullscreen}), "a normal app in front on the same monitor restores its bar");
    Require(!covered({other, normal, fullscreen}), "an unrelated foreground monitor does not override the local normal app");
    Require(!covered({other}), "minimizing or closing the local full-screen app restores its bar");
    Require(covered({other, {primary, {0, 0, 1920, 1040}, true}}), "an inactive maximized local app keeps auto-hide");
    Require(covered({{primary, {}}, fullscreen}), "empty frames do not override a full-screen app");
    Require(WindowCoverOnMonitor(external, leftScreen, false, external, leftScreen) == true, "full-screen detection supports negative display coordinates");

    Require(AtDisplayBottom({960, 1079}, screen) && AtDisplayBottom({960, 1078}, screen), "the local bottom edge can still reveal the bar");
    Require(!AtDisplayBottom({960, 1080}, screen) && !AtDisplayBottom({960, 1600}, screen), "a lower display must not repeatedly reveal the upper display's bar");
    Require(!AtDisplayBottom({1920, 1079}, screen) && !AtDisplayBottom({960, 1077}, screen), "another display or a point above the edge does not reveal the bar");
    Require(AtDisplayBottom({-1280, 1239}, leftScreen), "edge reveal supports negative display coordinates");
}

void CheckNotificationLayout()
{
    const RECT work{0, 0, 1920, 1040};
    auto single = notification::TopRightLayout(work, 1, 384, 112, 10, 12, 1);
    auto burst = notification::TopRightLayout(work, 1, 384, 112, 10, 12, 5);
    Require(single.rect.right == 1908 && single.rect.top == 12, "popup stays at the primary work area's top right");
    Require(EqualRect(&single.rect, &burst.rect), "new arrivals do not resize or move the popup viewport");
    Require(burst.contentHeight == 5 * 112 + 4 * 10, "all notifications have full-height slots separated by gaps");
    const float oldTop = notification::CardTop(0, 112, 10, 0), newTop = notification::CardTop(1, 112, 10, 0);
    const float moving = motion::Approach(oldTop, newTop, 16, 16);
    Require(oldTop == 0 && newTop == 122 && moving > oldTop && moving < newTop, "a new top notification makes existing cards animate down");
    Require(notification::CardTop(2, 112, 10, 122) == 122, "overflow scrolls older notifications upward into view");
    auto many = notification::TopRightLayout(work, 1, 384, 112, 10, 12, 50);
    Require(many.contentHeight > many.viewHeight && many.rect.bottom <= work.bottom, "notification bursts stay inside the work area and can scroll");
    const RECT negative{-2560, -200, 0, 1200};
    auto scaled = notification::TopRightLayout(negative, 1.5f, 384, 112, 10, 12, 2);
    Require(scaled.rect.right == -18 && scaled.rect.top == -182 && scaled.rect.right - scaled.rect.left == 576, "top-right placement supports negative coordinates and monitor DPI");
    const RECT small{0, 40, 320, 220};
    auto narrow = notification::TopRightLayout(small, 2, 384, 112, 10, 12, 3);
    Require(narrow.rect.left >= small.left && narrow.rect.top >= small.top && narrow.rect.right <= small.right && narrow.rect.bottom <= small.bottom,
        "small or highly scaled work areas clamp the notification viewport");

    const float controls = 44 + 10; // existing centre capsule and card gap
    auto centre = notification::TopRightLayout(work, 1, 384, 112, 10, 12, 5, controls);
    Require(EqualRect(&centre.rect, &burst.rect), "the bell's notification centre uses the same top-right corner as popup notifications");
    Require(centre.contentTop == controls && centre.viewHeight + centre.contentTop == burst.viewHeight,
        "the centre reserves visible controls above its list without leaving the primary work area");
    Require(centre.contentHeight == burst.contentHeight && burst.contentTop == 0,
        "the notification centre shows every card while popup spacing stays unchanged");
    Require(centre.contentTop + notification::CardTop(0, 112, 10, 0) == controls &&
        centre.contentTop + notification::CardTop(1, 112, 10, 0) == controls + 122,
        "the centre's newest notification starts below its controls and older notifications follow downward");
    auto emptyCentre = notification::TopRightLayout(work, 1, 384, 112, 10, 12, 0, controls);
    Require(EqualRect(&emptyCentre.rect, &centre.rect) && emptyCentre.contentHeight == 0 && emptyCentre.contentTop == controls,
        "an empty notification centre keeps its controls at the top-right corner");
    auto longCentre = notification::TopRightLayout(work, 1, 384, 112, 10, 12, 50, controls);
    Require(longCentre.contentHeight > longCentre.viewHeight && longCentre.rect.bottom <= work.bottom,
        "long centre lists scroll below the fixed top-right controls");
    auto scaledCentre = notification::TopRightLayout(negative, 1.5f, 384, 112, 10, 12, 2, controls);
    Require(EqualRect(&scaledCentre.rect, &scaled.rect) && scaledCentre.contentTop == controls,
        "the notification centre shares the popup's negative-coordinate and DPI placement");
}
}

int main()
{
    try {
        winrt::init_apartment();
        CheckSettings();
        CheckDisplays();
        CheckTaskDistribution();
        CheckFullscreenCoverage();
        CheckNotificationLayout();
        std::cout << "Display, fullscreen auto-hide, notification placement and animated reflow checks passed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
