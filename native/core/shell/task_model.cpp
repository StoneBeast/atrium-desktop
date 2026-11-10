// Task model implementation (D13): EnumWindows + WinEvent hooks on the UI
// thread, AUMID grouping, icons and display names loaded on one worker thread.
//
// Threading: everything except `jobs_`, `icons_` and the ready queues is touched
// by the UI thread only. The worker never touches model state; it publishes
// results under `mutex_` and posts a registered message to the UI window.

#include "shell/task_model.h"
#include "shell/app_processes.h"

#include "log.h"

#include <windows.h>
#include <appmodel.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <propsys.h>
#include <propkey.h>
#include <propvarutil.h>

#include <winrt/base.h>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace atrium {

namespace {

constexpr UINT_PTR kTimerId = 0xA701;     // >= 0xA700: clear of the host's own timers
constexpr UINT kCoalesceMs = 30;
constexpr UINT kWindowIconTimeoutMs = 100; // WM_GETICON on a possibly hung window
constexpr ULONGLONG kRetryFailedIconMs = 10000;

struct HandleCloser {
    HANDLE h = nullptr;
    explicit HandleCloser(HANDLE handle) : h(handle) {}
    HandleCloser(const HandleCloser&) = delete;
    HandleCloser& operator=(const HandleCloser&) = delete;
    ~HandleCloser()
    {
        if (h)
            CloseHandle(h);
    }
};

// COM for the calling thread, balanced on scope exit. A thread that is already
// initialised with another model (RPC_E_CHANGED_MODE) just carries on.
struct ComScope {
    bool owned;
    ComScope() : owned(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {}
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;
    ~ComScope()
    {
        if (owned)
            CoUninitialize();
    }
};

std::wstring Lower(std::wstring s)
{
    if (!s.empty())
        CharLowerBuffW(s.data(), static_cast<DWORD>(s.size()));
    return s;
}

std::wstring HwndKey(HWND h)
{
    wchar_t buf[40];
    swprintf_s(buf, L"hwnd:%llx", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(h)));
    return buf;
}

std::wstring FileStem(const std::wstring& path)
{
    size_t slash = path.find_last_of(L"\\/");
    std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
    size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > 0)
        name.resize(dot);
    return name;
}

bool IsLink(const std::wstring& path)
{
    return path.size() > 4 && _wcsicmp(path.c_str() + path.size() - 4, L".lnk") == 0;
}

std::wstring AppsFolderPath(const std::wstring& aumid)
{
    return L"shell:AppsFolder\\" + aumid;
}

// ---- process and window identity -----------------------------------------

std::wstring ImagePath(HANDLE process)
{
    std::wstring buf(512, L'\0');
    for (;;) {
        DWORD n = static_cast<DWORD>(buf.size());
        if (QueryFullProcessImageNameW(process, 0, buf.data(), &n)) {
            buf.resize(n);
            return buf;
        }
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || buf.size() >= 32768)
            return {};
        buf.resize(buf.size() * 2);
    }
}

// AUMID of a packaged process; empty for classic desktop processes.
std::wstring PackagedAumid(HANDLE process)
{
    wchar_t buf[APPLICATION_USER_MODEL_ID_MAX_LENGTH] = {};
    UINT32 len = ARRAYSIZE(buf);
    if (GetApplicationUserModelId(process, &len, buf) != ERROR_SUCCESS)
        return {};
    return buf;
}

std::wstring StoreAumid(IPropertyStore* store)
{
    PROPVARIANT v;
    PropVariantInit(&v);
    std::wstring result;
    if (SUCCEEDED(store->GetValue(PKEY_AppUserModel_ID, &v)) && v.vt == VT_LPWSTR && v.pwszVal)
        result = v.pwszVal;
    PropVariantClear(&v);
    return result;
}

std::wstring WindowAumid(HWND h)
{
    winrt::com_ptr<IPropertyStore> store;
    if (FAILED(SHGetPropertyStoreForWindow(h, IID_PPV_ARGS(store.put()))) || !store)
        return {};
    return StoreAumid(store.get());
}

// UWP frame windows live in ApplicationFrameHost; the app itself owns a child
// "Windows.UI.Core.CoreWindow" in another process.
std::wstring HostedUwpAumid(HWND frame, DWORD framePid)
{
    struct Ctx {
        DWORD framePid;
        std::wstring aumid;
    } ctx{framePid, {}};
    EnumChildWindows(
        frame,
        [](HWND child, LPARAM lp) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(lp);
            wchar_t cls[64] = {};
            GetClassNameW(child, cls, ARRAYSIZE(cls));
            if (wcscmp(cls, L"Windows.UI.Core.CoreWindow") != 0)
                return TRUE;
            DWORD pid = 0;
            GetWindowThreadProcessId(child, &pid);
            if (!pid || pid == c->framePid)
                return TRUE;
            HandleCloser proc(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
            if (proc.h)
                c->aumid = PackagedAumid(proc.h);
            return c->aumid.empty() ? TRUE : FALSE;
        },
        reinterpret_cast<LPARAM>(&ctx));
    return ctx.aumid;
}

struct WinInfo {
    DWORD pid = 0;
    std::wstring exePath;   // empty if the process could not be opened
    std::wstring aumid;     // original case
    std::wstring key;       // lowercase grouping key
    ULONGLONG created = 0;  // process creation time, orders newly seen groups
};

WinInfo ComputeInfo(HWND h, DWORD pid, const wchar_t* cls)
{
    WinInfo wi;
    wi.pid = pid;
    bool frame = wcscmp(cls, L"ApplicationFrameWindow") == 0;
    HandleCloser proc(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (frame) {
        wi.aumid = HostedUwpAumid(h, pid);
        if (wi.aumid.empty())
            wi.aumid = WindowAumid(h);
        // exePath stays empty: it would be ApplicationFrameHost.exe for every app.
    } else {
        wi.aumid = WindowAumid(h);
        if (proc.h) {
            wi.exePath = ImagePath(proc.h);
            if (wi.aumid.empty())
                wi.aumid = PackagedAumid(proc.h);
        }
    }
    if (proc.h) {
        FILETIME c{}, e{}, k{}, u{};
        if (GetProcessTimes(proc.h, &c, &e, &k, &u))
            wi.created = (static_cast<ULONGLONG>(c.dwHighDateTime) << 32) | c.dwLowDateTime;
    }
    if (!wi.aumid.empty())
        wi.key = Lower(wi.aumid);
    else if (!wi.exePath.empty())
        wi.key = Lower(wi.exePath);
    else
        wi.key = HwndKey(h);
    return wi;
}

// The Windows taskbar's rules for what gets a button.
bool IsAppWindow(HWND h, DWORD& pid, wchar_t* cls, int clsCap, std::wstring& title)
{
    if (!IsWindowVisible(h))
        return false;
    LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    bool appWindow = (ex & WS_EX_APPWINDOW) != 0;
    if ((ex & WS_EX_TOOLWINDOW) && !appWindow)
        return false;
    if (GetWindow(h, GW_OWNER) && !appWindow)
        return false;
    if ((ex & WS_EX_NOACTIVATE) && !appWindow)
        return false;
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked)
        return false;
    pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (!pid || pid == GetCurrentProcessId())
        return false;
    if (!GetClassNameW(h, cls, clsCap))
        return false;
    // Shell windows, plus top-level CoreWindows (Start, Search: shell UI, not apps;
    // real UWP apps appear through their ApplicationFrameWindow).
    for (const wchar_t* shell : {L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd", L"Progman", L"WorkerW", L"Windows.UI.Core.CoreWindow"})
        if (wcscmp(cls, shell) == 0)
            return false;
    // InternalGetWindowText never sends WM_GETTEXT, so a hung window cannot block us.
    wchar_t text[512] = {};
    if (InternalGetWindowText(h, text, ARRAYSIZE(text)) <= 0)
        return false;
    title = text;
    return true;
}

// ---- shortcuts and names -----------------------------------------------------

// Loads a .lnk without resolving it (no disk search, no UI).
bool ReadLink(const std::wstring& path, std::wstring& target, std::wstring& aumid)
{
    winrt::com_ptr<IShellLinkW> link;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(link.put()))))
        return false;
    auto file = link.try_as<IPersistFile>();
    if (!file || FAILED(file->Load(path.c_str(), STGM_READ)))
        return false;
    wchar_t buf[MAX_PATH * 2] = {};
    if (SUCCEEDED(link->GetPath(buf, ARRAYSIZE(buf), nullptr, 0)))
        target = buf;
    if (auto store = link.try_as<IPropertyStore>())
        aumid = StoreAumid(store.get());
    return true;
}

