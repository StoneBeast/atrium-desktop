#include "shell/tray_broker.h"

#include "log.h"

#include <shlwapi.h>
#include <windowsx.h>

#include <algorithm>
#include <mutex>
#include <vector>

#pragma comment(lib, "shlwapi.lib")

namespace atrium {

namespace {

constexpr wchar_t kTrayClass[] = L"Shell_TrayWnd";
constexpr wchar_t kNotifyClass[] = L"TrayNotifyWnd";
constexpr DWORD kSignature = 0x34753423; // TRAYNOTIFYDATAW's first field
constexpr UINT_PTR kRaiseTimer = 0x7A01;
constexpr UINT_PTR kNotifyTimer = 0x7A02;
constexpr UINT kRaiseMs = 250;   // Explorer raises its own tray now and then
constexpr UINT kNotifyMs = 40;   // live updates reach the panel in bursts
constexpr UINT kForwardTimeoutMs = 2000;
constexpr UINT kNinSelect = WM_USER;

enum CopyData : ULONG_PTR { kAppBar = 0, kIcon = 1, kRect = 3 };
enum IconMessage : DWORD { kAdd = 0, kModify = 1, kDelete = 2, kSetFocus = 3, kSetVersion = 4 };

// The shell sends the 32-bit layout of NOTIFYICONDATAW whatever the caller's
// bitness is: window and GDI handles are 32 bits there.
struct NotifyIconData32 {
    DWORD cbSize;
    DWORD hWnd;
    UINT uID;
    UINT uFlags;
    UINT uCallbackMessage;
    DWORD hIcon;
    WCHAR szTip[128];
    DWORD dwState;
    DWORD dwStateMask;
    WCHAR szInfo[256];
    UINT uVersion;
    WCHAR szInfoTitle[64];
    DWORD dwInfoFlags;
    GUID guidItem;
    DWORD hBalloonIcon;
};
static_assert(sizeof(NotifyIconData32) == 956, "the wire NOTIFYICONDATAW is the 32-bit layout");

struct TrayNotifyData32 {
    DWORD signature;
    DWORD message;
    NotifyIconData32 nid;
};

// APPBARMSGDATAV2/V3 as the shell sends them (32-bit fields, 8-byte pointers).
struct AppBarData32 {
    DWORD cbSize;
    DWORD hWnd;
    DWORD uCallbackMessage;
    DWORD uEdge;
    RECT rc;
    LONG64 lParam;
};
static_assert(sizeof(AppBarData32) == 40, "APPBARDATAV2 wire size");

struct AppBarMessage32 {
    AppBarData32 abd;
    DWORD message;
    DWORD padding;
    LONG64 shared; // SHAllocShared handle owned by the caller
    DWORD sourcePid;
    DWORD padding2;
};
static_assert(sizeof(AppBarMessage32) == 64, "APPBARMSGDATAV3 wire size");

std::wstring Lower(std::wstring s)
{
    if (!s.empty())
        CharLowerBuffW(s.data(), DWORD(s.size()));
    return s;
}

struct Entry {
    HWND window = nullptr;
    UINT uid = 0;
    bool hasGuid = false;
    GUID guid{};
    std::wstring exe; // lower case; empty when the process could not be asked
    std::wstring tip;
    UINT callback = 0;
    UINT version = 0;
    std::shared_ptr<const IconImage> image;
};

// The program that owns the icon: the window's process.
std::wstring ProcessPath(HWND window)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (!pid)
        return {};
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process)
        return {};
    wchar_t path[32768];
    DWORD length = DWORD(std::size(path));
    std::wstring out;
    if (QueryFullProcessImageNameW(process, 0, path, &length))
        out.assign(path, length);
    CloseHandle(process);
    return Lower(std::move(out));
}

LRESULT CALLBACK TrayProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);

} // namespace

struct TrayBroker::Impl {
    HWND window = nullptr;
    HWND child = nullptr;
    HWND target = nullptr; // Explorer's tray window
    HWND notify = nullptr;
    UINT notifyMessage = 0;
    UINT_PTR notifyTimer = 0;
    HINSTANCE instance = nullptr;
    RECT geometry{};
    bool haveGeometry = false;
    std::mutex mutex;
    std::vector<Entry> icons;

    HWND FindTarget()
    {
        if (target && IsWindow(target) && target != window)
            return target;
        target = nullptr;
        for (HWND w = FindWindowExW(nullptr, nullptr, kTrayClass, nullptr); w; w = FindWindowExW(nullptr, w, kTrayClass, nullptr))
            if (w != window) {
                target = w;
                break;
            }
        return target;
    }

