// The notification area's icons (the system tray), for Atrium's own tray
// panel. Read from the shell's UI Automation tree: the taskbar's and the
// overflow window's tray buttons are there even while hidden, named by their
// tooltips. A click is forwarded the same way: Invoke is the icon's left click,
// ShowContextMenu its right click (the app opens its own menu). Nothing is
// injected into Explorer. Reading never opens its UI; an explicit icon action
// may initialize its lazy overflow. The picture of
// an icon comes from Windows' record of tray icons (NotifyIconSettings: the
// program and a snapshot of its icon), matched to the button by name, or from
// the program's file. UI Automation runs on a worker thread.
#pragma once

#include "shell/task_model.h"

#include <windows.h>

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace atrium {

struct TrayIcon {
    std::wstring tip;      // the tooltip; may span lines
    std::wstring exe;      // the program it belongs to, when recognised
    std::shared_ptr<const IconImage> image; // null when unknown
    bool promoted = false; // on the system taskbar rather than in its overflow
    int index = 0;         // position among the icons of the same place
    std::wstring recordId; // stable Windows identity, also before the overflow UI exists
    unsigned uid = 0;      // the program's own id for the icon (NOTIFYICONDATA.uID)
    bool hasGuid = false;  // the icon's stable id, when the program registers one
    GUID guid{};
};

class TrayModel {
public:
    // Posts `msg` to `notify` when the list has been read (wParam 0) and when a
    // click has been forwarded (wParam 1).
    TrayModel(HWND notify, UINT msg, std::function<std::vector<PinnedApp>()> appNames = {});
    ~TrayModel();
    TrayModel(const TrayModel&) = delete;
    TrayModel& operator=(const TrayModel&) = delete;

    void Refresh(); // asynchronous
    // Swaps in the live icons the tray broker has received since the last read
    // (a blink delivers a new icon every few hundred milliseconds). Cheap: one
    // lookup per icon, no UI Automation.
    bool LiveRefresh();
    std::vector<TrayIcon> Icons() const;
    bool Loaded() const;
    bool Available() const; // the shell's tray was found in the last read
    // A left click (Invoke) or a right click (the context menu) on `icon`.
    void Click(const TrayIcon& icon, bool right);
    // Opens an existing background session; false when the app is not in the tray/running.
    bool OpenExisting(const std::wstring& exe);

    // First line of a tooltip, for display.
    static std::wstring Title(const std::wstring& tip);

private:
    struct Job {
        bool refresh = false;
        TrayIcon icon;
        bool right = false;
    };
    void Worker();
    HWND notify_;
    UINT msg_;
    std::function<std::vector<PinnedApp>()> appNames_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Job> jobs_;
    std::vector<TrayIcon> icons_;
    bool loaded_ = false, available_ = false, stop_ = false;
    std::thread thread_;
};

} // namespace atrium