std::wstring FileDescription(const std::wstring& path)
{
    DWORD ignored = 0;
    DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (!size)
        return {};
    std::vector<BYTE> data(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data()))
        return {};
    struct Lang {
        WORD lang, codepage;
    };
    std::vector<Lang> langs;
    Lang* tr = nullptr;
    UINT len = 0;
    if (VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&tr), &len) && tr)
        for (UINT i = 0; i < len / sizeof(Lang); ++i)
            langs.push_back(tr[i]);
    langs.push_back({0x0409, 0x04B0});
    langs.push_back({0x0409, 0x04E4});
    for (const Lang& l : langs) {
        wchar_t sub[80];
        swprintf_s(sub, L"\\StringFileInfo\\%04x%04x\\FileDescription", l.lang, l.codepage);
        wchar_t* value = nullptr;
        UINT n = 0;
        if (VerQueryValueW(data.data(), sub, reinterpret_cast<void**>(&value), &n) && value && n > 1 && value[0])
            return value;
    }
    return {};
}

std::wstring ShellDisplayName(const std::wstring& parsingName)
{
    winrt::com_ptr<IShellItem> item;
    if (FAILED(SHCreateItemFromParsingName(parsingName.c_str(), nullptr, IID_PPV_ARGS(item.put()))))
        return {};
    PWSTR name = nullptr;
    std::wstring result;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &name)) && name)
        result = name;
    CoTaskMemFree(name);
    return result;
}

// ---- pixels -----------------------------------------------------------------

// 32bpp top-down BGRA copy of `bmp`, as the driver stores it (alpha untouched).
bool ReadBits(HBITMAP bmp, int& w, int& h, std::vector<uint8_t>& out)
{
    BITMAP bm{};
    if (!GetObjectW(bmp, sizeof(bm), &bm) || bm.bmWidth <= 0 || bm.bmHeight <= 0)
        return false;
    w = bm.bmWidth;
    h = bm.bmHeight;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h; // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    out.assign(size_t(w) * size_t(h) * 4, 0);
    HDC dc = GetDC(nullptr);
    int lines = GetDIBits(dc, bmp, 0, static_cast<UINT>(h), out.data(), &bi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, dc);
    return lines == h;
}

// Makes `px` premultiplied. A bitmap without any alpha is opaque. Where alpha
// exists, a colour channel above alpha cannot be premultiplied data, so the
// bitmap holds straight alpha and gets premultiplied here.
void MakePremultiplied(std::vector<uint8_t>& px)
{
    bool anyAlpha = false, straight = false;
    for (size_t i = 0; i < px.size(); i += 4) {
        uint8_t a = px[i + 3];
        anyAlpha |= a != 0;
        straight |= px[i] > a || px[i + 1] > a || px[i + 2] > a;
    }
    if (!anyAlpha) {
        for (size_t i = 3; i < px.size(); i += 4)
            px[i] = 255;
    } else if (straight) {
        for (size_t i = 0; i < px.size(); i += 4) {
            unsigned a = px[i + 3];
            for (size_t c = 0; c < 3; ++c)
                px[i + c] = static_cast<uint8_t>((px[i + c] * a + 127) / 255);
        }
    }
}

bool BitmapToImage(HBITMAP bmp, IconImage& out)
{
    if (!ReadBits(bmp, out.width, out.height, out.bgra))
        return false;
    MakePremultiplied(out.bgra);
    return true;
}

bool IconToImage(HICON icon, IconImage& out)
{
    ICONINFO ii{};
    if (!GetIconInfo(icon, &ii))
        return false;
    bool ok = false;
    if (ii.hbmColor) { // monochrome icons (no colour bitmap) are not worth supporting
        std::vector<uint8_t> color;
        int w = 0, h = 0;
        if (ReadBits(ii.hbmColor, w, h, color)) {
            bool hasAlpha = false;
            for (size_t i = 3; i < color.size() && !hasAlpha; i += 4)
                hasAlpha = color[i] != 0;
            if (hasAlpha) {
                MakePremultiplied(color);
            } else {
                // Old-style icon: transparency is in the AND mask (black = opaque).
                std::vector<uint8_t> mask;
                int mw = 0, mh = 0;
                bool haveMask = ii.hbmMask && ReadBits(ii.hbmMask, mw, mh, mask) && mw == w && mh == h;
                for (size_t i = 0; i < color.size(); i += 4) {
                    bool opaque = !haveMask || mask[i] == 0;
                    color[i + 3] = opaque ? 255 : 0;
                    if (!opaque)
                        color[i] = color[i + 1] = color[i + 2] = 0;
                }
            }
            out.width = w;
            out.height = h;
            out.bgra = std::move(color);
            ok = true;
        }
    }
    if (ii.hbmColor)
        DeleteObject(ii.hbmColor);
    if (ii.hbmMask)
        DeleteObject(ii.hbmMask);
    return ok;
}

// Scales a premultiplied image into a size x size square (aspect kept, centred).
IconImage FitToSquare(const IconImage& src, int size)
{
    if (src.width == size && src.height == size)
        return src;
    IconImage out;
    out.width = out.height = size;
    out.bgra.assign(size_t(size) * size_t(size) * 4, 0);
    double scale = double(size) / double(std::max(src.width, src.height));
    int dw = std::max(1, int(std::lround(src.width * scale)));
    int dh = std::max(1, int(std::lround(src.height * scale)));
    int ox = (size - dw) / 2, oy = (size - dh) / 2;
    auto at = [&](int x, int y, int c) {
        x = std::clamp(x, 0, src.width - 1);
        y = std::clamp(y, 0, src.height - 1);
        return double(src.bgra[(size_t(y) * size_t(src.width) + size_t(x)) * 4 + size_t(c)]);
    };
    for (int dy = 0; dy < dh; ++dy) {
        double y0 = double(dy) * src.height / dh, y1 = double(dy + 1) * src.height / dh;
        for (int dx = 0; dx < dw; ++dx) {
            double x0 = double(dx) * src.width / dw, x1 = double(dx + 1) * src.width / dw;
            uint8_t* d = &out.bgra[(size_t(dy + oy) * size_t(size) + size_t(dx + ox)) * 4];
            for (int c = 0; c < 4; ++c) {
                double v = 0;
                if (x1 - x0 <= 1.0 && y1 - y0 <= 1.0) {
                    // Upscale: bilinear around the pixel centre.
                    double fx = (x0 + x1) / 2 - 0.5, fy = (y0 + y1) / 2 - 0.5;
                    int ix = int(std::floor(fx)), iy = int(std::floor(fy));
                    double tx = fx - ix, ty = fy - iy;
                    v = at(ix, iy, c) * (1 - tx) * (1 - ty) + at(ix + 1, iy, c) * tx * (1 - ty) + at(ix, iy + 1, c) * (1 - tx) * ty +
                        at(ix + 1, iy + 1, c) * tx * ty;
                } else {
                    // Downscale: area average with fractional edge weights.
                    double sum = 0, weight = 0;
                    for (int sy = int(std::floor(y0)); sy < int(std::ceil(y1)); ++sy) {
                        double wy = std::min<double>(sy + 1, y1) - std::max<double>(sy, y0);
                        for (int sx = int(std::floor(x0)); sx < int(std::ceil(x1)); ++sx) {
                            double wx = std::min<double>(sx + 1, x1) - std::max<double>(sx, x0);
                            sum += at(sx, sy, c) * wx * wy;
                            weight += wx * wy;
                        }
                    }
                    v = weight > 0 ? sum / weight : 0;
                }
                d[c] = static_cast<uint8_t>(std::clamp(int(std::lround(v)), 0, 255));
            }
        }
    }
    return out;
}

