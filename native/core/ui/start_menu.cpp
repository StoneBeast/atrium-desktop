#include "ui/start_menu.h"

#include "clock.h"
#include "log.h"
#include "motion.h"
#include "render/wifi_glyph.h"

#include <imm.h>
#include <sddl.h>
#include <shellscalingapi.h>
#include <shellapi.h>
#include <windowsx.h>
#define SECURITY_WIN32
#include <security.h>
#include <secext.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <set>

#pragma comment(lib, "imm32.lib")
#pragma comment(lib, "secur32.lib")
#pragma comment(lib, "advapi32.lib")

namespace atrium {

namespace {

// DIP. The panel above, the status capsule (the bar's height) kGap below it.
// The panel: the search row, the grid (cols x rows cells of kCellW x kCellH,
// kGridPad at the sides; the medium 9 x 4 makes the 704 x 480 of DESIGN.md), the page dots.
constexpr float kGap = 12;
constexpr float kSearchX = 24, kSearchTop = 20, kSearchH = 44, kAllW = 108;
constexpr float kGridTop = 86, kGridFoot = 42, kGridPad = 28, kCellW = 72, kCellH = 88, kIcon = 40;
constexpr float kRowH = 52, kRowIcon = 28, kHeadH = 34, kListTop = 84;
constexpr float kFolderHead = 52, kFolderFoot = 24, kFolderPad = 14; // the open folder: its name above its apps, its dots below
constexpr float kIndexW = 22, kHint = 52;           // the A-Z list's letters down its left side; the letter picked, large
constexpr float kPowerW = 176, kPowerRowH = 44, kPowerPad = 12;
constexpr float kGrip = 30; // the resize grip: the panel's top right corner
constexpr UINT_PTR kStatusTimer = 1, kDragTimer = 2;
constexpr double kStageMs = 260, kStaggerMs = 12, kRippleMs = 520, kPowerMs = 220;
// The folder's card out of its tile, with a little overshoot; back into it quicker, without.
constexpr motion::Spring kFolderOpen{0.46f, 0.72f}, kFolderClose{0.30f, 1.f};
constexpr double kFolderAppMs = 300, kFolderAppStaggerMs = 22; // its apps, one after another
constexpr double kRestMs = 220, kEdgeMs = 550;           // carrying a tile: the others make room; the page turns
constexpr float kGlideRate = 16;                         // tiles moving to their places (per second)
constexpr motion::Spring kResize{0.40f, 0.82f};          // the panel and the capsule to another size
const wchar_t kLetters[] = L"#ABCDEFGHIJKLMNOPQRSTUVWXYZ";

D2D1_COLOR_F Alpha(D2D1_COLOR_F c, float a) { return D2D1::ColorF(c.r, c.g, c.b, c.a * a); }
bool In(const D2D1_RECT_F& r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }
bool IsPath(const std::wstring& id) { return id.size() > 2 && id[1] == L':' && (id[2] == L'\\' || id[2] == L'/'); }
bool Starts(const std::wstring& s, const wchar_t* prefix) { return s.rfind(prefix, 0) == 0; }
int Index(const std::wstring& id, size_t prefix) { return int(wcstol(id.c_str() + prefix, nullptr, 10)); }
float Mix(float a, float b, float t) { return a + (b - a) * t; }
D2D1_RECT_F Mix(const D2D1_RECT_F& a, const D2D1_RECT_F& b, float t)
{
    return D2D1::RectF(Mix(a.left, b.left, t), Mix(a.top, b.top, t), Mix(a.right, b.right, t), Mix(a.bottom, b.bottom, t));
}
constexpr wchar_t kHiddenHead = 1; // the heading of the hidden apps, after Z
int LetterRank(wchar_t letter) { return letter == L'#' ? 0 : letter >= L'A' && letter <= L'Z' ? letter - L'A' + 1 : 27; }

// An app taken out of the arrangement: a tile of its own or one of a folder's
// (a folder left empty goes with it).
std::optional<StartTile> Take(std::vector<StartTile>& tiles, const std::wstring& id)
{
    for (auto it = tiles.begin(); it != tiles.end(); ++it) {
        if (!it->Folder()) {
            if (it->id == id) {
                StartTile t = std::move(*it);
                tiles.erase(it);
                return t;
            }
            continue;
        }
        for (auto a = it->apps.begin(); a != it->apps.end(); ++a)
            if (a->id == id) {
                StartTile t = std::move(*a);
                it->apps.erase(a);
                if (it->apps.empty())
                    tiles.erase(it);
                return t;
            }
    }
    return std::nullopt;
}

// The arrangement in the order of `keys` (the tiles shown); tiles not shown
// (apps not found yet) keep to the end.
void Reorder(std::vector<StartTile>& tiles, const std::vector<std::wstring>& keys)
{
    std::vector<StartTile> out;
    for (const std::wstring& key : keys)
        if (auto it = std::find_if(tiles.begin(), tiles.end(), [&](const StartTile& t) { return t.id == key; }); it != tiles.end()) {
            out.push_back(std::move(*it));
            tiles.erase(it);
        }
    for (StartTile& t : tiles)
        out.push_back(std::move(t));
    tiles = std::move(out);
}

StartTile* Find(std::vector<StartTile>& tiles, const std::wstring& id)
{
    auto it = std::find_if(tiles.begin(), tiles.end(), [&](const StartTile& t) { return t.id == id; });
    return it == tiles.end() ? nullptr : &*it;
}

enum MenuCommand { kOpen = 1, kPinBar, kPinStart, kRemove, kOutOfFolder, kRename, kUngroup, kReset, kHide, kUnhide, kShowHidden, kSize /* + the size */ };

} // namespace

StartMenu::StartMenu(HINSTANCE instance, TaskModel& tasks, AppCatalog& catalog)
    : surface_(instance, L"Atrium Start", true), tasks_(tasks), catalog_(catalog), layout_(LoadStartLayout())
{
    surface_.paint = [this](Canvas& c) { Paint(c); };
    surface_.message = [this](UINT m, WPARAM w, LPARAM l, LRESULT& r) { return Message(m, w, l, r); };
    surface_.onDismiss = [this] { Close(false); }; // a press next to the menu
    cols_ = StartLayout::kCols[layout_.size];
    rows_ = StartLayout::kRows[layout_.size];
    shown_ = layout_.size;
    Log(L"start: %d x %d tiles, %s", StartLayout::kCols[layout_.size], StartLayout::kRows[layout_.size],
        layout_.custom ? L"arranged by the user" : L"the Windows start menu's pins");
}

// ---- Geometry ----

float StartMenu::W() const { return panelW_; }
float StartMenu::H() const { return mainH_ + kGap + barH_; }
D2D1_SIZE_F StartMenu::SizeFor(int size) const
{
    return D2D1::SizeF(float(StartLayout::kCols[size]) * kCellW + 2 * kGridPad, kGridTop + float(StartLayout::kRows[size]) * kCellH + kGridFoot + kGap + barH_);
}
D2D1_SIZE_F StartMenu::Size() const { return SizeFor(room_); }
float StartMenu::OffsetY() const { return float(surface_.Rect().bottom - surface_.Rect().top) / surface_.Scale() - H(); }
LONG StartMenu::OffsetPx() const { return std::lround(OffsetY() * surface_.Scale()); }

bool StartMenu::Fits(int size) const
{
    const D2D1_SIZE_F z = SizeFor(size);
    const float scale = PopupScale(bar_);
    const RECT r = PopupRect(bar_, z, PopupAlign::Left);
    return r.bottom - r.top >= std::lround(z.height * scale) - 1 && r.right - r.left >= std::lround(z.width * scale) - 1;
}

void StartMenu::SetSize(int size)
{
    // The grid takes the new size at once (the tiles glide to their places), the glass springs to it.
    shown_ = std::clamp(size, 0, room_);
    cols_ = StartLayout::kCols[shown_];
    rows_ = StartLayout::kRows[shown_];
    page_ = std::clamp(page_, 0, Pages() - 1);
    folderPage_ = std::clamp(folderPage_, 0, FolderPages() - 1);
    surface_.Invalidate();
}
D2D1_RECT_F StartMenu::MainRect() const { return D2D1::RectF(0, 0, W(), H() - barH_ - kGap); }
D2D1_RECT_F StartMenu::ShelfRect() const { return D2D1::RectF(0, H() - barH_, W(), H()); }
D2D1_RECT_F StartMenu::AllButton() const { return D2D1::RectF(W() - kSearchX - kAllW, kSearchTop, W() - kSearchX, kSearchTop + kSearchH); }
D2D1_RECT_F StartMenu::SearchRect() const { return D2D1::RectF(kSearchX, kSearchTop, AllButton().left - 8, kSearchTop + kSearchH); }
float StartMenu::ListBottom() const { return MainRect().bottom - 44; }
float StartMenu::GridX() const { return (W() - float(cols_) * kCellW) / 2; }
float StartMenu::ButtonSize() const { return std::min(44.f, barH_ - 12); }

D2D1_RECT_F StartMenu::Cell(int slot) const
{
    const int per = PerPage(), page = slot / per, k = slot % per;
    const float x = GridX() + float(k % cols_) * kCellW + float(page) * W(), y = kGridTop + float(k / cols_) * kCellH;
    return D2D1::RectF(x, y, x + kCellW, y + kCellH);
}

D2D1_RECT_F StartMenu::FolderRect() const
{
    // Just big enough for its apps (pages of them beyond), centred over the grid.
    const float w = float(FolderCols()) * kCellW + 2 * kFolderPad, h = kFolderHead + float(FolderRows()) * kCellH + kFolderFoot;
    const float cx = W() / 2, cy = (kListTop - 10 + MainRect().bottom - 14) / 2;
    return D2D1::RectF(cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2);
}

int StartMenu::FolderApps() const { return folder_ >= 0 && folder_ < int(tiles_.size()) ? int(tiles_[size_t(folder_)].apps.size()) : 4; }

int StartMenu::FolderCols() const { return std::clamp(FolderApps(), 3, 4); }

int StartMenu::FolderRows() const
{
    const float room = MainRect().bottom - 14 - (kListTop - 10) - kFolderHead - kFolderFoot;
    return std::clamp((FolderApps() + FolderCols() - 1) / FolderCols(), 1, std::max(1, int(room / kCellH)));
}

D2D1_RECT_F StartMenu::IndexRect() const { return D2D1::RectF(10, kListTop + 2, 10 + kIndexW, ListBottom() - 2); }

D2D1_RECT_F StartMenu::FolderCell(int slot) const
{
    const D2D1_RECT_F f = FolderRect();
    const int cols = FolderCols(), per = cols * FolderRows(), page = slot / per, k = slot % per;
    const float x0 = (f.left + f.right - float(cols) * kCellW) / 2;
    const float x = x0 + float(k % cols) * kCellW + float(page) * (f.right - f.left), y = f.top + kFolderHead + float(k / cols) * kCellH;
    return D2D1::RectF(x, y, x + kCellW, y + kCellH);
}

D2D1_RECT_F StartMenu::PowerRect() const
{
    // Right-aligned with the power button, the last in the capsule.
    D2D1_RECT_F shelf = ShelfRect();
    float right = std::min(shelf.right - (barH_ - ButtonSize()) / 2 + 6, W());
    float bottom = shelf.top - 8, h = 4 * kPowerRowH + 2 * kPowerPad;
    return D2D1::RectF(right - kPowerW, bottom - h, right, bottom);
}

float StartMenu::Covered(const D2D1_RECT_F& r, double now) const
{
    // Content under the power card fades as it opens: the card's glass is
    // below the content layer, so the content would show through it.
    if (!powerOpen_)
        return 1;
    D2D1_RECT_F p = PowerRect();
    if (r.right <= p.left || r.left >= p.right || r.bottom <= p.top || r.top >= p.bottom)
        return 1;
    return 1 - motion::EaseOut(float((now - powerMs_) / (kPowerMs * 0.6)));
}

void StartMenu::LoadAccount()
{
    if (!userName_.empty())
        return;
    // Screenshots and screen recordings: ATRIUM_PRIVACY=1 shows neither the
    // account's name and picture nor the network's name.
    wchar_t privacy[8] = {};
    private_ = GetEnvironmentVariableW(L"ATRIUM_PRIVACY", privacy, 8) && privacy[0] == L'1';
    if (private_) {
        userName_ = L"用户"; // 用户
        return;
    }
    wchar_t buf[256] = {};
    ULONG n = 256;
    if (GetUserNameExW(NameDisplay, buf, &n) && buf[0]) {
        userName_ = buf;
    } else {
        DWORD m = 256;
        if (GetUserNameW(buf, &m))
            userName_ = buf;
    }
    // The account picture Windows keeps for the user.
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return;
    BYTE info[256];
    DWORD len = 0;
    LPWSTR sid = nullptr;
    if (GetTokenInformation(token, TokenUser, info, sizeof(info), &len) && ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(info)->User.Sid, &sid)) {
        std::wstring key = std::wstring(L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\AccountPicture\\Users\\") + sid;
        for (const wchar_t* value : {L"Image192", L"Image240", L"Image96", L"Image448"}) {
            wchar_t path[MAX_PATH] = {};
            DWORD size = sizeof(path);
            if (RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), value, RRF_RT_REG_SZ, nullptr, path, &size) == ERROR_SUCCESS &&
                GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES && LoadShellImage(path, 96, avatar_, true))
                break;
        }
        LocalFree(sid);
    }
    CloseHandle(token);
}