    // Explorer raises its tray now and then; whoever is first in z-order is the
    // window Shell_NotifyIcon finds.
    void RequireTop()
    {
        if (!window)
            return;
        HWND found = FindWindowW(kTrayClass, nullptr);
        if (found != window)
            SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        // Keep our geometry where Windows' tray is: programs do look at the
        // rectangle of the window they find.
        if (HWND t = FindTarget()) {
            RECT r{};
            if (GetWindowRect(t, &r) && (!haveGeometry || memcmp(&r, &geometry, sizeof r) != 0)) {
                geometry = r;
                haveGeometry = true;
                SetWindowPos(window, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE);
                if (child)
                    SetWindowPos(child, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE);
            }
        }
    }

    LRESULT Forward(const COPYDATASTRUCT* data, WPARAM wparam)
    {
        HWND t = FindTarget();
        if (!t)
            return 0;
        DWORD_PTR result = 0;
        SetLastError(0);
        LRESULT sent = SendMessageTimeoutW(t, WM_COPYDATA, wparam, reinterpret_cast<LPARAM>(data),
                                           SMTO_ABORTIFHUNG | SMTO_BLOCK, kForwardTimeoutMs, &result);
        if (!sent && GetLastError()) {
            target = nullptr; // Explorer may be restarting
            return 0;
        }
        return static_cast<LRESULT>(result);
    }

    // AppBar messages (SHAppBarMessage) come to the tray window too. The ones
    // with shared memory must be re-routed: Explorer needs a block allocated for
    // its own process, so allocate one, forward, then copy the answer back.
    LRESULT OnAppBar(const COPYDATASTRUCT* data, WPARAM wparam)
    {
        if (!data->lpData || data->cbData < static_cast<int>(sizeof(AppBarMessage32)))
            return Forward(data, wparam);
        AppBarMessage32 amd{};
        memcpy(&amd, data->lpData, sizeof(amd));
        HWND t = FindTarget();
        if (!t)
            return 0;
        if (!amd.shared)
            return Forward(data, wparam);

        DWORD pid = 0;
        GetWindowThreadProcessId(t, &pid);
        void* source = SHLockShared(reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(amd.shared)), amd.sourcePid);
        if (!source)
            return Forward(data, wparam);
        void* shared = SHAllocShared(nullptr, sizeof(AppBarData32), pid);
        if (!shared) {
            SHUnlockShared(source);
            return Forward(data, wparam);
        }
        void* fresh = SHLockShared(shared, pid);
        if (!fresh) {
            SHFreeShared(shared, pid);
            SHUnlockShared(source);
            return Forward(data, wparam);
        }
        memcpy(fresh, source, sizeof(AppBarData32));
        SHUnlockShared(fresh);