// IShellItemImageFactory on a parsing name (path, .lnk or shell:AppsFolder\...).
bool ShellImage(const std::wstring& parsingName, int size, IconImage& out, bool thumbnail = false)
{
    winrt::com_ptr<IShellItemImageFactory> factory;
    if (FAILED(SHCreateItemFromParsingName(parsingName.c_str(), nullptr, IID_PPV_ARGS(factory.put()))) || !factory)
        return false;
    const SIIGBF icons[] = {SIIGBF_ICONONLY, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, SIIGBF_BIGGERSIZEOK};
    const SIIGBF pictures[] = {SIIGBF_THUMBNAILONLY | SIIGBF_BIGGERSIZEOK, SIIGBF_RESIZETOFIT, SIIGBF_BIGGERSIZEOK};
    for (SIIGBF flags : thumbnail ? pictures : icons) {
        HBITMAP bmp = nullptr;
        if (SUCCEEDED(factory->GetImage(SIZE{size, size}, flags, &bmp)) && bmp) {
            IconImage raw;
            bool ok = BitmapToImage(bmp, raw);
            DeleteObject(bmp);
            if (ok) {
                out = FitToSquare(raw, size);
                return true;
            }
        }
    }
    return false;
}

// The window's own icon. WM_GETICON goes through SendMessageTimeout so a hung
// window costs at most 100 ms; the HICON belongs to the app, never destroyed.
bool WindowIcon(HWND h, int size, IconImage& out)
{
    if (!IsWindow(h) || IsHungAppWindow(h))
        return false;
    const WPARAM order[] = {size > 24 ? WPARAM(ICON_BIG) : WPARAM(ICON_SMALL), WPARAM(ICON_SMALL2), WPARAM(ICON_BIG), WPARAM(ICON_SMALL)};
    for (WPARAM type : order) {
        DWORD_PTR result = 0;
        if (!SendMessageTimeoutW(h, WM_GETICON, type, 0, SMTO_ABORTIFHUNG, kWindowIconTimeoutMs, &result) || !result)
            continue;
        if (IconToImage(reinterpret_cast<HICON>(result), out))
            return true;
    }
    for (int index : {GCLP_HICON, GCLP_HICONSM}) {
        HICON icon = reinterpret_cast<HICON>(GetClassLongPtrW(h, index));
        if (icon && IconToImage(icon, out))
            return true;
    }
    return false;
}

// ---- pins ---------------------------------------------------------------------

struct Pin {
    PinnedApp app;
    std::wstring key;    // lowercase
    std::wstring target; // exe the pin starts (lowercase), used for loose matching; may be empty
    std::wstring exe;    // same, original case
    std::wstring aumid;  // also resolved from the shortcut
    bool multipleWindows = false;
};

Pin ResolvePin(const PinnedApp& app)
{
    Pin p;
    p.app = app;
    std::wstring aumid = app.aumid;
    std::wstring target;
    if (!app.path.empty()) {
        if (IsLink(app.path)) {
            std::wstring linkAumid;
            if (ReadLink(app.path, target, linkAumid) && aumid.empty())
                aumid = linkAumid; // the link's own AUMID wins over the path
        } else {
            target = app.path;
        }
    }
    p.exe = AppExecutable(target);
    p.aumid = aumid;
    PinnedApp resolved = app;
    resolved.path = p.exe;
    resolved.aumid = aumid;
    p.multipleWindows = resolved.AllowsMultipleWindows();
    p.target = Lower(target);
    if (!aumid.empty()) {
        p.key = Lower(aumid);
    } else if (!target.empty()) {
        p.key = p.target;
    } else {
        p.key = Lower(app.path);
    }
    if (p.key.empty())
        p.key = Lower(app.name);
    return p;
}

// ---- worker jobs ----------------------------------------------------------------

struct Job {
    bool name = false; // false: icon
    std::wstring key;
    int size = 0;
    std::wstring sig;
    std::wstring aumid, pinPath, exePath;
    std::vector<HWND> hwnds;
};

struct IconEntry {
    bool requested = false, loading = false, have = false;
    std::wstring sig;
    ULONGLONG failedAt = 0;
    uint64_t used = 0;
    IconImage image;
};

// What the model knows about one item that icon and name lookups need.
struct Meta {
    std::wstring aumid, pinPath, exePath;
    std::vector<HWND> hwnds;
    std::vector<AppProcessIdentity> processTargets; // retained for pinned apps closed to the tray
    std::wstring Signature() const { return aumid + L"|" + pinPath + L"|" + exePath; }
};

struct IconKey {
    std::wstring key;
    int size;
    bool operator==(const IconKey&) const = default;
};
struct IconKeyHash {
    size_t operator()(const IconKey& k) const { return std::hash<std::wstring>()(k.key) * 31 + size_t(k.size); }
};

bool Same(const TaskItem& a, const TaskItem& b)
{
    return a.key == b.key && a.name == b.name && a.pinned == b.pinned && a.pinIndex == b.pinIndex && a.windows == b.windows &&
           a.active == b.active && a.attention == b.attention && a.iconVersion == b.iconVersion;
}

struct ScanRec {
    HWND hwnd = nullptr;
    int z = 0; // EnumWindows order, top first
    std::wstring title;
    WinInfo info;
};

} // namespace

struct TaskModel::Impl {
    HWND ui;
    std::function<void()> onChanged;
    std::function<bool(const std::wstring&)> openExisting;
    ComScope com;
    UINT shellHookMsg = 0;
    UINT readyMsg = 0;
    std::vector<HWINEVENTHOOK> hooks;
    bool shellHooked = false;
    bool timerArmed = false, dirty = true, notify = false;
    bool showRunning = true;

    // UI-thread state.
    std::vector<Pin> pins;
    std::unordered_map<std::wstring, size_t> pinByKey, pinByTarget;
    std::unordered_map<HWND, WinInfo> infos;      // identity cache per window
    std::unordered_map<HWND, uint64_t> lastActive; // activation sequence number
    std::unordered_set<HWND> attention;
    uint64_t seq = 0;
    HWND lastFg = nullptr;
    std::vector<ScanRec> scan;
    std::vector<std::wstring> order;               // unpinned groups, first seen first
    std::vector<TaskItem> items;                   // pins + running groups (not filtered by showRunning)
    std::unordered_map<std::wstring, Meta> meta;
    std::unordered_map<std::wstring, std::wstring> names;
    std::unordered_set<std::wstring> namesRequested;
    std::unordered_map<std::wstring, uint64_t> iconVersion;

    // Shared with the worker.
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<Job> jobs;
    std::unordered_map<IconKey, IconEntry, IconKeyHash> icons;
    std::unordered_set<std::wstring> liveIconKeys;
    std::unordered_map<std::wstring, int> latestIconSize;
    uint64_t iconUse = 0;
    std::vector<std::wstring> readyIcons;
    std::vector<std::pair<std::wstring, std::wstring>> readyNames;
    bool stop = false;
    std::thread worker;

    static Impl* self;

