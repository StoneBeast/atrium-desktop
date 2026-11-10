#include "ui/tray_panel.h"

#include "clock.h"
#include "log.h"
#include "motion.h"

#include <shellscalingapi.h>
#include <shlwapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace atrium {

namespace {

// Small, like the system's own tray. Layout by what is seen: the icons'
// edges, the footer's text and button keep one margin from the panel's
// edges (Margin(), from the corner radius, so nothing crowds a corner), and
// the divider sits halfway between the icons and the footer.
constexpr float kCell = 40, kIcon = 20, kAir = (kCell - kIcon) / 2; // kAir: from a cell's edge to its icon
constexpr float kSplit = 14, kFooterH = 24;                            // icons to divider to footer; footer row
constexpr double kStageMs = 240, kStaggerMs = 14, kRippleMs = 520;

D2D1_COLOR_F Alpha(D2D1_COLOR_F c, float a) { return D2D1::ColorF(c.r, c.g, c.b, c.a * a); }

// FNV-1a over the pixels: a blinking icon delivers a new image every few hundred
// milliseconds, and a bitmap per image pointer would never be freed. The same
// pixels keep one bitmap.
uint64_t ImageHash(const IconImage& image)
{
    uint64_t hash = 1469598103934665603ull;
    for (uint8_t byte : image.bgra) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    hash ^= uint64_t(unsigned(image.width)) * 0x9E3779B97F4A7C15ull;
    hash ^= uint64_t(unsigned(image.height)) * 0xC2B2AE3D27D4EB4Full;
    return hash;
}
bool In(const D2D1_RECT_F& r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }

// An app's menu opened through the hidden system tray: Explorer tells the app
// where its (hidden) button is, and some apps open the menu there, at the
// screen's left edge. The next window the app shows within a moment is moved
// to where the icon was clicked, as Windows places a tray menu. An
// out-of-context event hook: nothing runs in the app or in Explorer.
struct MenuFollow {
    HWINEVENTHOOK hook = nullptr;
    POINT at{};
    std::wstring dir; // the app's folder (its helper processes are there too); empty: any app but Explorer
    ULONGLONG until = 0;
    HWND menu = nullptr; // the menu moved: put back if the app moves it again meanwhile
    POINT placed{};
    int moves = 0;
} g_follow;

void StopFollow()
{
    if (g_follow.hook)
        UnhookWinEvent(g_follow.hook);
    g_follow.hook = nullptr;
    g_follow.menu = nullptr;
}

// Next to the click: above and to the right of it, else below or to the left; in the work area.
POINT MenuPlace(POINT at, int w, int h)
{
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(at, MONITOR_DEFAULTTONEAREST), &mi);
    const RECT& wa = mi.rcWork;
    int left = at.x + w <= wa.right ? at.x : at.x - w, top = at.y - h >= wa.top ? at.y - h : at.y;
    left = std::clamp(left, int(wa.left), std::max(int(wa.left), int(wa.right) - w));
    top = std::clamp(top, int(wa.top), std::max(int(wa.top), int(wa.bottom) - h));
    return POINT{left, top};
}