// ---- Opening, closing, contents ----

void StartMenu::Open(RECT bar, RECT logo, HWND previous)
{
    previous_ = previous;
    bar_ = bar;
    logo_ = logo;
    query_.clear();
    results_.clear();
    allApps_ = scrubbing_ = false;
    page_ = 0;
    pageShown_ = 0;
    wheelAcc_ = 0;
    scroll_ = scrollTarget_ = 0;
    sel_ = -1;
    hover_.clear();
    pressed_.clear();
    powerOpen_ = false;
    folder_ = -1;
    folderClosing_ = naming_ = false;
    drag_ = {};
    sizing_ = false;
    tilePos_.clear();
    apps_ = catalog_.Apps();
    pins_ = catalog_.StartPins();
    stale_ = false;
    Rebuild();
    catalog_.Refresh(); // new installs and pins since last time; the menu updates when ready
    if (onStatusRequest)
        onStatusRequest();
    LoadAccount();
    // The window has the largest size the work area has room for; the menu its own size, if it fits.
    room_ = 0;
    for (int size = 1; size < StartLayout::kSizes; ++size)
        if (Fits(size))
            room_ = size;
    SetSize(layout_.size);
    const D2D1_SIZE_F z = SizeFor(shown_);
    panelW_ = z.width;
    mainH_ = z.height - kGap - barH_;
    panelV_ = mainV_ = 0;
    RECT r = PopupRect(bar, Size(), PopupAlign::Left);
    Restage();
    // Pops out of the logo.
    POINT anchor{(logo.left + logo.right) / 2, r.bottom};
    if (logo.right <= logo.left)
        anchor.x = r.left + (r.right - r.left) / 6;
    surface_.Show(r, true, anchor);
    SetTimer(surface_.Hwnd(), kStatusTimer, 2000, nullptr);
    Log(L"start: opened (%zu apps, %zu tiles, %s)", apps_.size(), tiles_.size(), catalog_.Loaded() ? L"catalog ready" : L"catalog loading");
}

void StartMenu::Close(bool launched)
{
    if (!surface_.Visible())
        return;
    if (drag_.on || drag_.armed)
        EndDrag(false);
    if (sizing_)
        EndResize();
    FinishNaming(true);
    KillTimer(surface_.Hwnd(), kStatusTimer);
    catalog_.SetVisibleIcons({});
    powerOpen_ = false;
    surface_.Hide();
    lastClosed_ = GetTickCount64();
    // Give the keyboard back to where it was, unless an app was just started
    // (it takes the foreground itself).
    wchar_t cls[64] = {};
    if (!launched && previous_ && IsWindow(previous_)) {
        GetClassNameW(previous_, cls, 64);
        if (!SetForegroundWindow(previous_))
            SwitchToThisWindow(previous_, TRUE);
    }
    Log(L"start: closed%s%s%s", launched ? L" (launched)" : L"", cls[0] ? L", focus back to " : L"", cls);
}

void StartMenu::SetStatus(const SystemStatus& status)
{
    if (!(status_ == status)) {
        status_ = status;
        surface_.Invalidate();
    }
}

void StartMenu::Refresh()
{
    if (!surface_.Visible())
        return;
    // Icons arrive one by one and the apps' windows change all the time: the
    // tiles are rebuilt only when the apps themselves changed.
    auto apps = catalog_.Apps();
    auto pins = catalog_.StartPins();
    auto same = [](const std::vector<CatalogApp>& a, const std::vector<CatalogApp>& b) {
        return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](const CatalogApp& x, const CatalogApp& y) { return x.id == y.id && x.name == y.name; });
    };
    if (!same(apps, apps_) || !same(pins, pins_)) {
        if (drag_.on) {
            stale_ = true; // the carried tile's indices stay valid until it is put down
        } else {
            apps_ = std::move(apps);
            pins_ = std::move(pins);
            Rebuild();
        }
    }
    surface_.Invalidate();
}

void StartMenu::Rebuild()
{
    const std::wstring openKey = folder_ >= 0 && folder_ < int(tiles_.size()) ? tiles_[size_t(folder_)].key : L"";
    std::map<std::wstring, const CatalogApp*> known;
    for (const CatalogApp& a : apps_)
        known.emplace(a.id, &a);
    for (const CatalogApp& a : pins_)
        known.emplace(a.id, &a);
    // An app of the arrangement: the catalog's entry; until the catalog is
    // read, or for a shortcut outside it, what was saved. Uninstalled apps are left out.
    const bool loaded = catalog_.Loaded();
    auto resolve = [&](const StartTile& t, CatalogApp& out) {
        if (auto it = known.find(t.id); it != known.end()) {
            out = *it->second;
            return true;
        }
        if (loaded && !(IsPath(t.id) && GetFileAttributesW(t.id.c_str()) != INVALID_FILE_ATTRIBUTES))
            return false;
        out = CatalogApp{t.name.empty() ? t.id : t.name, t.id, {}, {}, {}};
        return true;
    };
    tiles_.clear();
    if (!layout_.custom) {
        for (const CatalogApp& p : pins_)
            if (!Hidden(p.id))
                tiles_.push_back({p.id, p.name, p, {}, false});
    } else {
        for (const StartTile& t : layout_.tiles) {
            Tile tile{t.id, t.name, {}, {}, t.Folder()};
            if (tile.folder) {
                for (const StartTile& a : t.apps)
                    if (CatalogApp app; resolve(a, app))
                        tile.apps.push_back(std::move(app));
                if (tile.apps.empty())
                    continue;
            } else {
                if (!resolve(t, tile.app))
                    continue;
                tile.name = tile.app.name;
            }
            tiles_.push_back(std::move(tile));
        }
    }
    std::vector<std::wstring> ids;
    for (const Tile& t : tiles_)
        if (!t.folder)
            ids.push_back(t.key);
    catalog_.SetStartIds(std::move(ids));
    folder_ = -1;
    for (size_t i = 0; i < tiles_.size() && !openKey.empty(); ++i)
        if (tiles_[i].key == openKey)
            folder_ = int(i);
    if (folder_ < 0)
        folderClosing_ = naming_ = false;
    if (allApps_)
        list_ = AllApps();
    if (!query_.empty())
        results_ = Results();
    page_ = std::clamp(page_, 0, Pages() - 1);
}

void StartMenu::Restage()
{
    stageMs_ = ClockMs();
    hoverAnim_.clear();
}

std::vector<StartMenu::Row> StartMenu::Results() const
{
    std::vector<Row> out;
    // Folders first (by name or initials), then the apps.
    for (size_t i = 0; i < tiles_.size(); ++i)
        if (tiles_[i].folder && AppCatalog::Matches(CatalogApp{tiles_[i].name}, query_))
            out.push_back({0, int(i), {}});
    std::vector<CatalogApp> apps;
    for (const CatalogApp& a : apps_)
        if (!Hidden(a.id) && AppCatalog::Matches(a, query_))
            apps.push_back(a);
    // Names that start with the query first, then the other matches.
    std::stable_partition(apps.begin(), apps.end(), [&](const CatalogApp& a) {
        return CompareStringEx(LOCALE_NAME_USER_DEFAULT, NORM_IGNORECASE, a.name.c_str(), int(std::min(a.name.size(), query_.size())), query_.c_str(),
                               int(query_.size()), nullptr, nullptr, 0) == CSTR_EQUAL;
    });
    for (CatalogApp& a : apps)
        out.push_back({0, -1, std::move(a)});
    return out;
}

std::vector<StartMenu::Row> StartMenu::AllApps() const
{
    // Every app and folder under its letter (# first, as in Windows), by name within it.
    struct Entry {
        wchar_t letter;
        Row row;
        const std::wstring* name;
    };
    std::vector<Entry> entries;
    for (size_t i = 0; i < tiles_.size(); ++i)
        if (tiles_[i].folder)
            entries.push_back({IndexLetter(tiles_[i].name), {0, int(i), {}}, &tiles_[i].name});
    std::vector<CatalogApp> hidden;
    for (const CatalogApp& a : apps_) {
        if (Hidden(a.id))
            hidden.push_back(a);
        else
            entries.push_back({IndexLetter(a.name), {0, -1, a}, nullptr});
    }
    for (Entry& e : entries)
        if (!e.name)
            e.name = &e.row.app.name;
    std::stable_sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        if (a.letter != b.letter)
            return LetterRank(a.letter) < LetterRank(b.letter);
        return CompareStringEx(LOCALE_NAME_USER_DEFAULT, NORM_IGNORECASE | SORT_DIGITSASNUMBERS, a.name->c_str(), int(a.name->size()), b.name->c_str(),
                               int(b.name->size()), nullptr, nullptr, 0) == CSTR_LESS_THAN;
    });
    std::vector<Row> out;
    wchar_t letter = 0;
    for (Entry& e : entries) {
        if (e.letter != letter) {
            letter = e.letter;
            out.push_back({letter, -1, {}});
        }
        out.push_back(std::move(e.row));
    }
    // The hidden apps last, under a heading of their own.
    if (!hidden.empty()) {
        std::stable_sort(hidden.begin(), hidden.end(), [](const CatalogApp& a, const CatalogApp& b) {
            return CompareStringEx(LOCALE_NAME_USER_DEFAULT, NORM_IGNORECASE | SORT_DIGITSASNUMBERS, a.name.c_str(), int(a.name.size()), b.name.c_str(),
                                   int(b.name.size()), nullptr, nullptr, 0) == CSTR_LESS_THAN;
        });
        out.push_back({kHiddenHead, -1, {}});
        for (CatalogApp& a : hidden)
            out.push_back({0, -1, std::move(a)});
    }
    return out;
}

int StartMenu::TileIconPx(float scale) { return int(std::lround(kIcon * scale * 1.1f)); }

int StartMenu::Pages() const
{
    int n = int(tiles_.size());
    if (drag_.on && !drag_.fromFolder)
        --n; // the carried tile is not in the grid
    if (drag_.on && !drag_.inFolder)
        ++n; // but there is room for it, also on a page after the last full one
    return std::max(1, (n + PerPage() - 1) / PerPage());
}

int StartMenu::FolderPages() const
{
    if (folder_ < 0)
        return 1;
    int n = int(tiles_[size_t(folder_)].apps.size());
    if (drag_.on && drag_.fromFolder && !drag_.inFolder)
        --n;
    const int per = FolderCols() * FolderRows();
    return std::max(1, (n + per - 1) / per);
}

void StartMenu::SetPage(int page)
{
    page = std::clamp(page, 0, Pages() - 1);
    if (page == page_)
        return;
    page_ = page;
    if (sel_ >= 0 && sel_ / PerPage() != page_ && !Listing())
        sel_ = -1;
    surface_.Invalidate();
}

bool StartMenu::Hidden(const std::wstring& id) const
{
    return std::find(layout_.hidden.begin(), layout_.hidden.end(), id) != layout_.hidden.end();
}

void StartMenu::SetHidden(const CatalogApp& app, bool hidden)
{
    if (hidden) {
        // Out of the grid and its folders too.
        Arrange([&](std::vector<StartTile>& tiles) {
            Take(tiles, app.id);
            layout_.hidden.push_back(app.id);
        });
    } else {
        std::erase(layout_.hidden, app.id);
        SaveStartLayout(layout_);
        Rebuild();
        surface_.Invalidate();
    }
    Log(L"start: %s %s", app.name.c_str(), hidden ? L"hidden" : L"shown again");
}

void StartMenu::ShowHidden()
{
    // The A-Z list, at its hidden apps.
    allApps_ = true;
    query_.clear();
    results_.clear();
    list_ = AllApps();
    sel_ = -1;
    float top = 0;
    for (const Row& row : list_) {
        if (row.letter == kHiddenHead)
            break;
        top += row.letter ? kHeadH : kRowH;
    }
    scroll_ = scrollTarget_ = top; // clamped to the list's end when painted
    Restage();
    surface_.Invalidate();
}

bool StartMenu::OnStart(const std::wstring& id) const
{
    for (const Tile& t : tiles_) {
        if (!t.folder && t.key == id)
            return true;
        if (t.folder && std::any_of(t.apps.begin(), t.apps.end(), [&](const CatalogApp& a) { return a.id == id; }))
            return true;
    }
    return false;
}

