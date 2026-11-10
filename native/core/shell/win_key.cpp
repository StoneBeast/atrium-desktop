#include "shell/win_key.h"

#include "log.h"

namespace atrium {

namespace {

// Marks the keys this hook sends on to Windows, so it lets them through.
constexpr ULONG_PTR kOurs = 0x41545257; // "ATRW"

WinKey* g_winKey = nullptr; // the hook procedure has no context of its own

// The window in front runs elevated: keys Atrium sends would not reach it.
bool ForegroundElevated()
{
    DWORD pid = 0;
    HWND fg = GetForegroundWindow();
    if (!fg || !GetWindowThreadProcessId(fg, &pid) || !pid)
        return false;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process)
        return true;
    bool elevated = true; // its token closed to us: elevated (or protected)
    HANDLE token = nullptr;
    if (OpenProcessToken(process, TOKEN_QUERY, &token)) {
        TOKEN_ELEVATION e{};
        DWORD size = 0;
        elevated = GetTokenInformation(token, TokenElevation, &e, sizeof(e), &size) && e.TokenIsElevated;
        CloseHandle(token);
    }
    CloseHandle(process);
    return elevated;
}

INPUT Key(DWORD vk, DWORD scan, bool extended)
{
    INPUT i{};
    i.type = INPUT_KEYBOARD;
    i.ki.wVk = WORD(vk);
    i.ki.wScan = WORD(scan);
    i.ki.dwFlags = extended ? KEYEVENTF_EXTENDEDKEY : 0;
    i.ki.dwExtraInfo = kOurs;
    return i;
}

} // namespace

WinKey::WinKey(HWND notify, UINT msg, bool alone, bool numbers) : notify_(notify), msg_(msg), alone_(alone), numbers_(numbers)
{
    g_winKey = this;
    thread_ = std::thread([this] { Run(); });
    // The hook is in place before the constructor returns (or failed and said so).
    while (!ready_.load())
        Sleep(1);
}

WinKey::~WinKey()
{
    if (threadId_)
        PostThreadMessageW(threadId_, WM_QUIT, 0, 0);
    thread_.join();
    g_winKey = nullptr;
}

void WinKey::Run()
{
    threadId_ = GetCurrentThreadId();
    MSG msg;
    PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE); // the thread's queue, for WM_QUIT
    HHOOK hook = SetWindowsHookExW(WH_KEYBOARD_LL, Hook, GetModuleHandleW(nullptr), 0);
    if (hook)
        Log(L"winkey: watching the Windows key (%s%s%s)", alone_ ? L"start menu" : L"", alone_ && numbers_ ? L", " : L"", numbers_ ? L"Win + digits" : L"");
    else
        Log(L"winkey: could not watch the keyboard (%lu)", GetLastError());
    ready_ = true;
    // Low-level hooks are called on this thread while it waits for messages:
    // it does nothing else, so a busy UI thread never delays a key.
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
        DispatchMessageW(&msg);
    if (hook)
        UnhookWindowsHookEx(hook);
}

LRESULT CALLBACK WinKey::Hook(int code, WPARAM w, LPARAM l)
{
    WinKey* self = g_winKey;
    const auto* k = reinterpret_cast<const KBDLLHOOKSTRUCT*>(l);
    if (code != HC_ACTION || !self || k->dwExtraInfo == kOurs)
        return CallNextHookEx(nullptr, code, w, l);
    const bool down = w == WM_KEYDOWN || w == WM_SYSKEYDOWN;
    const bool win = k->vkCode == VK_LWIN || k->vkCode == VK_RWIN;
    if (win && down) {
        if (!self->held_) { // not the key's auto-repeat
            self->held_ = true;
            self->used_ = false;
            self->key_ = k->vkCode;
            self->scan_ = k->scanCode;
            self->extended_ = (k->flags & LLKHF_EXTENDED) != 0;
            self->passed_ = ForegroundElevated();
        }
        return self->passed_ ? CallNextHookEx(nullptr, code, w, l) : 1; // held back
    }
    if (win) {
        if (self->held_ && k->vkCode == self->key_) {
            const bool held = !self->passed_;
            self->held_ = false;
            if (held) { // Windows never saw it go down
                if (self->used_)
                    return 1;
                if (self->alone_) { // no start menu of its own
                    PostMessageW(self->notify_, self->msg_, 0, 0);
                    return 1;
                }
                // Only the digits are ours: Windows' start menu, as if never held.
                INPUT keys[2] = {Key(self->key_, self->scan_, self->extended_), Key(self->key_, self->scan_, self->extended_)};
                keys[1].ki.dwFlags |= KEYEVENTF_KEYUP;
                SendInput(2, keys, sizeof(INPUT));
                return 1;
            }
        }
        return CallNextHookEx(nullptr, code, w, l);
    }
    if (self->digit_ && k->vkCode == self->digit_) { // the kept digit: its repeats and its release
        if (!down)
            self->digit_ = 0;
        return 1;
    }
    if (down && self->held_ && !self->passed_) {
        const bool digit = k->vkCode >= '0' && k->vkCode <= '9';
        const bool modified = (GetAsyncKeyState(VK_SHIFT) | GetAsyncKeyState(VK_CONTROL) | GetAsyncKeyState(VK_MENU)) & 0x8000;
        if (digit && self->numbers_ && !modified) {
            self->used_ = true;
            self->digit_ = k->vkCode;
            PostMessageW(self->notify_, self->msg_, k->vkCode == '0' ? 10 : k->vkCode - '0', 0);
            return 1;
        }
        // Another key: a shortcut. The Windows key goes on to Windows first, then this key.
        self->passed_ = true;
        INPUT keys[2] = {Key(self->key_, self->scan_, self->extended_), Key(k->vkCode, k->scanCode, (k->flags & LLKHF_EXTENDED) != 0)};
        SendInput(2, keys, sizeof(INPUT));
        return 1;
    }
    return CallNextHookEx(nullptr, code, w, l);
}

} // namespace atrium