        AppBarMessage32 copy = amd;
        copy.shared = static_cast<LONG64>(reinterpret_cast<UINT_PTR>(shared));
        copy.sourcePid = pid;
        COPYDATASTRUCT forwarded{};
        forwarded.dwData = kAppBar;
        forwarded.cbData = sizeof(copy);
        forwarded.lpData = &copy;
        DWORD_PTR result = 0;
        if (SendMessageTimeoutW(t, WM_COPYDATA, wparam, reinterpret_cast<LPARAM>(&forwarded),
                                SMTO_ABORTIFHUNG | SMTO_BLOCK, kForwardTimeoutMs, &result)) {
            if (void* back = SHLockShared(shared, pid)) {
                memcpy(source, back, sizeof(AppBarData32));
                SHUnlockShared(back);
            }
        } else {
            target = nullptr;
        }
        SHFreeShared(shared, pid);
        SHUnlockShared(source);
        return static_cast<LRESULT>(result);
    }

    void OnIcon(const COPYDATASTRUCT* data)
    {
        if (!data->lpData || data->cbData < 8)
            return;
        const BYTE* base = static_cast<const BYTE*>(data->lpData);
        DWORD signature = 0, message = 0;
        memcpy(&signature, base, 4);
        memcpy(&message, base + 4, 4);
        if (signature != kSignature)
            return; // not ours: forward untouched

        const BYTE* raw = base + 8;
        DWORD available = DWORD(data->cbData) - 8;
        if (available < 24)
            return;
        DWORD size = 0;
        memcpy(&size, raw, 4);
        size = std::min(size, available);
        if (size < 24)
            return;
        NotifyIconData32 nid{};
        memcpy(&nid, raw, std::min<size_t>(size, sizeof(nid)));
        if (message == kSetFocus)
            return; // no identity of its own: Explorer puts the focus back

        HWND owner = reinterpret_cast<HWND>(static_cast<UINT_PTR>(nid.hWnd));
        const bool hasGuid = (nid.uFlags & 0x20 /* NIF_GUID */) != 0 && !IsEqualGUID(nid.guidItem, GUID_NULL);
        std::lock_guard g(mutex);
        Entry* entry = nullptr;
        for (Entry& e : icons) {
            if (hasGuid ? (e.hasGuid && IsEqualGUID(e.guid, nid.guidItem)) : (e.window == owner && e.uid == nid.uID)) {
                entry = &e;
                break;
            }
        }

        if (message == kDelete) {
            if (entry)
                icons.erase(icons.begin() + (entry - icons.data()));
            NotifyChanged();
            return;
        }
        if (!entry) {
            if (owner == nullptr)
                return; // an update for an icon this program has not added
            icons.push_back({});
            entry = &icons.back();
            entry->window = owner;
            entry->uid = nid.uID;
            entry->exe = ProcessPath(owner);
        }
        if (hasGuid) {
            entry->hasGuid = true;
            entry->guid = nid.guidItem;
        }
        if (nid.uFlags & 0x2 /* NIF_ICON */) {
            IconImage image;
            entry->image = (nid.hIcon && CopyIconImage(reinterpret_cast<HICON>(static_cast<UINT_PTR>(nid.hIcon)), 0, image) &&
                            !image.empty())
                               ? std::make_shared<const IconImage>(std::move(image))
                               : nullptr;
        }
        if (nid.uFlags & 0x4 /* NIF_TIP */) {
            wchar_t tip[129]{};
            memcpy(tip, nid.szTip, sizeof(nid.szTip));
            entry->tip = tip;
        }
        if (nid.uFlags & 0x1 /* NIF_MESSAGE */)
            entry->callback = nid.uCallbackMessage;
        if (message == kSetVersion || (nid.uVersion >= 3 && nid.uVersion <= 4))
            entry->version = nid.uVersion;
        NotifyChanged();
    }

    void NotifyChanged()
    {
        if (!window || !notify || notifyTimer)
            return; // a burst of icon updates becomes one notification
        notifyTimer = SetTimer(window, kNotifyTimer, kNotifyMs, nullptr);
    }

    const Entry* FindLocked(const std::wstring& exeLower, UINT uid, const GUID* guid) const
    {
        for (const Entry& e : icons) {
            if (guid && e.hasGuid && IsEqualGUID(e.guid, *guid))
                return &e;
            if (!e.exe.empty() && e.uid == uid && e.exe == exeLower)
                return &e;
        }
        return nullptr;
    }

    LRESULT Handle(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
    {
        switch (message) {
        case WM_COPYDATA: {
            const auto* data = reinterpret_cast<const COPYDATASTRUCT*>(lparam);
            if (!data)
                return 0;
            switch (data->dwData) {
            case kIcon:
                OnIcon(data);
                Forward(data, wparam);
                return TRUE;
            case kRect:
                return Forward(data, wparam);
            case kAppBar:
                return OnAppBar(data, wparam);
            default:
                return Forward(data, wparam);
            }
        }
        case WM_TIMER:
            if (wparam == kRaiseTimer) {
                RequireTop();
                return 0;
            }
            if (wparam == kNotifyTimer) {
                KillTimer(window, kNotifyTimer);
                notifyTimer = 0;
                if (notify)
                    PostMessageW(notify, notifyMessage, 0, 0);
                return 0;
            }
            return 0;
        case WM_NCDESTROY:
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            break;
        default:
            break;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }
};

namespace {

LRESULT CALLBACK TrayProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    TrayBroker::Impl* impl = reinterpret_cast<TrayBroker::Impl*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        impl = static_cast<TrayBroker::Impl*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(impl));
    }
    if (!impl)
        return DefWindowProcW(window, message, wparam, lparam);
    return impl->Handle(window, message, wparam, lparam);
}

} // namespace

TrayBroker& Broker()
{
    static TrayBroker instance;
    return instance;
}

TrayBroker::TrayBroker() : impl_(std::make_unique<Impl>()) {}

TrayBroker::~TrayBroker()
{
    Stop();
}

bool TrayBroker::Running() const
{
    return impl_->window != nullptr;
}

