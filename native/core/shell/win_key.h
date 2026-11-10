// The Windows key alone opens Atrium's start menu (setting winKeyStart), Win +
// 1..9, 0 the bar's apps (winNumbers); both off by default. A low-level
// keyboard hook on a thread of its own holds the key back as it goes down:
// when another key follows, the two go on to Windows together (Win + E and
// every other shortcut work as before); a digit is kept and `notify` hears of
// it; when the key comes up alone, Windows never saw it, so its start menu
// stays shut and `notify` hears of it instead (or, without winKeyStart, the
// key goes on to Windows after all). Over an elevated window, whose input
// Atrium may not send to, the key is left alone. Nothing is injected into Explorer.
#pragma once

#include <windows.h>

#include <atomic>
#include <thread>

namespace atrium {

class WinKey {
public:
    // Posts `msg` to `notify`: wParam 0 when the Windows key was pressed alone
    // (if `alone`), 1..10 for Win + 1..9, 0 (if `numbers`).
    WinKey(HWND notify, UINT msg, bool alone, bool numbers);
    ~WinKey();
    WinKey(const WinKey&) = delete;
    WinKey& operator=(const WinKey&) = delete;

private:
    static LRESULT CALLBACK Hook(int code, WPARAM w, LPARAM l);
    void Run();
    HWND notify_;
    UINT msg_;
    bool alone_, numbers_;
    DWORD threadId_ = 0;
    std::atomic<bool> ready_{false};
    bool held_ = false;   // a Windows key is down and held back
    bool passed_ = false; // it went on to Windows (another key followed, or an elevated window is in front)
    bool used_ = false;   // a digit went with it: no start menu when it comes up
    DWORD digit_ = 0;     // the digit kept, until it comes up
    DWORD key_ = 0, scan_ = 0; // which one (VK_LWIN or VK_RWIN), as it came, to send it on
    bool extended_ = false;
    std::thread thread_;
};

} // namespace atrium