    Impl(HWND window, std::function<void()> changed, std::function<bool(const std::wstring&)> opener)
        : ui(window), onChanged(std::move(changed)), openExisting(std::move(opener))
    {
        self = this;
        shellHookMsg = RegisterWindowMessageW(L"SHELLHOOK");
        readyMsg = RegisterWindowMessageW(L"Atrium.TaskModel.Ready");
        worker = std::thread([this] { WorkerMain(); });

        struct Range {
            DWORD lo, hi;
        };
        const Range ranges[] = {{EVENT_OBJECT_CREATE, EVENT_OBJECT_HIDE},
                                {EVENT_OBJECT_NAMECHANGE, EVENT_OBJECT_NAMECHANGE},
                                {EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED},
                                {EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND},
                                {EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND}};
        for (const Range& r : ranges) {
            HWINEVENTHOOK h = SetWinEventHook(r.lo, r.hi, nullptr, &Impl::WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
            if (h)
                hooks.push_back(h);
            else
                Log(L"tasks: SetWinEventHook(0x%X-0x%X) failed (%lu)", r.lo, r.hi, GetLastError());
        }
        shellHooked = RegisterShellHookWindow(ui) != FALSE;
        if (!shellHooked)
            Log(L"tasks: RegisterShellHookWindow failed (%lu); no flash notifications", GetLastError());

        Rescan();
        Rebuild();
        ArmTimer(); // catches events that raced the initial scan
    }

    ~Impl()
    {
        for (HWINEVENTHOOK h : hooks)
            UnhookWinEvent(h);
        if (shellHooked)
            DeregisterShellHookWindow(ui);
        if (timerArmed)
            KillTimer(ui, kTimerId);
        {
            std::lock_guard g(mutex);
            stop = true;
        }
        cv.notify_all();
        if (worker.joinable())
            worker.join();
        self = nullptr;
    }

    // ---- events -------------------------------------------------------------

    static void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD, DWORD)
    {
        if (!self || idObject != OBJID_WINDOW || idChild != CHILDID_SELF || !hwnd)
            return;
        try {
            self->OnEvent(event, hwnd);
        } catch (...) {
            Log(L"tasks: exception in WinEvent handler");
        }
    }

    void OnEvent(DWORD event, HWND hwnd)
    {
        if (event == EVENT_SYSTEM_FOREGROUND) {
            NoteForeground(hwnd);
        } else if (event == EVENT_OBJECT_DESTROY) {
            if (!infos.contains(hwnd))
                return; // not something we show
        } else if (GetAncestor(hwnd, GA_PARENT) != GetDesktopWindow()) {
            return; // child window
        }
        if (event == EVENT_OBJECT_SHOW || event == EVENT_OBJECT_UNCLOAKED || event == EVENT_OBJECT_CREATE)
            infos.erase(hwnd); // identity (AUMID) may have been set meanwhile
        MarkDirty();
    }

    void NoteForeground(HWND fg)
    {
        if (!fg)
            return;
        HWND root = GetAncestor(fg, GA_ROOTOWNER);
        lastActive[fg] = ++seq;
        attention.erase(fg);
        if (root && root != fg) {
            lastActive[root] = ++seq;
            attention.erase(root);
        }
        lastFg = root ? root : fg;
    }

    void MarkDirty()
    {
        dirty = true;
        ArmTimer();
    }

    void ArmTimer()
    {
        if (!timerArmed && SetTimer(ui, kTimerId, kCoalesceMs, nullptr))
            timerArmed = true;
    }

    bool HandleMessage(UINT msg, WPARAM wp, LPARAM lp)
    {
        try {
            if (msg == WM_TIMER && wp == kTimerId) {
                KillTimer(ui, kTimerId);
                timerArmed = false;
                Tick();
                return true;
            }
            if (shellHookMsg && msg == shellHookMsg) {
                OnShellHook(wp, reinterpret_cast<HWND>(lp));
                return true;
            }
            if (readyMsg && msg == readyMsg) {
                OnReady();
                return true;
            }
        } catch (const std::exception& e) {
            Log(L"tasks: exception in HandleMessage: %S", e.what());
        } catch (...) {
            Log(L"tasks: unknown exception in HandleMessage");
        }
        return false;
    }

    void OnShellHook(WPARAM code, HWND hwnd)
    {
        if (code == HSHELL_FLASH && hwnd) {
            HWND root = GetAncestor(hwnd, GA_ROOTOWNER);
            if (!root)
                root = hwnd;
            HWND fg = GetForegroundWindow();
            if (root != fg && root != GetAncestor(fg, GA_ROOTOWNER))
                attention.insert(root);
        }
        MarkDirty(); // window created/destroyed/activated/redrawn
    }

    void OnReady()
    {
        std::vector<std::wstring> icons_;
        std::vector<std::pair<std::wstring, std::wstring>> names_;
        {
            std::lock_guard g(mutex);
            icons_.swap(readyIcons);
            names_.swap(readyNames);
        }
        for (auto& key : icons_)
            if (meta.contains(key)) ++iconVersion[key];
        for (auto& [key, name] : names_)
            if (meta.contains(key)) names[key] = name;
        if (Rebuild()) {
            notify = true;
            ArmTimer();
        }
    }

    void Tick()
    {
        bool changed = false;
        if (dirty) {
            dirty = false;
            Rescan();
            changed = Rebuild();
        }
        if (changed || notify) {
            notify = false;
            if (onChanged)
                onChanged();
        }
    }

    // ---- model --------------------------------------------------------------

    HWND ForegroundRoot() const
    {
        HWND fg = GetForegroundWindow();
        if (!fg)
            return nullptr;
        HWND root = GetAncestor(fg, GA_ROOTOWNER);
        return root ? root : fg;
    }

    static BOOL CALLBACK CollectWindow(HWND h, LPARAM lp)
    {
        reinterpret_cast<std::vector<HWND>*>(lp)->push_back(h);
        return TRUE;
    }

    void Rescan()
    {
        std::vector<HWND> all;
        EnumWindows(&Impl::CollectWindow, reinterpret_cast<LPARAM>(&all));
        scan.clear();
        std::unordered_set<HWND> seen;
        int z = 0;
        for (HWND h : all) {
            ++z;
            DWORD pid = 0;
            wchar_t cls[256] = {};
            std::wstring title;
            if (!IsAppWindow(h, pid, cls, ARRAYSIZE(cls), title))
                continue;
            auto it = infos.find(h);
            if (it == infos.end() || it->second.pid != pid)
                it = infos.insert_or_assign(h, ComputeInfo(h, pid, cls)).first;
            seen.insert(h);
            scan.push_back(ScanRec{h, z, std::move(title), it->second});
        }
        std::erase_if(infos, [&](const auto& kv) { return !seen.contains(kv.first); });
        std::erase_if(lastActive, [](const auto& kv) { return !IsWindow(kv.first); });
        std::erase_if(attention, [](HWND h) { return !IsWindow(h); });
        // Foreground changes normally arrive as events; this covers a missed one.
        HWND fg = ForegroundRoot();
        if (fg && fg != lastFg)
            NoteForeground(fg);
    }

    // Pin that a window with this identity belongs to, or -1.
    int MatchPin(const WinInfo& info) const
    {
        auto it = pinByKey.find(info.key);
        if (it != pinByKey.end())
            return int(it->second);
        // Desktop launchers can give the actual window a different identity or executable.
        // Exact AUMIDs still win; an executable shared by several pins is ambiguous.
        if (!info.exePath.empty()) {
            auto jt = pinByTarget.find(Lower(info.exePath));
            if (jt != pinByTarget.end() && jt->second < pins.size())
                return int(jt->second);
        }
        return -1;
    }

