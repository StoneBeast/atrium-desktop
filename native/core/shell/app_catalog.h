// All installed apps, as the Windows start menu lists them (the shell's
// AppsFolder: Store apps and start menu shortcuts), and the apps pinned to the
// Windows start menu, in its order. Enumerated and iconified on a worker
// thread; the UI thread reads snapshots.
#pragma once

#include "shell/task_model.h"

#include <windows.h>

#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace atrium {

struct CatalogApp {
    std::wstring name;    // display name
    std::wstring id;      // parsing name inside AppsFolder (AUMID or known-folder path), or a shortcut's full path
    std::wstring aumid;   // System.AppUserModel.ID, may be empty
    std::wstring target;  // link target exe, may be empty
    std::wstring args;    // the link's arguments (a browser's --app=..., a profile)
};

class AppCatalog {
public:
    // Posts `readyMsg` to `notify` when the list or an icon becomes available.
    AppCatalog(HWND notify, UINT readyMsg);
    ~AppCatalog();
    AppCatalog(const AppCatalog&) = delete;
    AppCatalog& operator=(const AppCatalog&) = delete;

    void Refresh();                       // enumerate again (asynchronous)
    bool Loaded() const;
    std::vector<CatalogApp> Apps() const; // sorted by name (user locale)
    // Pinned to the Windows start menu (not the taskbar: those are the bar's pins).
    std::vector<CatalogApp> StartPins() const;
    IconImage Icon(const std::wstring& id, int px); // empty until loaded
    // The start pins' icons at `px` are read after every refresh, so the start
    // menu's first opening shows them at once (requests for icons on screen go first).
    void PrefetchStartIcons(int px);
    // The apps of the start menu's own arrangement (start.json): read ahead and kept like the pins'.
    void SetStartIds(std::vector<std::wstring> ids);
    void SetVisibleIcons(std::vector<std::pair<std::wstring, int>> icons);

    static bool Launch(const CatalogApp& app);
    static PinnedApp PinFor(const CatalogApp& app);
    // Case-insensitive match of `query` against the name (and words in it), or the start of
    // its initials (NameInitials: "wx" finds 微信).
    static bool Matches(const CatalogApp& app, const std::wstring& query);

private:
    void Worker();
    HWND notify_;
    UINT msg_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<CatalogApp> apps_, startPins_;
    // Worker only: the exported layout and the layout file's time it is from.
    std::vector<std::pair<std::wstring, std::wstring>> layout_; // key (packagedAppId | desktopAppLink), value
    FILETIME layoutTime_{};
    bool loaded_ = false, refresh_ = false, stop_ = false;
    int prefetchPx_ = 0;
    void QueuePrefetch(); // under mutex_
    void PruneIcons(); // under mutex_
    bool QueueIcon(const std::pair<std::wstring, int>& key, bool prefetch);
    std::deque<std::pair<std::wstring, int>> iconJobs_;
    struct CachedIcon { IconImage image; uint64_t used = 0; };
    struct FailedIcon { unsigned attempts = 0; ULONGLONG time = 0; };
    std::map<std::pair<std::wstring, int>, CachedIcon> icons_;
    std::map<std::pair<std::wstring, int>, FailedIcon> failed_;
    std::set<std::pair<std::wstring, int>> requested_, visibleIcons_;
    std::set<std::wstring> liveIds_, startIds_;
    uint64_t iconUse_ = 0;
    std::thread thread_;
};

} // namespace atrium