void StartMenu::Arrange(const std::function<void(std::vector<StartTile>&)>& change)
{
    if (!layout_.custom) {
        // The first change: the Windows pins become the menu's own.
        layout_.tiles.clear();
        for (const Tile& t : tiles_)
            layout_.tiles.push_back({t.key, t.name, {}});
        layout_.custom = true;
        Log(L"start: arranged by the user from now on (%zu Windows pins copied)", layout_.tiles.size());
    }
    change(layout_.tiles);
    layout_.Clamp();
    SaveStartLayout(layout_);
    Rebuild();
    surface_.Invalidate();
}

void StartMenu::PinToStart(const CatalogApp& app)
{
    if (OnStart(app.id))
        return;
    std::erase(layout_.hidden, app.id);
    Arrange([&](std::vector<StartTile>& tiles) { tiles.push_back({app.id, app.name, {}}); });
    Log(L"start: %s pinned to the start menu", app.name.c_str());
}

float StartMenu::Hover(Canvas& c, const std::wstring& id, bool on)
{
    auto& f = hoverAnim_[id];
    float v = f.Step(on, c.now);
    if (f.moving)
        c.Continue();
    return v;
}

// ---- Painting ----

void StartMenu::Paint(Canvas& c)
{
    visibleIcons_.clear();
    const Theme& t = c.theme;
    const double now = c.now;
    hits_.clear();
    dt_ = lastPaint_ > 0 ? float(std::clamp(now - lastPaint_, 0.0, 100.0)) : 16.f;
    lastPaint_ = now;
    auto glide = [&](float& v, float target, float rate, float eps) {
        v = motion::Approach(v, target, dt_, rate);
        if (std::fabs(target - v) < eps)
            v = target;
        else
            c.Continue();
    };
    glide(scroll_, scrollTarget_, 16, 0.3f);
    glide(pageShown_, float(page_), 14, 0.002f);
    glide(folderPageShown_, float(folderPage_), 14, 0.002f);
    // Another size: the panel and the capsule spring to it, from the bottom left corner.
    const D2D1_SIZE_F target = SizeFor(shown_);
    const float targetH = target.height - kGap - barH_;
    bool resizing = false;
    if (panelW_ != target.width || mainH_ != targetH || panelV_ != 0 || mainV_ != 0) {
        kResize.Step(panelW_, panelV_, target.width, dt_ / 1000);
        kResize.Step(mainH_, mainV_, targetH, dt_ / 1000);
        if (std::fabs(panelW_ - target.width) < 0.25f && std::fabs(mainH_ - targetH) < 0.25f && std::fabs(panelV_) < 2 && std::fabs(mainV_) < 2) {
            panelW_ = target.width;
            mainH_ = targetH;
            panelV_ = mainV_ = 0;
        } else {
            resizing = true;
            c.Continue();
        }
    }
    // Only the menu's pieces are on screen and take clicks (with room for the frame still on its way).
    {
        const float winH = float(surface_.Rect().bottom - surface_.Rect().top) / surface_.Scale(), slack = resizing ? 8.f : 0.f;
        const float rw = std::max(W(), target.width) + slack, rh = std::max(H(), target.height) + slack;
        surface_.SetRegion(D2D1::RectF(0, std::max(0.f, winH - rh), rw, winH));
    }
    const float offY = OffsetY();
    c.BeginGroup(1, 1, 0, 0, 0, offY);

    // The pieces of glass: the panel, the capsule (round like the bar) and,
    // while open, the folder's card over the tiles and the power card growing
    // up out of the capsule.
    const D2D1_RECT_F main = MainRect(), shelf = ShelfRect();
    std::vector<ShapeDip> shapes{{main, radius_}, {shelf, (shelf.bottom - shelf.top) / 2}};
    bool animating = false;
    float open = 0;
    D2D1_RECT_F card{};
    if (folder_ >= 0) {
        // The card springs out of the folder's tile and back into it; turning round
        // half-way keeps its speed.
        const float goal = folderClosing_ ? 0.f : 1.f;
        (folderClosing_ ? kFolderClose : kFolderOpen).Step(folderGrow_, folderV_, goal, dt_ / 1000);
        const bool rest = std::fabs(folderGrow_ - goal) < 0.002f && std::fabs(folderV_) < 0.02f;
        if (rest) {
            folderGrow_ = goal;
            folderV_ = 0;
        }
        if (folderClosing_ && (rest || folderGrow_ < 0.01f)) {
            folder_ = -1;
            folderClosing_ = false;
            folderGrow_ = folderV_ = 0;
        } else {
            const float grow = folderGrow_;
            open = std::clamp(grow, 0.f, 1.f);
            card = Mix(folderFrom_, FolderRect(), grow);
            const float cardR = std::max(12.f, radius_ - 14); // concentric with the panel
            shapes.push_back({card, Mix(12.f, cardR, open)});
            if (!rest) {
                c.Continue();
                animating = true;
            }
        }
    }
    if (c.panel) // no glass (yet): plain panels keep the text readable
        for (const ShapeDip& sh : shapes)
            c.Fill(sh.rect, sh.radius, t.panel);

    PaintSearch(c, t);
    // What the folder's card covers stays, blurred and dimmed under it.
    const float under = 1 - 0.45f * open;
    if (open > 0.01f)
        c.BeginBlur();
    if (Listing()) {
        PaintRows(c, t, RowsShown(), under);
        if (Indexed())
            PaintIndex(c, t);
    } else {
        PaintTiles(c, t, under);
    }
    if (open > 0.01f)
        c.EndBlur(6 * open);
    if (folder_ >= 0)
        PaintFolder(c, t, card, open);
    PaintShelf(c, t);
    PaintGrip(c, t);
    if (drag_.on)
        PaintDragged(c, t);
    catalog_.SetVisibleIcons(visibleIcons_);
    if (powerOpen_) {
        float e = float((now - powerMs_) / kPowerMs);
        D2D1_RECT_F pc = PowerRect();
        float grow = 0.55f + 0.45f * motion::EaseOutBack(e, 1.5f);
        pc.top = pc.bottom - (pc.bottom - pc.top) * grow;
        shapes.push_back({pc, 20});
        if (c.panel)
            c.Fill(pc, 20, t.panel);
        PaintPower(c, t);
    }
    c.EndGroup();
    for (ShapeDip& sh : shapes) {
        sh.rect.top += offY;
        sh.rect.bottom += offY;
    }
    surface_.SetShapes(shapes, animating || resizing);
}

// Search: an outlined capsule at the top of the panel; typing anywhere goes
// here. Beside it the way to all the apps, A to Z (and back).
void StartMenu::PaintSearch(Canvas& c, const Theme& t)
{
    D2D1_RECT_F search = SearchRect();
    const float r = kSearchH / 2;
    float sh = Hover(c, L"search", hover_ == L"search" || !query_.empty());
    c.Fill(search, r, Alpha(t.card, 0.6f + 0.4f * sh));
    c.Stroke(search, r, query_.empty() ? t.border : Alpha(t.accent, 0.85f), query_.empty() ? 1.f : 1.5f);
    c.Glyph(L"\uE721", D2D1::RectF(search.left + 10, search.top, search.left + 44, search.bottom), 16, t.subtext);
    D2D1_RECT_F field = D2D1::RectF(search.left + 46, search.top, search.right - 48, search.bottom);
    if (query_.empty())
        c.Text(L"搜索应用和文件夹", field, 15, t.faint);
    else
        c.Text(query_, field, 15, t.text);
    if (!naming_) {
        float caretX = field.left + std::min(c.TextWidth(query_, 15), field.right - field.left - 2) + 1;
        c.Line(caretX, search.top + 12, caretX, search.bottom - 12, Alpha(t.accent, 0.9f), 1.5f);
    }
    hits_.push_back({search, L"search"});
    if (!query_.empty()) {
        D2D1_RECT_F clear = D2D1::RectF(search.right - 40, search.top + 6, search.right - 8, search.bottom - 6);
        float hv = Hover(c, L"clear", hover_ == L"clear");
        c.Fill(clear, 16, Alpha(pressed_ == L"clear" ? t.press : t.hover, 0.5f + 0.5f * hv));
        c.Glyph(L"\uE711", clear, 10, t.text);
        hits_.push_back({clear, L"clear"});
    }

    const D2D1_RECT_F b = AllButton();
    const bool back = allApps_ && query_.empty();
    const std::wstring label = back ? L"返回" : L"全部应用";
    float hv = Hover(c, L"allapps", hover_ == L"allapps");
    c.Fill(b, r, Alpha(pressed_ == L"allapps" ? t.press : t.hover, 0.45f + 0.55f * hv));
    const float tw = c.TextWidth(label, 13), x = (b.left + b.right - (16 + 6 + tw)) / 2;
    c.Glyph(back ? L"\uE72B" : L"\uE71D", D2D1::RectF(x, b.top, x + 16, b.bottom), 14, t.text);
    c.Text(label, D2D1::RectF(x + 22, b.top, b.right - 4, b.bottom), 13, t.text);
    hits_.push_back({b, L"allapps"});
}

void StartMenu::DrawIcon(Canvas& c, const std::wstring& id, int px, D2D1_RECT_F r, float radius)
{
    static const IconImage kNone;
    visibleIcons_.emplace_back(id, px);
    std::wstring cacheKey = L"c|" + id + L"|" + std::to_wstring(px);
    bool cached = c.Cached(cacheKey);
    IconImage fetched;
    if (!cached)
        fetched = catalog_.Icon(id, px);
    const IconImage& icon = cached ? kNone : fetched;
    if (cached || !icon.empty())
        c.Image(cacheKey, icon, r);
    else
        c.Fill(r, radius, c.theme.hover);
}

void StartMenu::PaintTileFace(Canvas& c, const Theme& t, const Tile& tile, D2D1_RECT_F r, const std::wstring& id, bool selected, bool target)
{
    const double now = c.now;
    float hv = Hover(c, id, hover_ == id || selected);
    bool pressed = pressed_ == id && !drag_.on;
    D2D1_RECT_F plate = D2D1::RectF(r.left + 2, r.top + 1, r.right - 2, r.bottom - 1);
    if (hv > 0.001f)
        c.Fill(plate, 16, selected && hover_ != id ? Alpha(t.accent, 0.22f * hv) : Alpha(t.hover, hv));
    if (pressed)
        c.Fill(plate, 16, t.press);
    if (rippleId_ == id && now - rippleMs_ < kRippleMs) {
        c.Ripple(plate, 16, rippleX_, rippleY_, float((now - rippleMs_) / kRippleMs), t.light ? D2D1::ColorF(1, 1, 1, 0.6f) : D2D1::ColorF(1, 1, 1, 0.22f));
        c.Continue();
    }
    // A carried app over it: it opens up to take it (a folder of the two).
    float into = Hover(c, id + L"#into", target);
    float cx = (r.left + r.right) / 2, iy = r.top + 8;
    D2D1_RECT_F ir = D2D1::RectF(cx - kIcon / 2, iy, cx + kIcon / 2, iy + kIcon);
    c.BeginGroup(1, pressed ? 0.94f : 1 + 0.08f * hv + 0.1f * into, cx, iy + kIcon, 0, -2 * hv);
    if (into > 0.01f) {
        D2D1_RECT_F ring = D2D1::RectF(ir.left - 5, ir.top - 5, ir.right + 5, ir.bottom + 5);
        c.Fill(ring, 15, Alpha(t.hover, into));
        c.Stroke(ring, 15, Alpha(t.accent, 0.8f * into), 1.5f);
    }
    if (tile.folder) {
        // A folder: a plate with its first four apps, small.
        c.Fill(ir, 11, t.cardHot);
        const float m = 14, gap = 3, pad = (kIcon - 2 * m - gap) / 2;
        const int px = int(std::lround(m * c.scale * 1.1f));
        for (size_t k = 0; k < std::min<size_t>(4, tile.apps.size()); ++k) {
            float x = ir.left + pad + float(k % 2) * (m + gap), y = ir.top + pad + float(k / 2) * (m + gap);
            DrawIcon(c, tile.apps[k].id, px, D2D1::RectF(x, y, x + m, y + m), 4);
        }
    } else {
        DrawIcon(c, tile.app.id, TileIconPx(c.scale), ir, 10);
    }
    c.EndGroup();
    c.Text(tile.name, D2D1::RectF(r.left + 2, r.top + 52, r.right - 2, r.bottom + 2), 11.5f, t.text, DWRITE_FONT_WEIGHT_NORMAL, 1, true);
}