    // Recomputes `items`/`meta` from `scan` and the pins; true if `items` changed.
    bool Rebuild()
    {
        std::vector<TaskItem> out;
        std::unordered_map<std::wstring, Meta> newMeta;
        std::unordered_map<std::wstring, size_t> at;
        std::unordered_map<std::wstring, ULONGLONG> firstSeen; // for ordering new groups
        std::unordered_map<std::wstring, std::wstring> firstTitle;

        for (size_t i = 0; i < pins.size(); ++i) {
            const Pin& p = pins[i];
            TaskItem t;
            t.key = p.key;
            t.name = p.app.name;
            t.pinned = true;
            t.pinIndex = int(i);
            t.multipleWindows = p.multipleWindows;
            at[p.key] = out.size();
            out.push_back(std::move(t));
            Meta& m = newMeta[p.key];
            m.aumid = p.aumid;
            m.pinPath = p.app.path;
            m.exePath = p.exe;
        }
        size_t pinCount = out.size();

        // Most recently active first; never-activated windows keep z-order.
        std::vector<const ScanRec*> sorted;
        for (const ScanRec& r : scan)
            sorted.push_back(&r);
        auto activity = [&](HWND h) {
            auto it = lastActive.find(h);
            return it == lastActive.end() ? uint64_t(0) : it->second;
        };
        std::stable_sort(sorted.begin(), sorted.end(), [&](const ScanRec* a, const ScanRec* b) {
            uint64_t x = activity(a->hwnd), y = activity(b->hwnd);
            return x != y ? x > y : a->z < b->z;
        });

        HWND fg = ForegroundRoot();
        std::unordered_set<std::wstring> running;
        for (const ScanRec* r : sorted) {
            int pin = MatchPin(r->info);
            const std::wstring& key = pin >= 0 ? pins[size_t(pin)].key : r->info.key;
            auto it = at.find(key);
            if (it == at.end()) {
                TaskItem t;
                t.key = key;
                it = at.emplace(key, out.size()).first;
                out.push_back(std::move(t));
                firstTitle[key] = r->title;
            }
            TaskItem& t = out[it->second];
            t.windows.push_back(r->hwnd);
            if (fg && (r->hwnd == fg || GetAncestor(r->hwnd, GA_ROOTOWNER) == fg))
                t.active = true;
            if (attention.contains(r->hwnd))
                t.attention = true;
            Meta& m = newMeta[key];
            if (m.exePath.empty())
                m.exePath = r->info.exePath;
            if (m.aumid.empty())
                m.aumid = r->info.aumid;
            m.hwnds.push_back(r->hwnd);
            if (std::none_of(m.processTargets.begin(), m.processTargets.end(), [&](const AppProcessIdentity& target) {
                    return target.exePath == r->info.exePath && target.aumid == r->info.aumid;
                }))
                m.processTargets.push_back({r->info.exePath, r->info.aumid});
            if (it->second >= pinCount) {
                running.insert(key);
                auto f = firstSeen.find(key);
                if (f == firstSeen.end() || (r->info.created && r->info.created < f->second))
                    firstSeen[key] = r->info.created;
            }
        }

        // A shortcut may point to a launcher; retain the actual app identity after it hides.
        for (auto& [key, value] : newMeta) {
            const auto previous = meta.find(key);
            if (value.processTargets.empty() && previous != meta.end())
                value.processTargets = previous->second.processTargets;
        }

        // Unpinned groups: keep first-seen order, append new ones by process start.
        std::erase_if(order, [&](const std::wstring& k) { return !running.contains(k); });
        std::vector<std::wstring> fresh;
        for (const auto& k : running)
            if (std::find(order.begin(), order.end(), k) == order.end())
                fresh.push_back(k);
        std::sort(fresh.begin(), fresh.end(), [&](const std::wstring& a, const std::wstring& b) {
            return firstSeen[a] != firstSeen[b] ? firstSeen[a] < firstSeen[b] : a < b;
        });
        order.insert(order.end(), fresh.begin(), fresh.end());

        std::vector<TaskItem> result(out.begin(), out.begin() + std::ptrdiff_t(pinCount));
        for (const auto& k : order)
            result.push_back(std::move(out[at[k]]));

        for (TaskItem& t : result) {
            const Meta& m = newMeta[t.key];
            if (!t.pinned)
                t.multipleWindows = PinnedApp{t.name, m.exePath, {}, m.aumid}.AllowsMultipleWindows();
            if (t.name.empty()) {
                auto n = names.find(t.key);
                if (n != names.end()) {
                    t.name = n->second;
                } else {
                    // Provisional until the worker has the shell display name.
                    t.name = !m.exePath.empty() ? FileStem(m.exePath) : !m.pinPath.empty() ? FileStem(m.pinPath) : firstTitle[t.key];
                    if (t.name.empty())
                        t.name = m.aumid;
                }
            }
            auto v = iconVersion.find(t.key);
            t.iconVersion = v == iconVersion.end() ? 0 : v->second;
            bool pinNamed = t.pinned && !pins[size_t(t.pinIndex)].app.name.empty();
            if (!pinNamed && !names.contains(t.key) && (!m.aumid.empty() || !m.exePath.empty()) && namesRequested.insert(t.key).second) {
                Job j;
                j.name = true;
                j.key = t.key;
                j.aumid = m.aumid;
                j.exePath = m.exePath;
                j.pinPath = m.pinPath;
                Enqueue(std::move(j));
            }
        }
        meta = std::move(newMeta);

        bool changed = result.size() != items.size();
        for (size_t i = 0; !changed && i < result.size(); ++i)
            changed = !Same(result[i], items[i]);
        items = std::move(result);
        std::erase_if(names, [&](const auto& entry) { return !meta.contains(entry.first); });
        std::erase_if(namesRequested, [&](const auto& key) { return !meta.contains(key); });
        std::erase_if(iconVersion, [&](const auto& entry) { return !meta.contains(entry.first); });
        {
            std::lock_guard g(mutex);
            liveIconKeys.clear();
            for (const auto& [key, value] : meta) liveIconKeys.insert(key);
            std::erase_if(latestIconSize, [&](const auto& entry) { return !liveIconKeys.contains(entry.first); });
            PruneIcons();
        }
        return changed;
    }

    const TaskItem* Find(const std::wstring& key) const
    {
        for (const TaskItem& t : items)
            if (t.key == key)
                return &t;
        return nullptr;
    }

    std::vector<TaskItem> Snapshot(bool all) const
    {
        if (showRunning || all)
            return items;
        std::vector<TaskItem> out;
        for (const TaskItem& t : items)
            if (t.pinned)
                out.push_back(t);
        return out;
    }

    void SetPins(const std::vector<PinnedApp>& list)
    {
        bool same = list.size() == pins.size();
        for (size_t i = 0; same && i < list.size(); ++i)
            same = list[i] == pins[i].app;
        if (same)
            return;
        pins.clear();
        pinByKey.clear();
        pinByTarget.clear();
        for (const PinnedApp& app : list) {
            Pin p = ResolvePin(app);
            if (pinByKey.contains(p.key))
                p.key += L"#" + std::to_wstring(pins.size()); // duplicate pin: shown, never matched
            else
                pinByKey[p.key] = pins.size();
            auto addTarget = [&](const std::wstring& target) {
                if (target.empty()) return;
                auto [it, added] = pinByTarget.emplace(target, pins.size());
                if (!added && it->second != pins.size()) it->second = size_t(-1);
            };
            // Browser profile shortcuts keep their explicit identity.
            std::wstring id = Lower(p.aumid);
            if (!(id.starts_with(L"chrome.") || id.starts_with(L"msedge."))) {
                addTarget(p.target);
                addTarget(Lower(p.exe));
            }
            pins.push_back(std::move(p));
        }
        if (Rebuild()) {
            notify = true;
            ArmTimer();
        }
    }