bool TrayBroker::Start(HWND notify, UINT message)
{
    if (impl_->window)
        return true;
    wchar_t off[8]{};
    if (GetEnvironmentVariableW(L"ATRIUM_TRAY_BROKER", off, 8) && off[0] == L'0')
        return false;

    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = TrayProc;
    wc.hInstance = instance;
    wc.lpszClassName = kTrayClass;
    if (!RegisterClassExW(&wc)) {
        Log(L"broker: could not register the tray window (error %lu)", GetLastError());
        return false;
    }
    wc.lpfnWndProc = DefWindowProcW;
    wc.lpszClassName = kNotifyClass;
    if (!RegisterClassExW(&wc))
        Log(L"broker: could not register %s (error %lu)", kNotifyClass, GetLastError());

    impl_->instance = instance;
    impl_->notify = notify;
    impl_->notifyMessage = message;
    impl_->window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kTrayClass, L"",
                                    WS_POPUP | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0, 1, 1, nullptr, nullptr, instance,
                                    impl_.get());
    if (!impl_->window) {
        Log(L"broker: could not create the tray window (error %lu)", GetLastError());
        UnregisterClassW(kTrayClass, instance);
        UnregisterClassW(kNotifyClass, instance);
        return false;
    }
    impl_->child = CreateWindowExW(0, kNotifyClass, L"", WS_CHILD, 0, 0, 1, 1, impl_->window, nullptr, instance, nullptr);
    impl_->RequireTop();
    SetTimer(impl_->window, kRaiseTimer, kRaiseMs, nullptr);
    // Programs register their icons when the taskbar is (re)created; the same
    // message makes them register with us, so icons that existed before Atrium
    // started are live from the first paint.
    if (UINT created = RegisterWindowMessageW(L"TaskbarCreated"))
        SendNotifyMessageW(HWND_BROADCAST, created, 0, 0);
    Log(L"broker: tray window 0x%p above Explorer's, forwarding to 0x%p",
        static_cast<void*>(impl_->window), static_cast<void*>(impl_->FindTarget()));
    return true;
}

void TrayBroker::Stop()
{
    if (!impl_->window)
        return;
    HWND window = impl_->window;
    impl_->window = nullptr;
    KillTimer(window, kRaiseTimer);
    KillTimer(window, kNotifyTimer);
    impl_->notifyTimer = 0;
    if (impl_->child)
        DestroyWindow(impl_->child);
    impl_->child = nullptr;
    DestroyWindow(window);
    UnregisterClassW(kTrayClass, impl_->instance);
    UnregisterClassW(kNotifyClass, impl_->instance);
    // Whatever was registered with us also has to exist in Explorer's tray.
    if (UINT created = RegisterWindowMessageW(L"TaskbarCreated"))
        SendNotifyMessageW(HWND_BROADCAST, created, 0, 0);
    Log(L"broker: stopped");
}

std::shared_ptr<const IconImage> TrayBroker::Image(const std::wstring& exeLower, UINT uid, const GUID* guid) const
{
    if (!impl_->window)
        return {};
    std::lock_guard g(impl_->mutex);
    const Entry* entry = impl_->FindLocked(exeLower, uid, guid);
    return entry ? entry->image : std::shared_ptr<const IconImage>{};
}

bool TrayBroker::Click(const std::wstring& exeLower, UINT uid, const GUID* guid, bool right) const
{
    if (!impl_->window)
        return false;
    HWND window = nullptr;
    UINT callback = 0, id = 0, version = 0;
    {
        std::lock_guard g(impl_->mutex);
        const Entry* entry = impl_->FindLocked(exeLower, uid, guid);
        if (!entry || !entry->callback || !IsWindow(entry->window))
            return false;
        window = entry->window;
        callback = entry->callback;
        id = entry->uid;
        version = entry->version;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    AllowSetForegroundWindow(pid);
    POINT cursor{};
    GetCursorPos(&cursor);
    const UINT mouse = MAKELPARAM(cursor.x, cursor.y);
    auto send = [&](UINT msg) {
        // Version 4 packs the cursor in wParam and the icon id in lParam's high
        // word; earlier versions get the icon id in wParam (as Explorer does).
        const WPARAM w = version >= 4 ? mouse : id;
        const LPARAM l = version >= 4 ? LPARAM(msg | (id << 16)) : LPARAM(msg);
        SendNotifyMessageW(window, callback, w, l);
    };
    if (right) {
        send(WM_RBUTTONDOWN);
        send(WM_RBUTTONUP);
        if (version >= 3)
            send(WM_CONTEXTMENU);
    } else {
        send(WM_LBUTTONDOWN);
        send(WM_LBUTTONUP);
        if (version >= 3)
            send(kNinSelect);
    }
    return true;
}

} // namespace atrium