void CALLBACK MenuShown(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD, DWORD)
{
    if (event != EVENT_OBJECT_SHOW && event != EVENT_OBJECT_LOCATIONCHANGE)
        return;
    if (GetTickCount64() > g_follow.until) {
        StopFollow();
        return;
    }
    if (!hwnd || idObject != OBJID_WINDOW || idChild != CHILDID_SELF)
        return;
    RECT r{};
    if (g_follow.menu) {
        // Moved already: some apps (WPF, a change of monitor's scale) place it once more themselves.
        if (hwnd != g_follow.menu || !IsWindowVisible(hwnd) || !GetWindowRect(hwnd, &r))
            return;
        // A pixel or two is the app rounding to its scale.
        if ((std::abs(r.left - g_follow.placed.x) > 2 || std::abs(r.top - g_follow.placed.y) > 2) && g_follow.moves < 3) {
            const POINT p = MenuPlace(g_follow.at, r.right - r.left, r.bottom - r.top);
            g_follow.placed = p;
            ++g_follow.moves;
            SetWindowPos(hwnd, nullptr, p.x, p.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            Log(L"tray: the menu went to %ld,%ld again: back to %ld,%ld", r.left, r.top, p.x, p.y);
        }
        return;
    }
    // Some apps keep their menu window and only move it into view (an event of its place, not of its showing).
    if (GetAncestor(hwnd, GA_ROOT) != hwnd || !IsWindowVisible(hwnd) || !GetWindowRect(hwnd, &r))
        return;
    wchar_t cls[64] = {};
    GetClassNameW(hwnd, cls, 64);
    const LONG style = GetWindowLongW(hwnd, GWL_STYLE), ex = GetWindowLongW(hwnd, GWL_EXSTYLE);
    // A menu: Windows' own, or a popup above the others (the toolkits' menus).
    const bool menuLike = wcscmp(cls, L"#32768") == 0 || ((style & WS_POPUP) && (ex & (WS_EX_TOOLWINDOW | WS_EX_TOPMOST))) || (ex & WS_EX_TOOLWINDOW);
    if (!menuLike || _wcsicmp(cls, L"tooltips_class32") == 0)
        return;
    const int w = r.right - r.left, h = r.bottom - r.top;
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(g_follow.at, MONITOR_DEFAULTTONEAREST), &mi);
    if (w < 40 || h < 20 || w > (mi.rcWork.right - mi.rcWork.left) * 3 / 5 || h > (mi.rcWork.bottom - mi.rcWork.top) * 19 / 20)
        return; // a mark, a balloon, a window: not a menu
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    std::wstring path;
    bool elevated = true; // its token closed to us: elevated (or protected)
    if (HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t buf[MAX_PATH * 2];
        DWORD size = DWORD(std::size(buf));
        if (QueryFullProcessImageNameW(process, 0, buf, &size))
            path = buf;
        HANDLE token = nullptr;
        if (OpenProcessToken(process, TOKEN_QUERY, &token)) {
            TOKEN_ELEVATION e{};
            DWORD got = 0;
            elevated = GetTokenInformation(token, TokenElevation, &e, sizeof(e), &got) && e.TokenIsElevated;
            CloseHandle(token);
        }
        CloseHandle(process);
    }
    const wchar_t* name = PathFindFileNameW(path.c_str());
    if (_wcsicmp(name, L"explorer.exe") == 0 || (!g_follow.dir.empty() && _wcsnicmp(path.c_str(), g_follow.dir.c_str(), g_follow.dir.size()) != 0))
        return;
    const POINT at = g_follow.at;
    if (elevated) { // Windows keeps an elevated app's windows from being moved by Atrium
        StopFollow();
        Log(L"tray: %s's menu is an elevated app's: left where it is", name);
        return;
    }
    if (at.x >= r.left - 48 && at.x <= r.right + 48 && at.y >= r.top - 48 && at.y <= r.bottom + 48) {
        StopFollow();
        return; // the app put it by the pointer itself
    }
    const POINT p = MenuPlace(at, w, h);
    if (!SetWindowPos(hwnd, nullptr, p.x, p.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE)) {
        Log(L"tray: %s's menu (%s) at %ld,%ld could not be moved (%lu)", name, cls, r.left, r.top, GetLastError());
        StopFollow();
        return;
    }
    Log(L"tray: %s's menu (%s) moved from %ld,%ld to %ld,%ld", name, cls, r.left, r.top, p.x, p.y);
    g_follow.menu = hwnd;
    g_follow.placed = p;
    g_follow.moves = 0;
    g_follow.until = GetTickCount64() + 800;
}

void FollowMenu(POINT at, const std::wstring& exe)
{
    StopFollow();
    g_follow.at = at;
    g_follow.dir.clear();
    if (!exe.empty()) {
        // Its folder with the separator (C:\a\app.exe -> C:\a\), so that C:\ab\ is not taken for it.
        g_follow.dir = exe.substr(0, exe.find_last_of(L"\\/") + 1);
    }
    g_follow.until = GetTickCount64() + 3000; // slow apps take a while to show it
    g_follow.hook = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_LOCATIONCHANGE, nullptr, MenuShown, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
}
} // namespace