    void SetShowRunning(bool show)
    {
        if (show == showRunning)
            return;
        showRunning = show;
        notify = true;
        ArmTimer();
    }

    // ---- icons --------------------------------------------------------------

    // Under mutex. Keep the latest size of every live/pinned item and every
    // in-flight lookup. Historical sizes share a byte/count budget.
    void PruneIcons()
    {
        std::erase_if(icons, [&](const auto& entry) { return !entry.second.loading && !liveIconKeys.contains(entry.first.key); });
        size_t bytes = 0;
        for (const auto& [key, entry] : icons) bytes += entry.image.bgra.size();
        while (bytes > 16 * 1024 * 1024 || icons.size() > 512) {
            auto victim = icons.end();
            for (auto it = icons.begin(); it != icons.end(); ++it) {
                const auto current = latestIconSize.find(it->first.key);
                if (it->second.loading || (current != latestIconSize.end() && current->second == it->first.size)) continue;
                if (victim == icons.end() || it->second.used < victim->second.used) victim = it;
            }
            if (victim == icons.end()) break; // protected live working set only
            bytes -= victim->second.image.bgra.size();
            icons.erase(victim);
        }
    }

    void Enqueue(Job job)
    {
        {
            std::lock_guard g(mutex);
            jobs.push_back(std::move(job));
        }
        cv.notify_one();
    }

    IconImage Icon(const std::wstring& key, int px)
    {
        px = std::clamp(px, 8, 512);
        auto mit = meta.find(key);
        if (mit == meta.end())
            return {};
        const Meta& m = mit->second;
        std::wstring sig = m.Signature();

        std::unique_lock g(mutex);
        latestIconSize[key] = px;
        PruneIcons();
        IconEntry& e = icons[IconKey{key, px}];
        e.used = ++iconUse;
        bool want = !e.requested;
        if (e.requested && !e.loading && e.sig != sig)
            want = true; // identity changed (pinned, relaunched from elsewhere): refresh, keep showing the old one
        if (e.requested && !e.loading && !e.have && e.sig == sig && GetTickCount64() - e.failedAt > kRetryFailedIconMs)
            want = true;
        if (want) {
            e.requested = e.loading = true;
            e.sig = sig;
            Job j;
            j.key = key;
            j.size = px;
            j.sig = sig;
            j.aumid = m.aumid;
            j.pinPath = m.pinPath;
            j.exePath = m.exePath;
            j.hwnds = m.hwnds;
            jobs.push_back(std::move(j));
            cv.notify_one();
        }
        return e.have ? e.image : IconImage{};
    }

    void WorkerMain()
    {
        ComScope workerCom; // STA is fine for IShellItemImageFactory
        for (;;) {
            Job job;
            {
                std::unique_lock g(mutex);
                cv.wait(g, [&] { return stop || !jobs.empty(); });
                if (stop)
                    return;
                job = std::move(jobs.front());
                jobs.pop_front();
                if (!liveIconKeys.contains(job.key)) {
                    if (!job.name) icons.erase(IconKey{job.key, job.size});
                    continue;
                }
            }
            try {
                if (job.name)
                    RunNameJob(job);
                else
                    RunIconJob(job);
            } catch (...) {
                Log(L"tasks: icon worker exception for %s", job.key.c_str());
            }
        }
    }

    void RunNameJob(const Job& job)
    {
        std::wstring name;
        if (!job.aumid.empty())
            name = ShellDisplayName(AppsFolderPath(job.aumid));
        if (name.empty() && !job.exePath.empty())
            name = FileDescription(job.exePath);
        if (name.empty() && !job.pinPath.empty() && IsLink(job.pinPath))
            name = FileStem(job.pinPath);
        if (name.empty() && !job.exePath.empty())
            name = FileStem(job.exePath);
        if (name.empty())
            return;
        {
            std::lock_guard g(mutex);
            readyNames.emplace_back(job.key, std::move(name));
        }
        PostMessageW(ui, readyMsg, 0, 0);
    }

    void RunIconJob(const Job& job)
    {
        std::vector<std::wstring> sources;
        auto add = [&](std::wstring s) {
            if (!s.empty() && std::find(sources.begin(), sources.end(), s) == sources.end())
                sources.push_back(std::move(s));
        };
        add(job.pinPath); // a pin's shortcut carries the icon the user sees in Windows
        if (!job.aumid.empty())
            add(AppsFolderPath(job.aumid));
        add(job.exePath);

        IconImage img;
        bool ok = false;
        LARGE_INTEGER t0, t1, f;
        QueryPerformanceCounter(&t0);
        ok = LoadAtriumIcon(job.exePath, job.size, img);
        for (size_t i = 0; !ok && i < sources.size(); ++i)
            ok = LoadShellImage(sources[i], job.size, img);
        for (size_t i = 0; !ok && i < job.hwnds.size(); ++i) {
            IconImage raw;
            if (WindowIcon(job.hwnds[i], job.size, raw)) {
                img = FitToSquare(raw, job.size);
                ok = true;
            }
        }
        QueryPerformanceCounter(&t1);
        QueryPerformanceFrequency(&f);
        if (!ok)
            Log(L"tasks: no icon for %s (%.1f ms)", job.key.c_str(), double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(f.QuadPart));

        bool changed = false;
        {
            std::lock_guard g(mutex);
            auto found = icons.find(IconKey{job.key, job.size});
            if (found == icons.end()) return;
            if (!liveIconKeys.contains(job.key)) { icons.erase(found); return; }
            IconEntry& e = found->second;
            if (e.sig != job.sig) return;
            e.loading = false;
            if (ok) {
                changed = !e.have || e.image.bgra != img.bgra;
                e.image = std::move(img);
                e.have = true;
            } else {
                e.failedAt = GetTickCount64();
            }
            if (changed)
                readyIcons.push_back(job.key);
            PruneIcons();
        }
        if (changed)
            PostMessageW(ui, readyMsg, 0, 0);
    }

    // ---- actions --------------------------------------------------------------

    static bool Exec(const std::wstring& file, const std::wstring& args, const std::wstring& dir)
    {
        SHELLEXECUTEINFOW sei{sizeof(sei)};
        sei.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
        sei.lpFile = file.c_str();
        sei.lpParameters = args.empty() ? nullptr : args.c_str();
        sei.lpDirectory = dir.empty() ? nullptr : dir.c_str();
        sei.nShow = SW_SHOWNORMAL;
        if (!ShellExecuteExW(&sei)) {
            Log(L"tasks: launch of %s failed (%lu)", file.c_str(), GetLastError());
            return false;
        }
        return true;
    }

    void Launch(const std::wstring& key)
    {
        const TaskItem* task = Find(key);
        if (task && !task->multipleWindows) {
            for (HWND window : task->windows)
                if (IsWindowVisible(window)) {
                    BringToFront(window);
                    return;
                }
            auto m = meta.find(key);
            if (m != meta.end() && openExisting && openExisting(m->second.exePath))
                return;
        }
        auto pit = pinByKey.find(key);
        size_t pinIndex = pit != pinByKey.end() ? pit->second : pins.size();
        if (pinIndex == pins.size()) // duplicate pins carry a suffixed key
            for (size_t i = 0; i < pins.size(); ++i)
                if (pins[i].key == key)
                    pinIndex = i;
        if (pinIndex < pins.size()) {
            const PinnedApp& app = pins[pinIndex].app;
            if (!app.path.empty()) {
                std::wstring dir;
                if (!IsLink(app.path)) {
                    size_t slash = app.path.find_last_of(L"\\/");
                    if (slash != std::wstring::npos)
                        dir = app.path.substr(0, slash);
                }
                Exec(app.path, IsLink(app.path) ? std::wstring() : app.args, dir);
            } else if (!app.aumid.empty()) {
                Exec(AppsFolderPath(app.aumid), {}, {});
            } else {
                Log(L"tasks: pin %s has nothing to launch", app.name.c_str());
            }
            return;
        }
        auto mit = meta.find(key);
        if (mit == meta.end()) {
            Log(L"tasks: launch of unknown item %s", key.c_str());
            return;
        }
        const Meta& m = mit->second;
        bool packaged = Lower(m.exePath).find(L"\\windowsapps\\") != std::wstring::npos;
        if (!m.exePath.empty() && !packaged)
            Exec(m.exePath, {}, {});
        else if (!m.aumid.empty())
            Exec(AppsFolderPath(m.aumid), {}, {});
        else
            Log(L"tasks: no way to start %s", key.c_str());
    }