// The tiles, cols x rows per page; pages slide sideways. Tiles glide to new
// places (a carried tile, another size); `shown` fades them under the folder's card.
void StartMenu::PaintTiles(Canvas& c, const Theme& t, float shown)
{
    const float w = W();
    const double now = c.now;
    const std::vector<int> order = Order();
    if (order.empty()) {
        std::wstring msg = catalog_.Loaded() ? L"还没有固定的应用。右键单击搜索结果或“全部应用”里的应用来固定" : L"正在读取…";
        c.Text(msg, D2D1::RectF(kSearchX, kGridTop + 60, w - kSearchX, kGridTop + 90), 13, Alpha(t.subtext, shown), DWRITE_FONT_WEIGHT_NORMAL, 1);
        return;
    }
    const int per = PerPage();
    c.Clip(D2D1::RectF(0, kListTop - 4, w, std::min(kGridTop + float(rows_) * kCellH, MainRect().bottom - kGridFoot) + 4));
    for (int slot = 0; slot < int(order.size()); ++slot) {
        const int i = order[size_t(slot)];
        if (i < 0)
            continue;
        const Tile& tile = tiles_[size_t(i)];
        const D2D1_RECT_F home = Cell(slot);
        auto& p = tilePos_.try_emplace(tile.key, D2D1::Point2F(home.left, home.top)).first->second;
        for (auto [v, target] : {std::pair{&p.x, home.left}, std::pair{&p.y, home.top}}) {
            *v = motion::Approach(*v, target, dt_, kGlideRate);
            if (std::fabs(target - *v) < 0.3f)
                *v = target;
            else
                c.Continue();
        }
        const float x = p.x - pageShown_ * w, y = p.y;
        if (x + kCellW < 0 || x > w)
            continue;
        D2D1_RECT_F r = D2D1::RectF(x, y, x + kCellW, y + kCellH);
        float e = motion::EaseOut(float((now - stageMs_ - kStaggerMs * std::min(slot % per, 18)) / kStageMs));
        if (e < 1)
            c.Continue();
        float vis = Covered(r, now) * shown;
        if (vis <= 0.01f)
            continue;
        if (vis < 1)
            c.Continue();
        c.BeginGroup(e * vis, 1, (r.left + r.right) / 2, (r.top + r.bottom) / 2, 0, 10 * (1 - e));
        const std::wstring id = L"tile:" + std::to_wstring(i);
        PaintTileFace(c, t, tile, r, id, i == sel_ && folder_ < 0, drag_.on && drag_.merge == i);
        c.EndGroup();
        if (folder_ < 0 && !drag_.on && slot / per == page_)
            hits_.push_back({r, id});
    }
    c.Unclip();
    if (shown > 0.01f)
        PaintDots(c, t, Pages(), pageShown_, MainRect().bottom - 26, L"page:");
}

// Page dots; the current one is drawn out into a short capsule.
void StartMenu::PaintDots(Canvas& c, const Theme& t, int pages, float current, float y, const wchar_t* prefix)
{
    if (pages <= 1)
        return;
    const float dot = 6, gapX = 8;
    float total = 0;
    std::vector<float> widths(static_cast<size_t>(pages));
    for (int p = 0; p < pages; ++p) {
        float on = std::max(0.f, 1 - std::fabs(current - float(p)));
        widths[size_t(p)] = dot + 12 * on;
        total += widths[size_t(p)] + (p ? gapX : 0);
    }
    float x = (W() - total) / 2;
    for (int p = 0; p < pages; ++p) {
        float on = std::max(0.f, 1 - std::fabs(current - float(p)));
        std::wstring id = prefix + std::to_wstring(p);
        float hv = Hover(c, id, hover_ == id);
        D2D1_RECT_F d = D2D1::RectF(x, y - dot / 2, x + widths[size_t(p)], y + dot / 2);
        D2D1_COLOR_F col = t.faint;
        col.a = t.faint.a + (t.text.a - t.faint.a) * std::max(on, 0.5f * hv);
        c.Fill(d, dot / 2, col);
        hits_.push_back({D2D1::RectF(d.left - gapX / 2, y - 10, d.right + gapX / 2, y + 10), id});
        x = d.right + gapX;
    }
}