TrayPanel::TrayPanel(HINSTANCE instance, TrayModel& model) : surface_(instance, L"Atrium Tray", true), model_(model)
{
    surface_.paint = [this](Canvas& c) { Paint(c); };
    surface_.message = [this](UINT m, WPARAM w, LPARAM l, LRESULT& r) { return Message(m, w, l, r); };
    surface_.onDismiss = [this] { Close(); };
    wchar_t privacy[8] = {};
    private_ = GetEnvironmentVariableW(L"ATRIUM_PRIVACY", privacy, 8) && privacy[0] == L'1';
}

float TrayPanel::Margin() const { return std::clamp(radius_ * 0.65f, 14.f, 22.f); }

int TrayPanel::Cols() const
{
    // At most six across, the rows as even as they can be (13 icons: 5, 5, 3), and wide enough for the footer.
    const int n = int(icons_.size()), rows = std::max(1, (n + 5) / 6);
    return std::clamp((n + rows - 1) / rows, 4, 6);
}

D2D1_SIZE_F TrayPanel::Size() const
{
    int cols = Cols(), rows = std::max(1, int((icons_.size() + size_t(cols) - 1) / size_t(cols)));
    const float m = Margin();
    return D2D1::SizeF(2 * (m - kAir) + float(cols) * kCell, (m - kAir) + float(rows) * kCell - kAir + 2 * kSplit + kFooterH + m);
}

D2D1_RECT_F TrayPanel::CellRect(size_t i) const
{
    // Rows from the top, left to right; a last row that is not full keeps to the left, as
    // in a grid (centred, its icons looked like gaps between the ones above).
    const size_t cols = size_t(Cols()), row = i / cols;
    const float pad = Margin() - kAir;
    float x = pad + float(i % cols) * kCell, y = pad + float(row) * kCell;
    return D2D1::RectF(x, y, x + kCell, y + kCell);
}

void TrayPanel::Place()
{
    RECT r = PopupRect(bar_, Size(), PopupAlign::Right);
    if (surface_.Visible())
        surface_.Move(r); // pops out of its entry on the bar
    else
        surface_.Show(r, true, POINT{(anchor_.left + anchor_.right) / 2, r.bottom});
}

void TrayPanel::Open(RECT anchor, RECT bar, HWND previous)
{
    previous_ = previous;
    anchor_ = anchor;
    bar_ = bar;
    icons_ = model_.Icons();
    hover_.clear();
    pressed_.clear();
    hoverAnim_.clear();
    openedMs_ = ClockMs();
    model_.Refresh(); // the tooltips and the set change; the panel updates when read
    Place();
    Log(L"tray: panel opened (%zu icons)", icons_.size());
}

void TrayPanel::Close()
{
    if (!surface_.Visible())
        return;
    surface_.Hide();
    lastClosed_ = GetTickCount64();
}

void TrayPanel::Refresh()
{
    if (!surface_.Visible())
        return;
    const D2D1_SIZE_F before = Size();
    icons_ = model_.Icons();
    // A live icon update (a blink) only changes the picture; moving the panel
    // again for it would make the window flicker. The grid's size changes when
    // the icons themselves do.
    if (Size().width != before.width || Size().height != before.height)
        Place();
    surface_.Invalidate();
}

float TrayPanel::Hover(Canvas& c, const std::wstring& id, bool on)
{
    auto& f = hoverAnim_[id];
    float v = f.Step(on, c.now);
    if (f.moving)
        c.Continue();
    return v;
}