    static void SysCommand(HWND h, WPARAM command)
    {
        DWORD_PTR ignored = 0;
        if (IsHungAppWindow(h) || !SendMessageTimeoutW(h, WM_SYSCOMMAND, command, 0, SMTO_ABORTIFHUNG, 500, &ignored))
            ShowWindowAsync(h, command == SC_RESTORE ? SW_RESTORE : SW_MINIMIZE);
    }

    static void BringToFront(HWND h)
    {
        // A closed-to-tray window must be restored by its app, not by the task switcher.
        if (!IsWindowVisible(h)) return;
        if (IsIconic(h))
            SysCommand(h, SC_RESTORE);
        HWND target = GetLastActivePopup(h);
        if (!target || target == h || !IsWindowVisible(target))
            target = h;
        auto isFront = [&] {
            HWND fg = GetForegroundWindow();
            return fg == target || fg == h || (fg && GetAncestor(fg, GA_ROOTOWNER) == h);
        };
        if (SetForegroundWindow(target) && isFront())
            return;
        // Foreground lock refused: the bar never takes activation itself, so on
        // Windows 11 this is the normal case. SwitchToThisWindow
        // is what task switchers use; unlike a synthetic Alt press it sends no
        // input to the user's app.
        static bool logged = false;
        if (!logged) {
            Log(L"tasks: SetForegroundWindow refused, switching with SwitchToThisWindow (logged once)");
            logged = true;
        }
        if (IsWindowVisible(target))
            SwitchToThisWindow(target, TRUE);
    }

    void Activate(const std::wstring& key, HMONITOR monitor)
    {
        const TaskItem* t = Find(key);
        auto windows = t ? t->windows : std::vector<HWND>{};
        std::erase_if(windows, [](HWND h) { return !IsWindowVisible(h); });
        if (monitor)
            std::erase_if(windows, [&](HWND h) { return MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST) != monitor; });
        if (windows.empty()) {
            Launch(key);
            return;
        }
        HWND fg = ForegroundRoot();
        bool inGroup = fg && std::find(windows.begin(), windows.end(), fg) != windows.end();
        if (windows.size() == 1) {
            HWND w = windows[0];
            if (inGroup && !IsIconic(w))
                SysCommand(w, SC_MINIMIZE);
            else
                BringToFront(w);
            return;
        }
        // Windows are most-recently-active first: the last one is the next in turn.
        BringToFront(inGroup ? windows.back() : windows.front());
    }

    void CloseAll(const std::wstring& key)
    {
        const TaskItem* t = Find(key);
        if (!t)
            return;
        for (HWND h : t->windows)
            PostMessageW(h, WM_CLOSE, 0, 0);
    }

    std::vector<AppProcessIdentity> TerminationTargets(const std::wstring& key) const
    {
        const auto found = meta.find(key);
        if (found == meta.end())
            return {};
        if (!found->second.processTargets.empty())
            return found->second.processTargets;
        // Pinned apps can keep running in the tray after their last window closes.
        return {{found->second.exePath, found->second.aumid}};
    }

    PinnedApp PinFor(const std::wstring& key) const
    {
        PinnedApp p;
        const TaskItem* t = Find(key);
        auto m = meta.find(key);
        if (!t || m == meta.end())
            return p;
        p.name = t->name;
        p.path = m->second.exePath;
        p.aumid = m->second.aumid;
        return p;
    }

    // Exe that starts the item, unless it is a packaged app (those are only
    // started through their AUMID).
    std::wstring DesktopExe(const std::wstring& key) const
    {
        auto m = meta.find(key);
        if (m == meta.end() || m->second.exePath.empty() || Lower(m->second.exePath).find(L"\\windowsapps\\") != std::wstring::npos)
            return {};
        return m->second.exePath;
    }

    std::wstring Aumid(const std::wstring& key) const
    {
        auto m = meta.find(key);
        return m == meta.end() ? std::wstring() : m->second.aumid;
    }

    void Show(const std::wstring& key)
    {
        const TaskItem* t = Find(key);
        if (t && !t->windows.empty() && !t->active)
            BringToFront(t->windows.front());
    }

    void MoveRunning(const std::wstring& key, int to)
    {
        auto it = std::find(order.begin(), order.end(), key);
        if (it == order.end())
            return;
        order.erase(it);
        order.insert(order.begin() + std::clamp<std::ptrdiff_t>(to, 0, std::ptrdiff_t(order.size())), key);
        if (Rebuild()) {
            notify = true;
            ArmTimer();
        }
    }

    bool OpenFiles(const std::wstring& key, const std::vector<std::wstring>& files)
    {
        if (files.empty())
            return false;
        if (std::wstring exe = DesktopExe(key); !exe.empty()) {
            std::wstring args;
            for (const auto& f : files)
                args += (args.empty() ? L"\"" : L" \"") + f + L"\"";
            Log(L"tasks: opening %zu file(s) with %s", files.size(), exe.c_str());
            return Exec(exe, args, {});
        }
        std::wstring aumid = Aumid(key);
        if (aumid.empty())
            return false;
        winrt::com_ptr<IApplicationActivationManager> aam;
        if (FAILED(CoCreateInstance(CLSID_ApplicationActivationManager, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(aam.put()))))
            return false;
        std::vector<PIDLIST_ABSOLUTE> ids;
        for (const auto& f : files) {
            PIDLIST_ABSOLUTE id = nullptr;
            if (SUCCEEDED(SHParseDisplayName(f.c_str(), nullptr, &id, 0, nullptr)))
                ids.push_back(id);
        }
        winrt::com_ptr<IShellItemArray> array;
        bool ok = !ids.empty() &&
                  SUCCEEDED(SHCreateShellItemArrayFromIDLists(UINT(ids.size()), const_cast<PCIDLIST_ABSOLUTE_ARRAY>(ids.data()), array.put()));
        DWORD pid = 0;
        ok = ok && SUCCEEDED(aam->ActivateForFile(aumid.c_str(), array.get(), L"open", &pid));
        if (ok)
            Log(L"tasks: opening %zu file(s) with %s", ids.size(), aumid.c_str());
        for (auto id : ids)
            CoTaskMemFree(id);
        if (!ok)
            Log(L"tasks: opening files with %s failed", aumid.c_str());
        return ok;
    }
};

TaskModel::Impl* TaskModel::Impl::self = nullptr;

bool PinnedApp::AllowsMultipleWindows() const
{
    if (multipleWindows) return *multipleWindows;
    std::wstring exe = Lower(FileStem(path));
    return exe == L"explorer" || Lower(aumid) == L"microsoft.windows.explorer" ||
           exe == L"chrome" || exe == L"msedge" || exe == L"firefox" || exe == L"brave" ||
           exe == L"opera" || exe == L"vivaldi" || exe == L"chromium";
}