// Rows: the search results (folders, then apps; the first or the selected one
// opens with Enter), or every app from A to Z under its letter's heading.
void StartMenu::PaintRows(Canvas& c, const Theme& t, const std::vector<Row>& rows, float shown)
{
    const float w = W(), bottom = ListBottom();
    const double now = c.now;
    contentH_ = 0;
    for (const Row& row : rows)
        contentH_ += row.letter ? kHeadH : kRowH;
    const float viewH = bottom - kListTop;
    scrollTarget_ = std::clamp(scrollTarget_, 0.f, std::max(0.f, contentH_ - viewH));
    scroll_ = std::clamp(scroll_, 0.f, std::max(0.f, contentH_ - viewH));
    if (rows.empty()) {
        std::wstring msg = !query_.empty() ? L"没有找到应用" : catalog_.Loaded() ? L"没有应用" : L"正在读取…";
        c.Text(msg, D2D1::RectF(kSearchX, kGridTop + 60, w - kSearchX, kGridTop + 90), 13, Alpha(t.subtext, shown), DWRITE_FONT_WEIGHT_NORMAL, 1);
        return;
    }
    if (shown <= 0.01f)
        return;
    const int rowPx = int(std::lround(kRowIcon * c.scale * 1.1f));
    const bool firstIsDefault = !query_.empty() && sel_ < 0;
    c.Clip(D2D1::RectF(0, kListTop, w, bottom));
    int staged = 0;
    float y = kListTop - scroll_;
    for (size_t i = 0; i < rows.size(); ++i) {
        const Row& row = rows[i];
        const float h = row.letter ? kHeadH : kRowH;
        D2D1_RECT_F r = D2D1::RectF(Indexed() ? IndexRect().right + 6 : 16, y, w - 16, y + h);
        y += h;
        if (r.bottom < kListTop || r.top > bottom)
            continue;
        float e = motion::EaseOut(float((now - stageMs_ - kStaggerMs * std::min(staged++, 8)) / kStageMs));
        if (e < 1)
            c.Continue();
        float visible = Covered(r, now) * shown;
        if (visible <= 0.01f)
            continue;
        if (visible < 1)
            c.Continue();
        c.BeginGroup(e * visible, 1, (r.left + r.right) / 2, (r.top + r.bottom) / 2, 0, 8 * (1 - e));
        if (row.letter) {
            // A letter's heading (or the hidden apps').
            const std::wstring label = row.letter == kHiddenHead ? L"已隐藏" : std::wstring(1, row.letter);
            const float tw = std::max(c.TextWidth(label, 13, DWRITE_FONT_WEIGHT_SEMI_BOLD), 10.f);
            D2D1_RECT_F chip = D2D1::RectF(r.left + 6, r.top + 6, r.left + 6 + tw + 20, r.bottom - 4);
            c.Fill(chip, (chip.bottom - chip.top) / 2, Alpha(t.hover, 0.35f));
            c.Text(label, chip, 13, t.subtext, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
            c.EndGroup();
            continue;
        }
        const std::wstring id = L"row:" + std::to_wstring(i);
        const bool selected = int(i) == sel_ || (firstIsDefault && i == 0);
        float hv = Hover(c, id, hover_ == id || selected);
        D2D1_RECT_F plate = D2D1::RectF(r.left, r.top + 2, r.right, r.bottom - 2);
        const float pr = (plate.bottom - plate.top) / 2;
        if (hv > 0.001f)
            c.Fill(plate, pr, selected && hover_ != id ? Alpha(t.accent, 0.22f * hv) : Alpha(t.hover, hv));
        if (pressed_ == id)
            c.Fill(plate, pr, t.press);
        if (rippleId_ == id && now - rippleMs_ < kRippleMs) {
            c.Ripple(plate, pr, rippleX_, rippleY_, float((now - rippleMs_) / kRippleMs), t.light ? D2D1::ColorF(1, 1, 1, 0.6f) : D2D1::ColorF(1, 1, 1, 0.22f));
            c.Continue();
        }
        float iy = (r.top + r.bottom) / 2 - kRowIcon / 2;
        D2D1_RECT_F ir = D2D1::RectF(r.left + 14, iy, r.left + 14 + kRowIcon, iy + kRowIcon);
        c.BeginGroup(1, pressed_ == id ? 0.94f : 1 + 0.06f * hv, (ir.left + ir.right) / 2, (ir.top + ir.bottom) / 2);
        std::wstring name, note;
        if (row.folder >= 0) {
            const Tile& f = tiles_[size_t(row.folder)];
            c.Fill(ir, 8, t.cardHot);
            const float m = 10, gap = 2, pad = (kRowIcon - 2 * m - gap) / 2;
            const int px = int(std::lround(m * c.scale * 1.1f));
            for (size_t k = 0; k < std::min<size_t>(4, f.apps.size()); ++k) {
                float x = ir.left + pad + float(k % 2) * (m + gap), yy = ir.top + pad + float(k / 2) * (m + gap);
                DrawIcon(c, f.apps[k].id, px, D2D1::RectF(x, yy, x + m, yy + m), 3);
            }
            name = f.name;
            note = std::to_wstring(f.apps.size()) + L" 个应用";
        } else {
            DrawIcon(c, row.app.id, rowPx, ir, 7);
            name = row.app.name;
        }
        c.EndGroup();
        c.Text(name, D2D1::RectF(r.left + 56, r.top, r.right - 100, r.bottom), 14, t.text);
        if (selected && !query_.empty())
            note = L"Enter 打开";
        if (!note.empty())
            c.Text(note, D2D1::RectF(r.right - 110, r.top, r.right - 18, r.bottom), 12, t.faint, DWRITE_FONT_WEIGHT_NORMAL, 2);
        c.EndGroup();
        hits_.push_back({r, id});
    }
    c.Unclip();
    if (contentH_ > viewH + 1) {
        float th = std::max(28.f, viewH * viewH / contentH_);
        float ty = kListTop + (viewH - th) * (scroll_ / (contentH_ - viewH));
        c.Fill(D2D1::RectF(w - 9, ty, w - 5, ty + th), 2, Alpha(t.faint, shown));
    }
}

// The A-Z list's letters down its left side: those with apps under them in
// the ink, the others faint, the one at the top of the list in the accent.
// Pressed or dragged along, the list glides to the letter, and the letter
// shows large beside the pointer. Too little room for all of them: every
// other one is a dot (it can still be picked).
void StartMenu::PaintIndex(Canvas& c, const Theme& t)
{
    std::set<wchar_t> present;
    for (const Row& row : list_)
        if (row.letter)
            present.insert(row.letter);
    const D2D1_RECT_F bar = IndexRect();
    const int n = int(std::size(kLetters)) - 1;
    const float step = (bar.bottom - bar.top) / float(n), cx = (bar.left + bar.right) / 2;
    const bool compact = step < 12;
    const wchar_t current = scrubbing_ ? scrubLetter_ : TopLetter();
    float hv = Hover(c, L"azbar", hover_ == L"azbar" || scrubbing_);
    if (hv > 0.001f)
        c.Fill(bar, kIndexW / 2, Alpha(t.hover, 0.7f * hv));
    for (int i = 0; i < n; ++i) {
        const wchar_t letter = kLetters[i];
        const float y = bar.top + step * (float(i) + 0.5f);
        const D2D1_COLOR_F ink = letter == current ? t.accent : present.contains(letter) ? t.subtext : t.faint;
        if (compact && i % 2 && letter != current) {
            c.Circle(cx, y, 1.4f, ink);
            continue;
        }
        c.Text(std::wstring(1, letter), D2D1::RectF(bar.left, y - 8, bar.right, y + 8), compact ? 9.5f : 10.5f, ink, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
    }
    hits_.push_back({bar, L"azbar"});

    // The letter picked, large, following the pointer.
    const float hint = Hover(c, L"#hint", scrubbing_);
    if (hint <= 0.01f || !scrubLetter_)
        return;
    const float y = std::clamp(scrubY_, kListTop + kHint / 2, ListBottom() - kHint / 2);
    hintY_ = hint < 0.05f ? y : motion::Approach(hintY_, y, dt_, 30);
    if (std::fabs(hintY_ - y) > 0.3f)
        c.Continue();
    const D2D1_RECT_F b = D2D1::RectF(bar.right + 12, hintY_ - kHint / 2, bar.right + 12 + kHint, hintY_ + kHint / 2);
    c.BeginGroup(hint, 0.8f + 0.2f * hint, b.left, (b.top + b.bottom) / 2);
    c.Fill(b, 18, Alpha(t.accent, 0.94f));
    bool halo = c.halo;
    c.halo = false;
    c.Text(std::wstring(1, scrubLetter_), b, 26, t.accentText, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
    c.halo = halo;
    c.EndGroup();
}

wchar_t StartMenu::TopLetter() const
{
    wchar_t letter = 0;
    float y = 0;
    for (const Row& row : list_) {
        if (row.letter) {
            if (y > scroll_ + 1)
                break;
            letter = row.letter;
        }
        y += row.letter ? kHeadH : kRowH;
    }
    return letter;
}

void StartMenu::ScrubTo(float y)
{
    const D2D1_RECT_F bar = IndexRect();
    const int n = int(std::size(kLetters)) - 1;
    const int i = std::clamp(int((y - bar.top) / ((bar.bottom - bar.top) / float(n))), 0, n - 1);
    scrubY_ = std::clamp(y, bar.top, bar.bottom);
    // The letter's heading, or the next one with apps under it (past the last: the last).
    float top = 0, at = -1;
    wchar_t found = 0;
    for (const Row& row : list_) {
        if (row.letter && row.letter != kHiddenHead) {
            if (LetterRank(row.letter) >= LetterRank(kLetters[i]) && !found) {
                found = row.letter;
                at = top;
            }
            if (!found)
                at = top, scrubLetter_ = row.letter;
        }
        top += row.letter ? kHeadH : kRowH;
    }
    if (found)
        scrubLetter_ = found;
    if (at >= 0)
        scrollTarget_ = std::clamp(at, 0.f, std::max(0.f, contentH_ - (ListBottom() - kListTop)));
    surface_.Invalidate();
}

// The open folder: its name (click to rename) and its apps, pages of them.
void StartMenu::PaintFolder(Canvas& c, const Theme& t, D2D1_RECT_F card, float shown)
{
    if (folder_ < 0)
        return;
    const Tile& f = tiles_[size_t(folder_)];
    if (!folderClosing_)
        hits_.push_back({card, L"fcard"});
    const float a = std::clamp((shown - 0.3f) / 0.7f, 0.f, 1.f);
    if (a <= 0.01f)
        return;
    const D2D1_RECT_F full = FolderRect();
    c.BeginGroup(a, 0.94f + 0.06f * a, (full.left + full.right) / 2, (full.top + full.bottom) / 2); // settles in with the card

    // The name; while it is being edited, a field with a caret.
    D2D1_RECT_F title = D2D1::RectF(full.left + 24, full.top + 12, full.right - 24, full.top + 46);
    const std::wstring shownName = naming_ ? nameDraft_ : f.name;
    const float tw = std::min(c.TextWidth(shownName.empty() ? f.name : shownName, 17, DWRITE_FONT_WEIGHT_SEMI_BOLD), title.right - title.left - 40);
    const float cx = (title.left + title.right) / 2;
    D2D1_RECT_F field = D2D1::RectF(cx - tw / 2 - 18, title.top, cx + tw / 2 + 18, title.bottom);
    if (naming_) {
        field = D2D1::RectF(std::max(title.left, cx - std::max(tw / 2 + 18, 90.f)), title.top, std::min(title.right, cx + std::max(tw / 2 + 18, 90.f)), title.bottom);
        c.Fill(field, 17, Alpha(t.card, 1));
        c.Stroke(field, 17, Alpha(t.accent, 0.85f), 1.5f);
        if (nameDraft_.empty())
            c.Text(f.name, title, 17, t.faint, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
        else
            c.Text(nameDraft_, title, 17, t.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
        const float caretX = nameDraft_.empty() ? cx - tw / 2 : cx + tw / 2 + 1;
        c.Line(caretX, title.top + 8, caretX, title.bottom - 8, Alpha(t.accent, 0.9f), 1.5f);
    } else {
        float hv = Hover(c, L"ftitle", hover_ == L"ftitle");
        if (hv > 0.001f)
            c.Fill(field, 17, Alpha(t.hover, hv));
        c.Text(f.name, title, 17, t.text, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
    }
    hits_.push_back({field, L"ftitle"});

    // The apps.
    const std::vector<int> order = FolderOrder();
    const int per = FolderCols() * FolderRows();
    const float pageW = full.right - full.left;
    c.Clip(D2D1::RectF(full.left + 2, full.top + kFolderHead - 6, full.right - 2, full.bottom - kFolderFoot + 6));
    for (int slot = 0; slot < int(order.size()); ++slot) {
        const int i = order[size_t(slot)];
        if (i < 0)
            continue;
        const CatalogApp& app = f.apps[size_t(i)];
        const D2D1_RECT_F home = FolderCell(slot);
        auto& p = tilePos_.try_emplace(L"f|" + app.id, D2D1::Point2F(home.left, home.top)).first->second;
        for (auto [v, target] : {std::pair{&p.x, home.left}, std::pair{&p.y, home.top}}) {
            *v = motion::Approach(*v, target, dt_, kGlideRate);
            if (std::fabs(target - *v) < 0.3f)
                *v = target;
            else
                c.Continue();
        }
        const float x = p.x - folderPageShown_ * pageW;
        if (x + kCellW < full.left || x > full.right)
            continue;
        D2D1_RECT_F r = D2D1::RectF(x, p.y, x + kCellW, p.y + kCellH);
        const std::wstring id = L"fapp:" + std::to_wstring(i);
        // Opening, the apps pop up one after another.
        const float e = folderClosing_ ? 1.f : float((c.now - folderMs_ - 70 - kFolderAppStaggerMs * std::min(slot % per, 12)) / kFolderAppMs);
        if (e < 1)
            c.Continue();
        c.BeginGroup(motion::EaseOut(e * 1.4f), 0.6f + 0.4f * motion::EaseOutBack(e, 1.6f), (r.left + r.right) / 2, r.top + 8 + kIcon / 2);
        PaintTileFace(c, t, Tile{app.id, app.name, app, {}, false}, r, id, i == sel_, false);
        c.EndGroup();
        if (!drag_.on && !folderClosing_ && slot / per == folderPage_)
            hits_.push_back({r, id});
    }
    c.Unclip();
    const int pages = FolderPages();
    if (pages > 1) {
        // The dots under the card's apps (centred on the panel, as the card is).
        PaintDots(c, t, pages, folderPageShown_, full.bottom - 16, L"fpage:");
    }
    c.EndGroup();
}

// The carried tile, over everything, a little larger.
void StartMenu::PaintDragged(Canvas& c, const Theme& t)
{
    Tile tile;
    if (drag_.fromFolder) {
        if (drag_.folderTile < 0 || drag_.folderTile >= int(tiles_.size()) || drag_.from >= int(tiles_[size_t(drag_.folderTile)].apps.size()))
            return;
        const CatalogApp& app = tiles_[size_t(drag_.folderTile)].apps[size_t(drag_.from)];
        tile = {app.id, app.name, app, {}, false};
    } else {
        if (drag_.from < 0 || drag_.from >= int(tiles_.size()))
            return;
        tile = tiles_[size_t(drag_.from)];
    }
    D2D1_RECT_F r = D2D1::RectF(drag_.x - drag_.grabX, drag_.y - drag_.grabY, drag_.x - drag_.grabX + kCellW, drag_.y - drag_.grabY + kCellH);
    c.BeginGroup(0.95f, 1.08f, (r.left + r.right) / 2, r.top + 8 + kIcon / 2);
    PaintTileFace(c, t, tile, r, L"#carried", false, false);
    c.EndGroup();
}

// The grip that sizes the grid: an arc in the panel's top right corner,
// concentric with it (as the widgets' grips), faint while the pointer is on
// the menu and clear on the grip itself.
void StartMenu::PaintGrip(Canvas& c, const Theme& t)
{
    const D2D1_RECT_F m = MainRect();
    const float inMenu = Hover(c, L"#menu", tracking_ || sizing_), on = Hover(c, L"resize", hover_ == L"resize" || sizing_);
    const float a = std::max(0.4f * inMenu, on);
    hits_.push_back({D2D1::RectF(m.right - kGrip, 0, m.right, kGrip), L"resize"});
    if (a <= 0.01f)
        return;
    const float r = std::max(radius_, 18.f);
    c.Arc(m.right - r, r, r - 6, -80.f, -10.f, Alpha(t.subtext, a), 3.5f);
}

// The capsule: the account on the left; network, volume, battery and
// power on the right, round buttons like the bar's.
void StartMenu::PaintShelf(Canvas& c, const Theme& t)
{
    const D2D1_RECT_F shelf = ShelfRect();
    const float cy = (shelf.top + shelf.bottom) / 2, bh = ButtonSize();
    const float edge = (shelf.bottom - shelf.top - bh) / 2; // concentric with the capsule

    // Right side, built from the right edge.
    float x = shelf.right - edge;
    auto button = [&](const std::wstring& id, const std::wstring& glyph, const std::wstring& label, bool dim) {
        float tw = label.empty() ? 0 : c.TextWidth(label, 12) + 4;
        float bw = label.empty() ? bh : 12 + 20 + 4 + tw + 12;
        D2D1_RECT_F b = D2D1::RectF(x - bw, cy - bh / 2, x, cy + bh / 2);
        float hv = Hover(c, id, hover_ == id || (id == L"power" && powerOpen_));
        if (hv > 0)
            c.Fill(b, bh / 2, Alpha(pressed_ == id ? t.press : t.hover, hv));
        D2D1_RECT_F g = label.empty() ? b : D2D1::RectF(b.left + 10, b.top, b.left + 34, b.bottom);
        if (id == L"network" && status_.connected && status_.wifi)
            c.WifiFan((g.left + g.right) / 2, cy, 12, WifiSteps(status_.signal), t.text);
        else
            c.Glyph(glyph, g, 16, dim ? t.faint : t.text);
        if (!label.empty())
            c.Text(label, D2D1::RectF(b.left + 36, b.top, b.right - 8, b.bottom), 12, dim ? t.faint : t.subtext);
        hits_.push_back({b, id});
        x = b.left - 2;
    };
    button(L"power", L"\uE7E8", L"", false);
    if (status_.battery) {
        int level = std::clamp(status_.batteryPercent / 10, 0, 10);
        std::wstring glyph(1, wchar_t((status_.charging ? 0xEBAB : 0xEBA0) + level));
        button(L"battery", glyph, std::to_wstring(status_.batteryPercent) + L"%", false);
    }
    if (status_.audio)
        button(L"volume", status_.muted ? L"\uE74F" : status_.volume < 34 ? L"\uE993" : status_.volume < 67 ? L"\uE994" : L"\uE995", L"", status_.muted);
    button(L"network", !status_.connected ? L"\uEB55" : status_.wifi ? L"\uE701" : L"\uE839", L"", !status_.connected);

    // Account, in what is left.
    std::wstring name = userName_.empty() ? L"用户" : userName_;
    const float left = shelf.left + edge;
    float nw = std::clamp(c.TextWidth(name, 14, DWRITE_FONT_WEIGHT_SEMI_BOLD), 0.f, std::max(0.f, x - left - 70));
    D2D1_RECT_F acct = D2D1::RectF(left, cy - bh / 2, left + 8 + 28 + 10 + nw + 16, cy + bh / 2);
    float ah = Hover(c, L"account", hover_ == L"account");
    if (ah > 0)
        c.Fill(acct, bh / 2, Alpha(pressed_ == L"account" ? t.press : t.hover, ah));
    D2D1_RECT_F pic = D2D1::RectF(acct.left + 8, cy - 14, acct.left + 36, cy + 14);
    if (!avatar_.empty()) {
        c.ImageCircle(L"avatar", avatar_, pic);
    } else {
        c.Circle((pic.left + pic.right) / 2, cy, 14, Alpha(t.accent, 0.9f));
        bool halo = c.halo;
        c.halo = false;
        c.Text(name.substr(0, 1), pic, 13, t.accentText, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
        c.halo = halo;
    }
    c.Text(name, D2D1::RectF(pic.right + 10, shelf.top, pic.right + 10 + nw + 2, shelf.bottom), 14, t.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    hits_.push_back({acct, L"account"});
}

// Power choices, in their own piece of glass above the power button.
void StartMenu::PaintPower(Canvas& c, const Theme& t)
{
    D2D1_RECT_F card = PowerRect();
    float e = float((c.now - powerMs_) / kPowerMs);
    if (e < 1)
        c.Continue();
    c.BeginGroup(motion::EaseOut(e * 1.3f), 0.92f + 0.08f * motion::EaseOutBack(e, 1.5f), card.right - 22, card.bottom);
    static const wchar_t* const kGlyphs[] = {L"\uE708", L"\uE72C", L"\uE7E8", L"\uE72E"};
    static const wchar_t* const kNames[] = {L"睡眠", L"重启", L"关机", L"锁定"};
    for (int i = 0; i < 4; ++i) {
        D2D1_RECT_F row = D2D1::RectF(card.left + 8, card.top + kPowerPad + kPowerRowH * float(i), card.right - 8,
                                      card.top + kPowerPad + kPowerRowH * float(i + 1));
        std::wstring id = L"pw:" + std::to_wstring(i);
        float hv = Hover(c, id, hover_ == id || powerSel_ == i);
        if (hv > 0)
            c.Fill(row, 14, powerSel_ == i && hover_ != id ? Alpha(t.accent, 0.24f * hv) : Alpha(t.hover, hv));
        c.Glyph(kGlyphs[i], D2D1::RectF(row.left + 6, row.top, row.left + 38, row.bottom), 15, t.text);
        c.Text(kNames[i], D2D1::RectF(row.left + 44, row.top, row.right - 8, row.bottom), 14, t.text);
        hits_.push_back({row, id});
    }
    c.EndGroup();
}

std::wstring StartMenu::HitAt(POINT pt) const
{
    float s = surface_.Scale();
    float x = float(pt.x) / s, y = float(pt.y) / s;
    // Last painted is on top (the power card over the panel).
    for (auto it = hits_.rbegin(); it != hits_.rend(); ++it) {
        const Hit& h = *it;
        if (!In(h.rect, x, y))
            continue;
        // Rows scrolled out of the list are not clickable.
        if ((Starts(h.id, L"row:") || Starts(h.id, L"head:")) && (y < kListTop || y > ListBottom()))
            continue;
        return h.id;
    }
    return {};
}

// ---- Folders ----

void StartMenu::OpenFolder(int tile)
{
    if (tile < 0 || tile >= int(tiles_.size()) || !tiles_[size_t(tile)].folder)
        return;
    // The card grows out of the folder's icon when the grid shows it, else out of the panel's middle.
    const D2D1_RECT_F card = FolderRect();
    const float mx = (card.left + card.right) / 2, my = (card.top + card.bottom) / 2;
    folderFrom_ = D2D1::RectF(mx - kIcon / 2, my - kIcon / 2, mx + kIcon / 2, my + kIcon / 2);
    const std::vector<int> order = Order();
    if (auto it = std::find(order.begin(), order.end(), tile); it != order.end() && !Listing()) {
        D2D1_RECT_F cell = Cell(int(it - order.begin()));
        const float x = cell.left - float(page_) * W(), cx = x + kCellW / 2;
        if (x >= 0 && x + kCellW <= W())
            folderFrom_ = D2D1::RectF(cx - kIcon / 2, cell.top + 8, cx + kIcon / 2, cell.top + 8 + kIcon);
    }
    if (folder_ != tile)
        folderGrow_ = folderV_ = 0;
    folder_ = tile;
    folderClosing_ = false;
    folderMs_ = ClockMs();
    folderPage_ = 0;
    folderPageShown_ = 0;
    naming_ = false;
    sel_ = -1;
    std::erase_if(tilePos_, [](const auto& p) { return Starts(p.first, L"f|"); });
    surface_.Invalidate();
    Log(L"start: folder '%s' opened (%zu apps)", tiles_[size_t(tile)].name.c_str(), tiles_[size_t(tile)].apps.size());
}

void StartMenu::CloseFolder()
{
    if (folder_ < 0 || folderClosing_)
        return;
    FinishNaming(true);
    if (folder_ < 0)
        return;
    // Back into its tile (which may have moved meanwhile).
    const std::vector<int> order = Order();
    if (auto it = std::find(order.begin(), order.end(), folder_); it != order.end() && !Listing()) {
        D2D1_RECT_F cell = Cell(int(it - order.begin()));
        const float x = cell.left - float(page_) * W(), cx = x + kCellW / 2;
        if (x >= 0 && x + kCellW <= W())
            folderFrom_ = D2D1::RectF(cx - kIcon / 2, cell.top + 8, cx + kIcon / 2, cell.top + 8 + kIcon);
    }
    folderClosing_ = true;
    sel_ = -1;
    surface_.Invalidate();
}

void StartMenu::StartNaming()
{
    if (folder_ < 0 || folderClosing_)
        return;
    naming_ = true;
    nameDraft_ = tiles_[size_t(folder_)].name;
    surface_.Invalidate();
}

// keep: the new name is taken (an empty one keeps the old).
void StartMenu::FinishNaming(bool keep)
{
    if (!naming_)
        return;
    naming_ = false;
    std::wstring name = nameDraft_;
    while (!name.empty() && iswspace(name.back()))
        name.pop_back();
    while (!name.empty() && iswspace(name.front()))
        name.erase(name.begin());
    if (keep && folder_ >= 0 && !name.empty() && name != tiles_[size_t(folder_)].name) {
        const std::wstring key = tiles_[size_t(folder_)].key;
        Arrange([&](std::vector<StartTile>& tiles) {
            if (StartTile* f = Find(tiles, key))
                f->name = name;
        });
        Log(L"start: folder renamed to '%s'", name.c_str());
    }
    surface_.Invalidate();
}

// ---- Carrying tiles ----

std::vector<int> StartMenu::Order() const
{
    std::vector<int> order;
    for (int i = 0; i < int(tiles_.size()); ++i)
        if (!(drag_.on && !drag_.fromFolder && i == drag_.from))
            order.push_back(i);
    if (drag_.on && !drag_.inFolder)
        order.insert(order.begin() + std::clamp(drag_.insert, 0, int(order.size())), -1);
    return order;
}

std::vector<int> StartMenu::FolderOrder() const
{
    std::vector<int> order;
    if (folder_ < 0)
        return order;
    const bool carried = drag_.on && drag_.fromFolder && drag_.folderTile == folder_;
    for (int i = 0; i < int(tiles_[size_t(folder_)].apps.size()); ++i)
        if (!(carried && i == drag_.from))
            order.push_back(i);
    if (carried && drag_.inFolder)
        order.insert(order.begin() + std::clamp(drag_.insert, 0, int(order.size())), -1);
    return order;
}

void StartMenu::BeginDrag()
{
    const float s = surface_.Scale(), px = float(pressPt_.x) / s, py = float(pressPt_.y) / s;
    Drag d;
    d.on = true;
    D2D1_RECT_F cell{};
    if (Starts(pressed_, L"tile:")) {
        d.from = Index(pressed_, 5);
        if (d.from < 0 || d.from >= int(tiles_.size()))
            return;
        d.key = tiles_[size_t(d.from)].key;
        const std::vector<int> order = Order();
        const int slot = int(std::find(order.begin(), order.end(), d.from) - order.begin());
        d.insert = slot;
        cell = Cell(slot);
        cell.left -= float(page_) * W();
    } else {
        d.from = Index(pressed_, 5);
        if (folder_ < 0 || d.from < 0 || d.from >= int(tiles_[size_t(folder_)].apps.size()))
            return;
        d.fromFolder = d.inFolder = true;
        d.folderTile = folder_;
        d.key = L"f|" + tiles_[size_t(folder_)].apps[size_t(d.from)].id;
        d.insert = d.from;
        cell = FolderCell(d.from);
        cell.left -= float(folderPage_) * (FolderRect().right - FolderRect().left);
    }
    d.x = px;
    d.y = py;
    d.grabX = px - cell.left;
    d.grabY = py - cell.top;
    drag_ = d;
    pressed_.clear();
    hover_.clear();
    sel_ = -1;
    SetTimer(surface_.Hwnd(), kDragTimer, 50, nullptr);
    surface_.Invalidate();
}

void StartMenu::UpdateDrag(POINT client)
{
    const float s = surface_.Scale();
    drag_.x = float(client.x) / s;
    drag_.y = float(client.y) / s;
    const double now = ClockMs();
    if (drag_.inFolder) {
        // Carried off the folder's card: the folder closes, and the app goes onto the grid.
        const D2D1_RECT_F card = FolderRect();
        if (!In(D2D1::RectF(card.left - 12, card.top - 12, card.right + 12, card.bottom + 12), drag_.x, drag_.y)) {
            drag_.inFolder = false;
            drag_.insert = int(tiles_.size());
            drag_.pendingSlot = -1;
            CloseFolder();
        }
    }
    // The slot under the pointer, and whether it is over the icon there.
    int cols, rows, page;
    float x0, top, pageW;
    if (drag_.inFolder) {
        const D2D1_RECT_F card = FolderRect();
        cols = FolderCols();
        rows = FolderRows();
        x0 = (card.left + card.right - float(cols) * kCellW) / 2;
        top = card.top + kFolderHead;
        pageW = card.right - card.left;
        page = folderPage_;
    } else {
        cols = cols_;
        rows = rows_;
        x0 = GridX();
        top = kGridTop;
        pageW = W();
        page = page_;
    }
    (void)pageW;
    const float lx = drag_.x - x0, ly = drag_.y - top;
    const int col = std::clamp(int(std::floor(lx / kCellW)), 0, cols - 1), row = std::clamp(int(std::floor(ly / kCellH)), 0, rows - 1);
    const int slot = page * cols * rows + row * cols + col;
    const std::vector<int> order = drag_.inFolder ? FolderOrder() : Order();
    const float cx = x0 + float(col) * kCellW + kCellW / 2, iy = top + float(row) * kCellH + 8;
    const bool overIcon = std::fabs(drag_.x - cx) < kIcon * 0.55f && drag_.y > iy - 6 && drag_.y < iy + kIcon + 6;
    const int occupant = slot < int(order.size()) ? order[size_t(slot)] : -1;
    // Onto another tile: a folder of the two (or into that folder). Folders do not go into folders.
    const bool carriesFolder = !drag_.fromFolder && tiles_[size_t(drag_.from)].folder;
    const bool merge = overIcon && occupant >= 0 && !drag_.inFolder && !carriesFolder;
    drag_.merge = merge ? occupant : -1;
    if (merge) {
        drag_.pendingSlot = -1;
    } else {
        // Elsewhere: the others make room there once the pointer rests (passing over
        // tiles does not shuffle them). Not past the last tile.
        const int items = int(order.size()) - int(std::count(order.begin(), order.end(), -1));
        const int target = std::min(slot, items);
        if (target == drag_.insert)
            drag_.pendingSlot = -1;
        else if (target != drag_.pendingSlot) {
            drag_.pendingSlot = target;
            drag_.pendingMs = now;
        }
    }
    // Resting beside the grid turns the page.
    const bool level = drag_.y > top - 10 && drag_.y < top + float(rows) * kCellH;
    const int edge = !level ? 0 : drag_.x < x0 - 4 ? -1 : drag_.x > x0 + float(cols) * kCellW + 4 ? 1 : 0;
    if (edge != drag_.edge) {
        drag_.edge = edge;
        drag_.edgeMs = now;
    }
    surface_.Invalidate();
}

void StartMenu::DragTick()
{
    if (!drag_.on)
        return;
    const double now = ClockMs();
    bool changed = false;
    if (drag_.pendingSlot >= 0 && now - drag_.pendingMs >= kRestMs) {
        drag_.insert = drag_.pendingSlot;
        drag_.pendingSlot = -1;
        changed = true;
    }
    if (drag_.edge && now - drag_.edgeMs >= kEdgeMs) {
        drag_.edgeMs = now;
        if (drag_.inFolder) {
            const int next = std::clamp(folderPage_ + drag_.edge, 0, FolderPages() - 1);
            changed |= next != folderPage_;
            folderPage_ = next;
        } else {
            const int next = std::clamp(page_ + drag_.edge, 0, Pages() - 1);
            changed |= next != page_;
            page_ = next;
        }
        // On the new page, the slot under the pointer.
        const float s = surface_.Scale();
        UpdateDrag(POINT{LONG(drag_.x * s), LONG(drag_.y * s)});
    }
    if (changed)
        surface_.Invalidate();
}

void StartMenu::EndDrag(bool commit)
{
    KillTimer(surface_.Hwnd(), kDragTimer);
    const Drag d = drag_;
    // What it means, in keys, before anything moves.
    std::vector<std::wstring> keys;
    std::wstring appId, targetKey, folderKey;
    bool targetIsFolder = false;
    if (d.on && commit) {
        if (d.fromFolder) {
            folderKey = tiles_[size_t(d.folderTile)].key;
            appId = tiles_[size_t(d.folderTile)].apps[size_t(d.from)].id;
        } else {
            appId = tiles_[size_t(d.from)].key;
        }
        if (d.merge >= 0) {
            targetKey = tiles_[size_t(d.merge)].key;
            targetIsFolder = tiles_[size_t(d.merge)].folder;
        } else if (d.inFolder) {
            for (int i : FolderOrder())
                keys.push_back(i < 0 ? appId : tiles_[size_t(d.folderTile)].apps[size_t(i)].id);
        } else {
            for (int i : Order())
                keys.push_back(i < 0 ? appId : tiles_[size_t(i)].key);
        }
    }
    drag_ = {};
    if (GetCapture() == surface_.Hwnd())
        ReleaseCapture();
    if (stale_) {
        stale_ = false;
        apps_ = catalog_.Apps();
        pins_ = catalog_.StartPins();
        Rebuild();
    }
    if (!d.on || !commit) {
        surface_.Invalidate();
        return;
    }
    // The tile glides from where it was let go into its place.
    const float dropX = d.x - d.grabX, dropY = d.y - d.grabY;
    if (d.inFolder)
        tilePos_[d.key] = D2D1::Point2F(dropX + float(folderPage_) * (FolderRect().right - FolderRect().left), dropY);
    else
        tilePos_[d.fromFolder ? appId : d.key] = D2D1::Point2F(dropX + float(page_) * W(), dropY);

    if (!targetKey.empty()) {
        std::wstring made;
        Arrange([&](std::vector<StartTile>& tiles) {
            std::optional<StartTile> moved = Take(tiles, appId);
            if (!moved)
                return;
            StartTile* target = Find(tiles, targetKey);
            if (!target)
                tiles.push_back(std::move(*moved));
            else if (target->Folder())
                target->apps.push_back(std::move(*moved));
            else {
                StartTile folder{layout_.NewFolderId(), L"新建文件夹", {std::move(*target), std::move(*moved)}};
                made = folder.id;
                *target = std::move(folder);
            }
        });
        Log(L"start: %s put into %s%s", appId.c_str(), targetIsFolder ? L"folder " : L"a new folder with ", targetKey.c_str());
        // A new folder opens with its name ready to be typed over.
        for (size_t i = 0; i < tiles_.size() && !made.empty(); ++i)
            if (tiles_[i].key == made) {
                OpenFolder(int(i));
                StartNaming();
                nameDraft_.clear();
            }
        return;
    }
    if (d.inFolder) {
        Arrange([&](std::vector<StartTile>& tiles) {
            if (StartTile* f = Find(tiles, folderKey)) {
                std::vector<StartTile> apps;
                for (const std::wstring& key : keys)
                    if (auto it = std::find_if(f->apps.begin(), f->apps.end(), [&](const StartTile& a) { return a.id == key; }); it != f->apps.end())
                        apps.push_back(*it);
                if (apps.size() == f->apps.size())
                    f->apps = std::move(apps);
            }
        });
        return;
    }
    Arrange([&](std::vector<StartTile>& tiles) {
        if (d.fromFolder) // out of its folder, onto the grid
            if (std::optional<StartTile> moved = Take(tiles, appId))
                tiles.push_back(std::move(*moved));
        Reorder(tiles, keys);
    });
    Log(L"start: %s moved%s", appId.c_str(), d.fromFolder ? L" out of its folder" : L"");
}

// ---- Resizing ----

void StartMenu::Resize(POINT screen)
{
    // The size nearest to where the corner is pulled (of those the screen has room for).
    const float s = surface_.Scale();
    const D2D1_SIZE_F from = SizeFor(sizeStart_);
    const float w = from.width + float(screen.x - sizeFrom_.x) / s, h = from.height + float(sizeFrom_.y - screen.y) / s;
    int best = shown_;
    float nearest = 1e9f;
    for (int size = 0; size <= room_; ++size) {
        const D2D1_SIZE_F z = SizeFor(size);
        if (float d = std::hypot(z.width - w, z.height - h); d < nearest) {
            nearest = d;
            best = size;
        }
    }
    if (best != shown_)
        SetSize(best);
}

void StartMenu::EndResize()
{
    if (!sizing_)
        return;
    sizing_ = false;
    if (GetCapture() == surface_.Hwnd())
        ReleaseCapture();
    if (shown_ != sizeStart_) {
        layout_.size = shown_;
        SaveStartLayout(layout_);
        Log(L"start: sized to %d x %d tiles", cols_, rows_);
    }
    surface_.Invalidate();
}

// ---- Actions ----

void StartMenu::Launch(const CatalogApp& app)
{
    AppCatalog::Launch(app);
    Close(true);
}

void StartMenu::RunPower(int index)
{
    if (index < 0 || index > 3)
        return;
    Close(true);
    if (index == 3) { // Windows' own lock
        if (onSystem)
            onSystem(L"lock");
        return;
    }
    RunPowerAction(index == 0 ? PowerAction::Sleep : index == 1 ? PowerAction::Restart : PowerAction::Shutdown);
}

void StartMenu::Activate(const std::wstring& id)
{
    if (Starts(id, L"tile:")) {
        const int i = Index(id, 5);
        if (i >= 0 && i < int(tiles_.size())) {
            if (tiles_[size_t(i)].folder)
                OpenFolder(i);
            else
                Launch(tiles_[size_t(i)].app);
        }
    } else if (Starts(id, L"fapp:")) {
        const int i = Index(id, 5);
        if (folder_ >= 0 && i >= 0 && i < int(tiles_[size_t(folder_)].apps.size()))
            Launch(tiles_[size_t(folder_)].apps[size_t(i)]);
    } else if (Starts(id, L"row:")) {
        const auto& rows = RowsShown();
        const size_t i = size_t(Index(id, 4));
        if (i < rows.size() && !rows[i].letter) {
            if (rows[i].folder >= 0)
                OpenFolder(rows[i].folder);
            else
                Launch(rows[i].app);
        }
    } else if (id == L"allapps") {
        allApps_ = query_.empty() ? !allApps_ : true;
        query_.clear();
        results_.clear();
        if (allApps_)
            list_ = AllApps();
        sel_ = -1;
        scroll_ = scrollTarget_ = 0;
        Restage();
        surface_.Invalidate();
    } else if (Starts(id, L"page:")) {
        SetPage(Index(id, 5));
    } else if (Starts(id, L"fpage:")) {
        folderPage_ = std::clamp(Index(id, 6), 0, FolderPages() - 1);
        surface_.Invalidate();
    } else if (id == L"ftitle") {
        StartNaming();
    } else if (id == L"fcard" || id == L"resize" || id == L"azbar") {
        // The folder's card itself, the grip: nothing to do on a click.
    } else if (id == L"clear") {
        query_.clear();
        results_.clear();
        sel_ = -1;
        Restage();
        surface_.Invalidate();
    } else if (id == L"power") {
        powerOpen_ = !powerOpen_;
        powerSel_ = -1;
        powerMs_ = ClockMs();
        surface_.Invalidate();
    } else if (Starts(id, L"pw:")) {
        RunPower(id[3] - L'0');
    } else if (id == L"search") {
        // The field already has the keyboard.
    } else if (!id.empty() && onSystem) {
        Close(true);
        onSystem(id);
    }
}

void StartMenu::ContextMenu(const std::wstring& id, POINT screen)
{
    if (!menu)
        return;
    std::vector<MenuEntry> entries;
    CatalogApp app;
    bool isApp = false, inFolder = false;
    std::wstring folderKey; // the folder the menu is for
    if (Starts(id, L"tile:")) {
        const int i = Index(id, 5);
        if (i < 0 || i >= int(tiles_.size()))
            return;
        if (tiles_[size_t(i)].folder)
            folderKey = tiles_[size_t(i)].key;
        else
            app = tiles_[size_t(i)].app, isApp = true;
    } else if (Starts(id, L"fapp:")) {
        const int i = Index(id, 5);
        if (folder_ < 0 || i < 0 || i >= int(tiles_[size_t(folder_)].apps.size()))
            return;
        app = tiles_[size_t(folder_)].apps[size_t(i)];
        isApp = inFolder = true;
    } else if (Starts(id, L"row:")) {
        const auto& rows = RowsShown();
        const size_t i = size_t(Index(id, 4));
        if (i >= rows.size() || rows[i].letter)
            return;
        if (rows[i].folder >= 0)
            folderKey = tiles_[size_t(rows[i].folder)].key;
        else
            app = rows[i].app, isApp = true;
    }
    if (isApp) {
        entries.push_back({L"打开", kOpen});
        entries.push_back({L"固定到任务栏", kPinBar});
        if (!OnStart(app.id))
            entries.push_back({L"固定到开始菜单", kPinStart});
        if (inFolder)
            entries.push_back({L"移出文件夹", kOutOfFolder});
        if (OnStart(app.id))
            entries.push_back({L"从开始菜单移除", kRemove});
        entries.push_back(Hidden(app.id) ? MenuEntry{L"取消隐藏", kUnhide} : MenuEntry{L"隐藏", kHide});
    } else if (!folderKey.empty()) {
        entries = {{L"打开", kOpen}, {L"重命名", kRename}, {L"解散文件夹", kUngroup}};
    } else if (id.empty() && !Listing() && folder_ < 0) {
        static const wchar_t* const kNames[] = {L"小", L"中", L"大"};
        for (int size = 0; size <= room_; ++size)
            entries.push_back({kNames[size], kSize + size, size == shown_});
        if (!layout_.hidden.empty()) {
            entries.push_back({});
            entries.push_back({L"显示隐藏的应用", kShowHidden});
        }
        if (layout_.custom) {
            entries.push_back({});
            entries.push_back({L"恢复为 Windows 开始菜单的固定项", kReset});
        }
    }
    if (entries.empty())
        return;
    const std::wstring openKey = folder_ >= 0 ? tiles_[size_t(folder_)].key : L"";
    menuOpen_ = true;
    menu->Open(std::move(entries), screen, MenuPlace::Below,
               [this, app, folderKey, openKey](int cmd) {
                   auto tileOf = [&](const std::wstring& key) {
                       for (size_t i = 0; i < tiles_.size(); ++i)
                           if (tiles_[i].key == key)
                               return int(i);
                       return -1;
                   };
                   switch (cmd) {
                   case kOpen:
                       if (!folderKey.empty())
                           OpenFolder(tileOf(folderKey));
                       else
                           Launch(app);
                       break;
                   case kPinBar:
                       if (onPin)
                           onPin(AppCatalog::PinFor(app));
                       break;
                   case kPinStart:
                       PinToStart(app);
                       break;
                   case kRemove:
                       Arrange([&](std::vector<StartTile>& tiles) { Take(tiles, app.id); });
                       Log(L"start: %s removed from the start menu", app.name.c_str());
                       break;
                   case kShowHidden:
                       ShowHidden();
                       break;
                   case kHide:
                       SetHidden(app, true);
                       break;
                   case kUnhide:
                       SetHidden(app, false);
                       break;
                   case kOutOfFolder:
                       // Next to its folder on the grid.
                       Arrange([&](std::vector<StartTile>& tiles) {
                           std::optional<StartTile> moved = Take(tiles, app.id);
                           if (!moved)
                               return;
                           auto at = std::find_if(tiles.begin(), tiles.end(), [&](const StartTile& t) { return t.id == openKey; });
                           tiles.insert(at == tiles.end() ? at : at + 1, std::move(*moved));
                       });
                       break;
                   case kRename:
                       OpenFolder(tileOf(folderKey));
                       StartNaming();
                       break;
                   case kUngroup:
                       // Its apps take its place on the grid.
                       if (folder_ >= 0 && tiles_[size_t(folder_)].key == folderKey)
                           folder_ = -1;
                       Arrange([&](std::vector<StartTile>& tiles) {
                           auto at = std::find_if(tiles.begin(), tiles.end(), [&](const StartTile& t) { return t.id == folderKey; });
                           if (at == tiles.end())
                               return;
                           std::vector<StartTile> apps = std::move(at->apps);
                           at = tiles.erase(at);
                           tiles.insert(at, apps.begin(), apps.end());
                       });
                       Log(L"start: folder %s ungrouped", folderKey.c_str());
                       break;
                   case kSize:
                   case kSize + 1:
                   case kSize + 2:
                       SetSize(cmd - kSize);
                       layout_.size = shown_;
                       SaveStartLayout(layout_);
                       Log(L"start: sized to %d x %d tiles", cols_, rows_);
                       break;
                   case kReset:
                       layout_.custom = false;
                       layout_.tiles.clear();
                       SaveStartLayout(layout_);
                       Rebuild();
                       surface_.Invalidate();
                       Log(L"start: back to the Windows start menu's pins");
                       break;
                   }
               },
               [this] {
                   menuOpen_ = false;
                   if (!surface_.Visible())
                       return;
                   // Back to the start menu, unless the click that closed the menu went to another window.
                   HWND fg = GetForegroundWindow();
                   if (!fg || fg == surface_.Hwnd() || fg == menu->Hwnd())
                       SetForegroundWindow(surface_.Hwnd());
                   else
                       Close(false);
               });
    surface_.Invalidate();
}

void StartMenu::MoveSelection(int dx, int dy)
{
    if (folder_ >= 0 && !folderClosing_) {
        // The open folder's apps.
        const int n = int(tiles_[size_t(folder_)].apps.size()), cols = FolderCols(), per = cols * FolderRows();
        if (!n)
            return;
        int next = sel_ < 0 ? folderPage_ * per : sel_ + dx + dy * cols;
        sel_ = std::clamp(next, 0, n - 1);
        folderPage_ = sel_ / per;
        surface_.Invalidate();
        return;
    }
    if (Listing()) {
        // Rows; the headings are passed over. With nothing selected in the
        // results, the first row counts as selected.
        const auto& rows = RowsShown();
        std::vector<int> selectable;
        for (int i = 0; i < int(rows.size()); ++i)
            if (!rows[size_t(i)].letter)
                selectable.push_back(i);
        if (selectable.empty())
            return;
        int at = int(std::find(selectable.begin(), selectable.end(), sel_) - selectable.begin());
        if (sel_ < 0)
            at = !query_.empty() ? 0 : -1;
        int step = dy + dx;
        at = std::clamp(at < 0 ? 0 : at + step, 0, int(selectable.size()) - 1);
        sel_ = selectable[size_t(at)];
        EnsureVisible();
        surface_.Invalidate();
        return;
    }
    const int n = int(tiles_.size()), cols = cols_;
    if (!n)
        return;
    int next;
    if (sel_ < 0)
        next = page_ * PerPage(); // the first tile of the page shown
    else if (int to = sel_ + dx + dy * cols; to >= 0 && to < n)
        next = to;
    else
        next = dx ? std::clamp(to, 0, n - 1) : sel_; // up/down past the grid stays
    sel_ = std::clamp(next, 0, n - 1);
    SetPage(sel_ / PerPage());
    surface_.Invalidate();
}

void StartMenu::EnsureVisible()
{
    if (sel_ < 0 || !Listing())
        return;
    const auto& rows = RowsShown();
    float top = 0;
    for (int i = 0; i < sel_ && i < int(rows.size()); ++i)
        top += rows[size_t(i)].letter ? kHeadH : kRowH;
    const float viewH = ListBottom() - kListTop;
    if (top < scrollTarget_)
        scrollTarget_ = top;
    else if (top + kRowH > scrollTarget_ + viewH)
        scrollTarget_ = top + kRowH - viewH;
}

void StartMenu::PlaceIme()
{
    float s = surface_.Scale();
    if (HIMC imc = ImmGetContext(surface_.Hwnd())) {
        // Window coordinates: the content starts after the shadow margin.
        RECT win{};
        GetWindowRect(surface_.Hwnd(), &win);
        POINT origin{surface_.Rect().left - win.left, surface_.Rect().top - win.top + OffsetPx()};
        // Below the field being typed in: the search, or the folder's name.
        const D2D1_RECT_F f = FolderRect();
        const float x = naming_ ? (f.left + f.right) / 2 - 60 : kSearchX + 46, y = naming_ ? f.top + 50 : kSearchTop + kSearchH + 4;
        COMPOSITIONFORM cf{};
        cf.dwStyle = CFS_POINT;
        cf.ptCurrentPos = POINT{origin.x + LONG(x * s), origin.y + LONG(y * s)};
        ImmSetCompositionWindow(imc, &cf);
        ImmReleaseContext(surface_.Hwnd(), imc);
    }
}

bool StartMenu::Message(UINT m, WPARAM w, LPARAM l, LRESULT& result)
{
    // Mouse positions in the menu's coordinates (the panel's corner, lower in the window).
    POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
    const POINT content = pt;
    if (m >= WM_MOUSEFIRST && m <= WM_MOUSELAST && m != WM_MOUSEWHEEL && m != WM_MOUSEHWHEEL)
        pt.y -= OffsetPx();
    switch (m) {
    case WM_ACTIVATE:
        if (LOWORD(w) == WA_INACTIVE && !menuOpen_ && surface_.Visible()) {
            wchar_t cls[64] = {};
            GetClassNameW(reinterpret_cast<HWND>(l), cls, 64);
            Log(L"start: deactivated by %s", cls);
            previous_ = nullptr; // the user clicked elsewhere: that window has the focus now
            Close(false);
        }
        return false;
    case WM_TIMER:
        if (w == kStatusTimer) {
            if (onStatusRequest)
                onStatusRequest();
            return true;
        }
        if (w == kDragTimer) {
            DragTick();
            return true;
        }
        return false;
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT && (sizing_ || hover_ == L"resize")) {
            SetCursor(LoadCursorW(nullptr, IDC_SIZENESW));
            result = TRUE;
            return true;
        }
        return false;
    case WM_MOUSEMOVE: {
        if (sizing_) {
            POINT p;
            GetCursorPos(&p);
            Resize(p);
            return true;
        }
        if (scrubbing_) {
            ScrubTo(float(pt.y) / surface_.Scale());
            return true;
        }
        if (drag_.armed && !drag_.on && (w & MK_LBUTTON) &&
            (std::abs(pt.x - pressPt_.x) > GetSystemMetrics(SM_CXDRAG) || std::abs(pt.y - pressPt_.y) > GetSystemMetrics(SM_CYDRAG))) {
            drag_.armed = false;
            BeginDrag();
        }
        if (drag_.on) {
            UpdateDrag(pt);
            return true;
        }
        if (!tracking_) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, surface_.Hwnd(), 0};
            tracking_ = TrackMouseEvent(&tme) != FALSE;
            surface_.Invalidate();
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
        if (!drag_.on && !sizing_ && !scrubbing_) {
            hover_.clear();
            pressed_.clear();
        }
        surface_.Invalidate();
        return true;
    case WM_LBUTTONDOWN: {
        pressed_ = HitAt(pt);
        if (powerOpen_ && pressed_ != L"power" && !Starts(pressed_, L"pw:"))
            powerOpen_ = false; // a press elsewhere folds the power card away
        if (naming_ && pressed_ != L"ftitle")
            FinishNaming(true);
        const float s = surface_.Scale(), x = float(pt.x) / s, y = float(pt.y) / s;
        // A press beside the open folder closes it (the capsule's buttons and the grip still work).
        if (folder_ >= 0 && !folderClosing_ && y < MainRect().bottom && !In(FolderRect(), x, y) && pressed_ != L"resize") {
            CloseFolder();
            pressed_.clear();
            return true;
        }
        if (pressed_ == L"azbar") {
            scrubbing_ = true;
            SetCapture(surface_.Hwnd());
            hintY_ = y;
            ScrubTo(y);
            return true;
        }
        if (pressed_ == L"resize") {
            sizing_ = true;
            GetCursorPos(&sizeFrom_);
            sizeStart_ = shown_;
            SetCapture(surface_.Hwnd());
            return true;
        }
        if ((Starts(pressed_, L"tile:") && folder_ < 0) || (Starts(pressed_, L"fapp:") && folder_ >= 0 && !folderClosing_)) {
            drag_.armed = true; // a tile can be carried away
            pressPt_ = pt;
            SetCapture(surface_.Hwnd());
        }
        if (!pressed_.empty() && pressed_ != L"search" && pressed_ != L"fcard") {
            rippleId_ = pressed_;
            rippleX_ = x;
            rippleY_ = y;
            rippleMs_ = ClockMs();
            // 玻璃涟漪使用内容坐标，保留菜单的垂直偏移。
            surface_.Ripple(float(content.x) / s, float(content.y) / s);
        }
        surface_.Invalidate();
        return true;
    }
    case WM_LBUTTONUP: {
        if (scrubbing_) {
            scrubbing_ = false;
            pressed_.clear();
            if (GetCapture() == surface_.Hwnd())
                ReleaseCapture();
            surface_.Invalidate();
            return true;
        }
        if (sizing_) {
            EndResize();
            pressed_.clear();
            return true;
        }
        if (drag_.on) {
            pressed_.clear();
            EndDrag(true);
            return true;
        }
        if (drag_.armed) {
            drag_.armed = false;
            if (GetCapture() == surface_.Hwnd())
                ReleaseCapture();
        }
        std::wstring h = HitAt(pt);
        bool same = !h.empty() && h == pressed_;
        pressed_.clear();
        if (same)
            Activate(h);
        else
            surface_.Invalidate();
        return true;
    }
    case WM_CAPTURECHANGED:
        // Taken away mid-way (another window, a system dialog): put the tile back, keep the size.
        if (drag_.on || drag_.armed)
            EndDrag(false);
        if (sizing_)
            EndResize();
        if (scrubbing_) {
            scrubbing_ = false;
            surface_.Invalidate();
        }
        return false;
    case WM_RBUTTONUP:
        if (!drag_.on && !sizing_ && !scrubbing_)
            ContextMenu(HitAt(pt), surface_.ContentToScreen(content));
        return true;
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL: {
        POINT client = surface_.ScreenToContent(pt);
        client.y -= OffsetPx();
        float notches = float(GET_WHEEL_DELTA_WPARAM(w)) / WHEEL_DELTA;
        if (m == WM_MOUSEWHEEL && HitAt(client) == L"volume") {
            status_.volume = AdjustVolume(int(std::lround(2 * notches)));
            if (notches > 0)
                status_.muted = false;
            if (onStatusRequest)
                onStatusRequest();
        } else if (folder_ >= 0) {
            // The folder's pages.
            wheelAcc_ += m == WM_MOUSEWHEEL ? -notches : notches;
            if (std::fabs(wheelAcc_) >= 1) {
                folderPage_ = std::clamp(folderPage_ + (wheelAcc_ > 0 ? 1 : -1), 0, FolderPages() - 1);
                wheelAcc_ = 0;
            }
        } else if (Listing()) {
            if (m == WM_MOUSEWHEEL)
                scrollTarget_ = std::clamp(scrollTarget_ - notches * 2 * kRowH, 0.f, std::max(0.f, contentH_ - (ListBottom() - kListTop)));
        } else {
            // A page per notch (touchpads: per notch's worth of small steps);
            // the wheel turned down, or a swipe to the right, goes forward.
            wheelAcc_ += m == WM_MOUSEWHEEL ? -notches : notches;
            if (std::fabs(wheelAcc_) >= 1) {
                SetPage(page_ + (wheelAcc_ > 0 ? 1 : -1));
                wheelAcc_ = 0;
            }
        }
        surface_.Invalidate();
        return true;
    }
    case WM_IME_STARTCOMPOSITION:
        PlaceIme();
        return false;
    case WM_KEYDOWN:
        if (naming_)
            return w == VK_LEFT || w == VK_RIGHT || w == VK_UP || w == VK_DOWN; // the name is typed, not navigated
        if (powerOpen_ && (w == VK_UP || w == VK_DOWN)) {
            powerSel_ = std::clamp(powerSel_ < 0 ? 0 : powerSel_ + (w == VK_DOWN ? 1 : -1), 0, 3);
            surface_.Invalidate();
            return true;
        }
        switch (w) {
        case VK_UP:
            MoveSelection(0, -1);
            return true;
        case VK_DOWN:
            MoveSelection(0, 1);
            return true;
        case VK_LEFT:
            MoveSelection(-1, 0);
            return true;
        case VK_RIGHT:
            MoveSelection(1, 0);
            return true;
        case VK_F2:
            StartNaming(); // the open folder's name
            return true;
        case VK_PRIOR:
        case VK_NEXT:
            if (!Listing() && folder_ < 0) {
                SetPage(page_ + (w == VK_NEXT ? 1 : -1));
                return true;
            }
            return false;
        }
        return false;
    case WM_CHAR: {
        wchar_t ch = wchar_t(w);
        result = 0;
        if (naming_) {
            if (ch == 27)
                FinishNaming(false);
            else if (ch == 13)
                FinishNaming(true);
            else if (ch == 8 && !nameDraft_.empty())
                nameDraft_.pop_back();
            else if (ch >= 32 && nameDraft_.size() < 24)
                nameDraft_ += ch;
            surface_.Invalidate();
            return true;
        }
        if (ch == 27) { // Esc: one step back at a time, then close
            if (powerOpen_) {
                powerOpen_ = false;
                surface_.Invalidate();
            } else if (folder_ >= 0 && !folderClosing_) {
                CloseFolder();
            } else if (!query_.empty()) {
                Activate(L"clear");
            } else if (allApps_) {
                Activate(L"allapps");
            } else {
                Close(false);
            }
        } else if (ch == 13) {
            if (powerOpen_) {
                RunPower(powerSel_);
            } else if (folder_ >= 0 && !folderClosing_) {
                if (sel_ >= 0)
                    Activate(L"fapp:" + std::to_wstring(sel_));
            } else if (Listing()) {
                const auto& rows = RowsShown();
                int i = sel_;
                if (i < 0 && !query_.empty())
                    for (int k = 0; k < int(rows.size()) && i < 0; ++k)
                        if (!rows[size_t(k)].letter)
                            i = k;
                if (i >= 0)
                    Activate(L"row:" + std::to_wstring(i));
            } else if (sel_ >= 0) {
                Activate(L"tile:" + std::to_wstring(sel_));
            }
        } else if (ch == 8) {
            if (!query_.empty()) {
                query_.pop_back();
                results_ = Results();
                if (query_.empty())
                    Restage();
            }
            sel_ = -1;
            scroll_ = scrollTarget_ = 0;
            surface_.Invalidate();
        } else if (ch >= 32 && query_.size() < 64) {
            CloseFolder(); // typing searches, wherever the menu was
            if (query_.empty())
                Restage(); // the results slide in
            query_ += ch;
            results_ = Results();
            sel_ = -1;
            scroll_ = scrollTarget_ = 0;
            powerOpen_ = false;
            surface_.Invalidate();
        }
        return true;
    }
    }
    return false;
}

} // namespace atrium
