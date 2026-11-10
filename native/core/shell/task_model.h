// Task model for the bar: pinned apps plus running app windows, grouped per
// application (D13). Lives on the UI thread (WinEvent hooks need its message
// loop); the bar reads immutable snapshots.
#pragma once

#include <windows.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace atrium {

// A pinned entry as stored in settings.json ("pins", docs/SETTINGS-PROTOCOL.md).
struct PinnedApp {
    std::wstring name;  // display name
    std::wstring path;  // .lnk, .exe or other launchable file; may be empty for Store apps
    std::wstring args;  // extra arguments when `path` is an .exe
    std::wstring aumid; // AppUserModelID, empty when unknown
    std::optional<bool> multipleWindows; // absent: browsers and Explorer allow new windows; others reuse
    bool AllowsMultipleWindows() const;
    bool operator==(const PinnedApp&) const = default;
};

// Premultiplied BGRA, top-down, width * height * 4 bytes.
struct IconImage {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> bgra;
    bool empty() const { return bgra.empty(); }
};

struct TaskItem {
    std::wstring key;   // stable grouping key: lowercase AUMID if known, else lowercase exe path
    std::wstring name;  // pinned name, else window/app display name
    bool pinned = false;
    int pinIndex = -1;  // position in the pin list, -1 if not pinned
    std::vector<HWND> windows; // visible app windows of this group, most recently active first
    bool active = false;       // one of `windows` is the foreground window
    bool attention = false;    // a window is flashing (FlashWindowEx / HSHELL_FLASH)
    uint64_t iconVersion = 0;  // changes when the icon should be fetched again
    bool multipleWindows = false;
};

// Icon of a shell item (a path or "shell:AppsFolder\\<id>") through
// IShellItemImageFactory, square, premultiplied. Blocking; call it off the UI
// thread. COM must be initialised on the calling thread.
// thumbnail: the picture itself (an image file) rather than its icon.
bool LoadShellImage(const std::wstring& parsingName, int sizePx, IconImage& out, bool thumbnail = false);
// Atrium's own programs (Atrium.exe, Atrium.Settings.exe): their icon read from the file
// itself, not from the shell's icon cache or Explorer's tray snapshot, which keep the icon an
// earlier build had at the same path. False for any other program.
bool LoadAtriumIcon(const std::wstring& exePath, int sizePx, IconImage& out);
// A Win32 icon handle copied to premultiplied BGRA. The handle may belong to another
// process: the tray broker receives those with every Shell_NotifyIcon message. sizePx 0
// keeps the icon's own size; otherwise it is fitted into that square.
bool CopyIconImage(HICON icon, int sizePx, IconImage& out);
// Resolves a shortcut and a versioned client's app/ launcher without starting it.
std::wstring AppExecutable(const std::wstring& path);

class TaskModel {
public:
    // Calls `onChanged` on the UI thread (coalesced, at most every ~30 ms)
    // whenever Snapshot() would return something different.
    TaskModel(HWND uiWindow, std::function<void()> onChanged,
              std::function<bool(const std::wstring&)> openExisting = {});
    ~TaskModel();
    TaskModel(const TaskModel&) = delete;
    TaskModel& operator=(const TaskModel&) = delete;

    // Pinned items first in pin order, then running unpinned groups in the
    // order they first appeared. Unpinned groups are omitted if !showRunning,
    // unless `all` (Stage Manager sees every app).
    std::vector<TaskItem> Snapshot(bool all = false) const;

    void SetPins(const std::vector<PinnedApp>& pins);
    void SetShowRunning(bool show);

    // Icon for an item at `sizePx` (square), cached per key and size. Never
    // blocks for long: returns empty until loaded, then onChanged fires.
    IconImage Icon(const std::wstring& key, int sizePx);

    // Left click, like the Windows taskbar: no window -> launch; one window ->
    // minimize if it is the foreground window, otherwise restore and activate;
    // several windows -> activate the next one in turn.
    void Activate(const std::wstring& key);
    // Middle click / "new window": start another instance.
    void Launch(const std::wstring& key);
    // Politely close every window of the group (WM_CLOSE).
    void CloseAll(const std::wstring& key);
    // One window of a group (the window previews): restore and bring it to the front, or close it.
    static void ActivateWindow(HWND window);
    // True means the native close command was queued; the application still
    // owns confirmation, cancellation and close-to-tray behavior.
    static bool CloseWindow(HWND window);
    // A pin for a running unpinned item (to append to settings).
    PinnedApp PinFor(const std::wstring& key) const;
    // Brings the group's most recent window to the front (never minimizes or
    // launches); used while files are dragged over the item.
    void Show(const std::wstring& key);
    // Moves a running unpinned group to `toIndex` among the unpinned groups.
    void MoveRunning(const std::wstring& key, int toIndex);
    // Files dropped on an item: open them with that app.
    bool CanOpenFiles(const std::wstring& key) const;
    bool OpenFiles(const std::wstring& key, const std::vector<std::wstring>& files);
    // The desktop exe behind an item ("open file location"); empty for packaged apps.
    std::wstring FileLocation(const std::wstring& key) const;

    // The user's current Windows taskbar pins (desktop apps, from
    // %APPDATA%\Microsoft\Internet Explorer\Quick Launch\User Pinned\TaskBar).
    static std::vector<PinnedApp> ImportWindowsPins();

    // Forwarded from the UI window procedure; returns true if handled.
    bool HandleMessage(UINT msg, WPARAM wp, LPARAM lp);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace atrium