void TrayPanel::Paint(Canvas& c)
{
    const Theme& t = c.theme;
    const D2D1_SIZE_F size = Size();
    const float w = size.width, h = size.height;
    hits_.clear();
    if (c.panel)
        c.Fill(D2D1::RectF(0, 0, w, h), radius_, t.panel);

    const float m = Margin(), gridTop = m - kAir, footTop = h - m - kFooterH, divider = footTop - kSplit;
    const float plateR = std::clamp(radius_ - m - 2, 6.f, 12.f); // concentric with the panel's corners
    if (icons_.empty()) {
        std::wstring msg = !model_.Loaded() ? L"正在读取…" : model_.Available() ? L"没有托盘图标" : L"读取不到系统托盘";
        c.Text(msg, D2D1::RectF(m, gridTop, w - m, gridTop + kCell), 12.5f, t.subtext, DWRITE_FONT_WEIGHT_NORMAL, 1);
    }
    const int iconPx = int(std::lround(kIcon * c.scale));
    for (size_t i = 0; i < icons_.size(); ++i) {
        const TrayIcon& icon = icons_[i];
        D2D1_RECT_F cell = CellRect(i);
        std::wstring id = L"icon:" + std::to_wstring(i);
        float e = motion::EaseOut(float((c.now - openedMs_ - kStaggerMs * double(std::min<size_t>(i, 18))) / kStageMs));
        if (e < 1)
            c.Continue();
        c.BeginGroup(e, 1, (cell.left + cell.right) / 2, (cell.top + cell.bottom) / 2, 0, 6 * (1 - e));
        float hv = Hover(c, id, hover_ == id);
        D2D1_RECT_F plate = D2D1::RectF(cell.left + 2, cell.top + 2, cell.right - 2, cell.bottom - 2);
        if (hv > 0.001f)
            c.Fill(plate, plateR, Alpha(t.hover, hv));
        if (pressed_ == id)
            c.Fill(plate, plateR, t.press);
        if (rippleId_ == id && c.now - rippleMs_ < kRippleMs) {
            c.Ripple(plate, plateR, rippleX_, rippleY_, float((c.now - rippleMs_) / kRippleMs), t.light ? D2D1::ColorF(1, 1, 1, 0.6f) : D2D1::ColorF(1, 1, 1, 0.22f));
            c.Continue();
        }
        float cx = (cell.left + cell.right) / 2, cy = (cell.top + cell.bottom) / 2;
        D2D1_RECT_F ir = D2D1::RectF(cx - kIcon / 2, cy - kIcon / 2, cx + kIcon / 2, cy + kIcon / 2);
        c.BeginGroup(1, pressed_ == id ? 0.9f : 1 + 0.1f * hv, cx, cy);
        if (icon.image) {
            // A white or black glyph follows the ink as the panel's text does (0.50 / 0.42, faded):
            // its inked copy over bright glass, or over dark glass, cross-fading with the ink.
            const float dark = icon.inked ? c.DarkAt(ir) : 0.f;
            const float inked = !icon.inked ? 0.f : icon.inkedOnLight ? dark : 1 - dark;
            // Two layers, so a glyph can cross-fade into the other ink. The key is the picture's
            // content rather than the object: the broker hands over a new image every few
            // hundred milliseconds (a blink), and a bitmap per pointer would never be freed.
            wchar_t key[80];
            const auto layer = [&](const IconImage& image, const wchar_t* which, float opacity) {
                swprintf_s(key, L"tray|%s|%016llX|%d", which, static_cast<unsigned long long>(ImageHash(image)), iconPx);
                c.Image(key, image, ir, opacity);
            };
            if (inked < 0.999f)
                layer(*icon.image, L"base", 1 - inked);
            if (icon.inked && inked > 0.001f)
                layer(*icon.inked, L"inked", inked);
        } else {
            // Not recognised: its initial on a quiet disc.
            std::wstring title = TrayModel::Title(icon.tip);
            c.Fill(ir, kIcon / 2, Alpha(t.hover, 1.4f));
            c.Text(title.empty() ? L"?" : title.substr(0, 1), ir, 10, t.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
        }
        c.EndGroup();
        c.EndGroup();
        hits_.push_back({cell, id});
    }

    // Below the grid: the name under the pointer, and the system's own tray.
    c.Line(m, divider, w - m, divider, t.divider);
    std::wstring label;
    if (hover_.rfind(L"icon:", 0) == 0) {
        size_t i = size_t(std::stoul(hover_.substr(5)));
        if (i < icons_.size())
            label = private_ ? L"托盘图标" : TrayModel::Title(icons_[i].tip);
    }
    if (label.empty())
        label = icons_.empty() ? L"托盘" : std::to_wstring(icons_.size()) + L" 个图标";
    std::wstring sys = L"系统托盘";
    float bw = c.TextWidth(sys, 11.5f) + 20;
    D2D1_RECT_F b = D2D1::RectF(w - m - bw, footTop, w - m, footTop + kFooterH);
    c.Text(label, D2D1::RectF(m, footTop, b.left - 10, footTop + kFooterH), 12, hover_.rfind(L"icon:", 0) == 0 ? t.text : t.subtext);
    float hv = Hover(c, L"system", hover_ == L"system");
    c.Fill(b, (b.bottom - b.top) / 2, Alpha(pressed_ == L"system" ? t.press : t.hover, 0.45f + 0.55f * hv));
    c.Text(sys, b, 11.5f, t.text, DWRITE_FONT_WEIGHT_NORMAL, 1);
    hits_.push_back({b, L"system"});
}

std::wstring TrayPanel::HitAt(POINT pt) const
{
    float s = surface_.Scale(), x = float(pt.x) / s, y = float(pt.y) / s;
    for (const Hit& h : hits_)
        if (In(h.rect, x, y))
            return h.id;
    return {};
}

void TrayPanel::Activate(const std::wstring& id, bool right)
{
    if (id == L"system") {
        if (!right) {
            Close();
            if (onSystemTray)
                onSystemTray();
        }
        return;
    }
    if (id.rfind(L"icon:", 0) != 0)
        return;
    size_t i = size_t(std::stoul(id.substr(5)));
    if (i >= icons_.size())
        return;
    // We have the foreground: let the app take it for its window or menu.
    AllowSetForegroundWindow(ASFW_ANY);
    if (right) {
        POINT at;
        GetCursorPos(&at);
        FollowMenu(at, icons_[i].exe);
    }
    model_.Click(icons_[i], right);
    // A left click usually opens the app's window: the panel steps aside. A
    // right click opens the app's menu, which takes the focus and closes it.
    if (!right)
        Close();
}

bool TrayPanel::Message(UINT m, WPARAM w, LPARAM l, LRESULT& result)
{
    POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
    switch (m) {
    case WM_ACTIVATE:
        if (LOWORD(w) == WA_INACTIVE && surface_.Visible())
            Close();
        return false;
    case WM_MOUSEMOVE: {
        if (!tracking_) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, surface_.Hwnd(), 0};
            tracking_ = TrackMouseEvent(&tme) != FALSE;
        }
        std::wstring h = HitAt(pt);
        if (h != hover_) {
            hover_ = h;
            surface_.Invalidate();
        }
        return true;
    }
    case WM_MOUSELEAVE:
        tracking_ = false;
        hover_.clear();
        pressed_.clear();
        surface_.Invalidate();
        return true;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
        pressed_ = HitAt(pt);
        if (!pressed_.empty()) {
            float s = surface_.Scale();
            rippleId_ = pressed_;
            rippleX_ = float(pt.x) / s;
            rippleY_ = float(pt.y) / s;
            rippleMs_ = ClockMs();
            surface_.Ripple(rippleX_, rippleY_);
        }
        surface_.Invalidate();
        return true;
    case WM_LBUTTONUP:
    case WM_RBUTTONUP: {
        std::wstring h = HitAt(pt);
        bool same = !h.empty() && h == pressed_;
        pressed_.clear();
        surface_.Invalidate();
        if (same)
            Activate(h, m == WM_RBUTTONUP);
        return true;
    }
    case WM_KEYDOWN:
        if (w == VK_ESCAPE) {
            Close();
            return true;
        }
        return false;
    }
    (void)result;
    return false;
}

} // namespace atrium