std::wstring AppExecutable(const std::wstring& path)
{
    std::wstring target = path;
    if (IsLink(path)) {
        std::wstring aumid;
        if (!ReadLink(path, target, aumid)) return {};
    }
    size_t slash = target.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        std::wstring app = target.substr(0, slash + 1) + L"app\\" + target.substr(slash + 1);
        DWORD attr = GetFileAttributesW(app.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY))
            return app;
    }
    return target;
}

bool LoadAtriumIcon(const std::wstring& exePath, int sizePx, IconImage& out)
{
    const size_t slash = exePath.find_last_of(L"\\/");
    const wchar_t* name = exePath.c_str() + (slash == std::wstring::npos ? 0 : slash + 1);
    if (_wcsicmp(name, L"Atrium.Settings.exe") != 0 && _wcsicmp(name, L"Atrium.exe") != 0)
        return false;
    HICON icon = nullptr;
    if (SHDefExtractIconW(exePath.c_str(), 0, 0, &icon, nullptr, MAKELONG(sizePx, 0)) != S_OK || !icon)
        return false;
    IconImage raw;
    bool ok = IconToImage(icon, raw);
    DestroyIcon(icon);
    if (ok)
        out = FitToSquare(raw, sizePx);
    return ok;
}

bool CopyIconImage(HICON icon, int sizePx, IconImage& out)
{
    if (!icon)
        return false;
    IconImage raw;
    if (!IconToImage(icon, raw))
        return false;
    out = sizePx > 0 ? FitToSquare(raw, sizePx) : std::move(raw);
    return true;
}

bool LoadShellImage(const std::wstring& parsingName, int sizePx, IconImage& out, bool thumbnail)
{
    try {
        if (!thumbnail) {
            std::wstring source = AppExecutable(parsingName);
            int index = 0;
            if (IsLink(parsingName)) {
                winrt::com_ptr<IShellLinkW> link;
                if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(link.put())))) {
                    auto file = link.as<IPersistFile>();
                    wchar_t iconPath[MAX_PATH * 2]{};
                    if (SUCCEEDED(file->Load(parsingName.c_str(), STGM_READ)) &&
                        SUCCEEDED(link->GetIconLocation(iconPath, ARRAYSIZE(iconPath), &index)) && *iconPath)
                        source = iconPath;
                }
            }
            HICON icon = nullptr;
            if (!source.empty() && SHDefExtractIconW(source.c_str(), index, 0, &icon, nullptr, MAKELONG(sizePx, 0)) == S_OK && icon) {
                IconImage raw;
                bool ok = IconToImage(icon, raw);
                DestroyIcon(icon);
                if (ok) { out = FitToSquare(raw, sizePx); return true; }
            }
        }
        return ShellImage(parsingName, sizePx, out, thumbnail);
    } catch (...) {
        return false;
    }
}

// ---- TaskModel: every entry point is exception-safe -----------------------------

TaskModel::TaskModel(HWND uiWindow, std::function<void()> onChanged, std::function<bool(const std::wstring&)> openExisting)
{
    impl_ = std::make_unique<Impl>(uiWindow, std::move(onChanged), std::move(openExisting));
}

TaskModel::~TaskModel() = default;

std::vector<TaskItem> TaskModel::Snapshot(bool all) const
{
    try {
        return impl_->Snapshot(all);
    } catch (...) {
        Log(L"tasks: Snapshot failed");
        return {};
    }
}

void TaskModel::SetPins(const std::vector<PinnedApp>& pins)
{
    try {
        impl_->SetPins(pins);
    } catch (...) {
        Log(L"tasks: SetPins failed");
    }
}

void TaskModel::SetShowRunning(bool show)
{
    impl_->SetShowRunning(show);
}

IconImage TaskModel::Icon(const std::wstring& key, int sizePx)
{
    try {
        return impl_->Icon(key, sizePx);
    } catch (...) {
        Log(L"tasks: Icon failed");
        return {};
    }
}

void TaskModel::Activate(const std::wstring& key, HMONITOR monitor)
{
    try {
        impl_->Activate(key, monitor);
    } catch (...) {
        Log(L"tasks: Activate failed");
    }
}

void TaskModel::ActivateWindow(HWND window)
{
    if (IsWindow(window))
        Impl::BringToFront(window);
}

bool TaskModel::CloseWindow(HWND window)
{
    if (!IsWindow(window)) {
        SetLastError(ERROR_INVALID_WINDOW_HANDLE);
        return false;
    }
    if (!PostMessageW(window, WM_SYSCOMMAND, SC_CLOSE, 0)) {
        const DWORD error = GetLastError();
        Log(L"tasks: window close request failed for %p (%lu)", window, error);
        return false;
    }
    return true;
}

void TaskModel::Launch(const std::wstring& key)
{
    try {
        impl_->Launch(key);
    } catch (...) {
        Log(L"tasks: Launch failed");
    }
}

void TaskModel::CloseAll(const std::wstring& key)
{
    try {
        impl_->CloseAll(key);
    } catch (...) {
        Log(L"tasks: CloseAll failed");
    }
}

bool TaskModel::CanTerminate(const std::wstring& key) const
{
    const auto targets = impl_->TerminationTargets(key);
    return std::any_of(targets.begin(), targets.end(), CanTerminateApp);
}

void TaskModel::Terminate(const std::wstring& key)
{
    try {
        const auto targets = impl_->TerminationTargets(key);
        TerminateAppProcesses(targets);
        impl_->MarkDirty();
    } catch (...) {
        Log(L"tasks: Terminate failed");
    }
}

PinnedApp TaskModel::PinFor(const std::wstring& key) const
{
    try {
        return impl_->PinFor(key);
    } catch (...) {
        return {};
    }
}

void TaskModel::Show(const std::wstring& key)
{
    try {
        impl_->Show(key);
    } catch (...) {
        Log(L"tasks: Show failed");
    }
}

void TaskModel::MoveRunning(const std::wstring& key, int toIndex)
{
    try {
        impl_->MoveRunning(key, toIndex);
    } catch (...) {
        Log(L"tasks: MoveRunning failed");
    }
}

bool TaskModel::CanOpenFiles(const std::wstring& key) const
{
    try {
        return !impl_->DesktopExe(key).empty() || !impl_->Aumid(key).empty();
    } catch (...) {
        return false;
    }
}

bool TaskModel::OpenFiles(const std::wstring& key, const std::vector<std::wstring>& files)
{
    try {
        return impl_->OpenFiles(key, files);
    } catch (...) {
        Log(L"tasks: OpenFiles failed");
        return false;
    }
}

std::wstring TaskModel::FileLocation(const std::wstring& key) const
{
    try {
        return impl_->DesktopExe(key);
    } catch (...) {
        return {};
    }
}

bool TaskModel::HandleMessage(UINT msg, WPARAM wp, LPARAM lp)
{
    return impl_->HandleMessage(msg, wp, lp);
}

std::vector<PinnedApp> TaskModel::ImportWindowsPins()
{
    std::vector<PinnedApp> pins;
    try {
        ComScope com;
        PWSTR base = nullptr;
        std::wstring dir;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &base)))
            dir = std::wstring(base) + L"\\Microsoft\\Internet Explorer\\Quick Launch\\User Pinned\\TaskBar";
        CoTaskMemFree(base);
        if (dir.empty())
            return pins;
        WIN32_FIND_DATAW fd{};
        HANDLE find = FindFirstFileW((dir + L"\\*.lnk").c_str(), &fd);
        if (find == INVALID_HANDLE_VALUE)
            return pins;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                continue;
            PinnedApp p;
            p.path = dir + L"\\" + fd.cFileName;
            p.name = fd.cFileName;
            p.name.resize(p.name.size() - 4); // ".lnk"
            std::wstring target;
            ReadLink(p.path, target, p.aumid);
            pins.push_back(std::move(p));
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    } catch (...) {
        Log(L"tasks: ImportWindowsPins failed");
    }
    return pins;
}

} // namespace atrium
