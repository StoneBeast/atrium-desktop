// Atrium's own start menu (opened from the logo on the bar, or by the Windows
// key: winKeyStart), a launcher of separate pieces of the bar's glass: a panel
// with the search field and the tiles, and below it a capsule shaped like the
// bar with the account, network, volume, battery and power.
// The power choices open in a third piece of glass, an open folder in another
// over the tiles.
//
// The tiles are the apps pinned to the Windows start menu until the user
// arranges them (shell/start_layout.h): dragged to another place or page, onto
// another app (a folder of the two) or into a folder. Folders open in a small
// card, take a name and give their apps back. The grip in the panel's top
// right corner (or the menu of the panel) picks one of three sizes; the
// window keeps the largest that fits and the pieces of glass spring to the
// size inside it. Typing searches all apps and folders, by name or by
// initials ("wx" for 微信); the button next to the field lists them all from
// A to Z, with the letters down the left to jump through the list.
// No recommendations, recent files or file search. Tiles lift under the
// pointer, presses ripple.
#pragma once

#include "motion.h"
#include "shell/app_catalog.h"
#include "shell/start_layout.h"
#include "shell/system_status.h"
#include "shell/task_model.h"
#include "ui/glass_menu.h"
#include "ui/surface.h"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace atrium {

class StartMenu {
public:
    StartMenu(HINSTANCE instance, TaskModel& tasks, AppCatalog& catalog);

    // barRect: the bar's glass shape on screen; the menu opens above its left
    // end, out of `logo` (the logo's rectangle).
    // previous: the window to give the focus back to when closed without launching.
    void Open(RECT barRect, RECT logo, HWND previous);
    void Close(bool launched);
    bool Visible() const { return surface_.Visible(); }
    ULONGLONG LastClosed() const { return lastClosed_; }
    void Refresh(); // an icon or the catalog changed
    void SetStatus(const SystemStatus& status);
    static int TileIconPx(float scale); // the tiles' icon size (AppCatalog::PrefetchStartIcons)
    HWND Hwnd() const { return surface_.Hwnd(); }
    // Corner radius (DIP), the bar's; the panel uses it, the capsule is round like the bar.
    void SetRadius(float dip) { radius_ = dip; surface_.SetRadius(dip); }
    // Height of the bar (DIP): the status capsule has the same.
    void SetBarHeight(float dip) { barH_ = dip; }

    std::function<void(const PinnedApp&)> onPin;            // pin to the bar (the dock)
    std::function<void(const std::wstring& what)> onSystem; // "network", "volume", "battery", "tray", "account"
    std::function<void()> onStatusRequest;
    GlassMenu* menu = nullptr;                               // a tile's menu (main's)

private:
    struct Hit {
        D2D1_RECT_F rect; // DIP, content coordinates
        std::wstring id;
    };
    // A tile of the grid: an app, or a folder of apps.
    struct Tile {
        std::wstring key; // the app's id, or the folder's
        std::wstring name;
        CatalogApp app;
        std::vector<CatalogApp> apps; // a folder's
        bool folder = false;
    };
    // A line of the search results or of the A-Z list.
    struct Row {
        wchar_t letter = 0; // a heading of the list (no app)
        int folder = -1;    // a folder: its tile
        CatalogApp app;
    };
    // A tile carried by the pointer: from the grid, or out of the open folder.
    struct Drag {
        bool armed = false, on = false;
        bool fromFolder = false; // one of the open folder's apps
        bool inFolder = false;   // still over the folder (its apps make room), else over the grid
        int from = -1;           // its tile, or its place in the folder
        int folderTile = -1;     // the folder it came out of
        std::wstring key;
        float x = 0, y = 0;      // the pointer (DIP)
        float grabX = 0, grabY = 0; // the pointer within the tile's cell
        int insert = -1;         // where the others make room (display slot)
        int merge = -1;          // the tile it would go into (a folder of the two)
        int pendingSlot = -1;    // the slot the pointer rests on, before the others move
        double pendingMs = 0, edgeMs = 0;
        int edge = 0;            // resting at the grid's left (-1) or right (1) edge: turns the page
    };

    void Paint(Canvas& c);
    void PaintSearch(Canvas& c, const Theme& t);
    void PaintTiles(Canvas& c, const Theme& t, float shown);
    // A tile in `cell`: its icon (a folder: its first apps, small) and name. target: a carried app would go into it.
    void PaintTileFace(Canvas& c, const Theme& t, const Tile& tile, D2D1_RECT_F cell, const std::wstring& id, bool selected, bool target);
    void DrawIcon(Canvas& c, const std::wstring& id, int px, D2D1_RECT_F r, float radius);
    void PaintRows(Canvas& c, const Theme& t, const std::vector<Row>& rows, float shown);
    void PaintIndex(Canvas& c, const Theme& t); // the A-Z list's letters, and the one being picked
    void PaintFolder(Canvas& c, const Theme& t, D2D1_RECT_F card, float shown);
    void PaintDragged(Canvas& c, const Theme& t);
    void PaintShelf(Canvas& c, const Theme& t);
    void PaintPower(Canvas& c, const Theme& t);
    void PaintGrip(Canvas& c, const Theme& t);
    void PaintDots(Canvas& c, const Theme& t, int pages, float current, float y, const wchar_t* prefix);
    bool Message(UINT m, WPARAM w, LPARAM l, LRESULT& result);

    // The tiles, the rows.
    void Rebuild();            // tiles_ from the layout (or the Windows pins) and the catalog
    std::vector<Row> Results() const; // search matches, best first
    std::vector<Row> AllApps() const; // the A-Z list with its headings
    int PerPage() const { return cols_ * rows_; }
    int Pages() const;
    int FolderPages() const;
    void SetPage(int page);
    // Changing the arrangement: the Windows pins copied in first, saved after.
    void Arrange(const std::function<void(std::vector<StartTile>&)>& change);
    void PinToStart(const CatalogApp& app);
    bool OnStart(const std::wstring& id) const;
    bool Hidden(const std::wstring& id) const;
    void SetHidden(const CatalogApp& app, bool hidden);
    void ShowHidden(); // the A-Z list at its hidden apps

    // Geometry (DIP). The menu's own coordinates: the panel's top left corner is
    // 0, 0; in the window it sits OffsetY() lower, on the window's bottom edge.
    float W() const; // the panel and the capsule, as shown (springing to a new size)
    float H() const;
    D2D1_SIZE_F SizeFor(int size) const; // a size's, StartLayout::kCols/kRows
    D2D1_SIZE_F Size() const;            // the window's: the largest size that fits
    bool Fits(int size) const;
    float OffsetY() const;
    LONG OffsetPx() const;
    void SetSize(int size); // the grid's, the pieces spring to it
    D2D1_RECT_F MainRect() const;
    D2D1_RECT_F ShelfRect() const;
    D2D1_RECT_F PowerRect() const; // the power card when fully open
    D2D1_RECT_F SearchRect() const;
    D2D1_RECT_F AllButton() const; // "全部应用" / "返回"
    D2D1_RECT_F FolderRect() const; // the open folder's card
    float ListBottom() const;
    float GridX() const;
    D2D1_RECT_F Cell(int slot) const; // a grid slot, its page laid out side by side (x + page * W)
    int FolderApps() const;
    int FolderCols() const;
    int FolderRows() const;
    D2D1_RECT_F FolderCell(int slot) const; // likewise in the folder's card
    float ButtonSize() const;      // round buttons in the capsule
    float Covered(const D2D1_RECT_F& r, double now) const; // 1, or less while the power card covers r
    D2D1_RECT_F IndexRect() const; // the A-Z letters down the list's left side
    bool Indexed() const { return allApps_ && query_.empty(); }

    // Folders.
    void OpenFolder(int tile);
    void CloseFolder();
    float FolderOpen(double now) const; // 0 closed .. 1 open
    void StartNaming();
    void FinishNaming(bool keep);

    // Dragging tiles; resizing.
    std::vector<int> Order() const;       // the grid's slots: tile indices, -1 the gap
    std::vector<int> FolderOrder() const; // the folder's
    void BeginDrag();
    void UpdateDrag(POINT client);
    void EndDrag(bool commit);
    void DragTick(); // the pointer resting: the others make room, the page turns
    void Resize(POINT screen);
    void EndResize();
    void ScrubTo(float y); // a letter of the index: the list glides to it
    wchar_t TopLetter() const; // the letter of the list's rows at the top

    void Activate(const std::wstring& id);
    void Launch(const CatalogApp& app);
    void ContextMenu(const std::wstring& id, POINT screen);
    void RunPower(int index);
    std::wstring HitAt(POINT client) const;
    const std::vector<Row>& RowsShown() const { return query_.empty() ? list_ : results_; }
    bool Listing() const { return !query_.empty() || allApps_; }
    void MoveSelection(int dx, int dy);
    void EnsureVisible();
    void PlaceIme();
    void Restage(); // the content changed: tiles slide in again
    void LoadAccount();
    float Hover(Canvas& c, const std::wstring& id, bool on); // eased 0..1, per element

    Surface surface_;
    TaskModel& tasks_;
    AppCatalog& catalog_;
    StartLayout layout_;
    int cols_ = 0, rows_ = 0;   // the grid of the size shown (layout_.size, unless the screen is too small)
    int shown_ = 0, room_ = 0;  // that size; the largest that fits (the window's)
    float panelW_ = 0, panelV_ = 0, mainH_ = 0, mainV_ = 0; // the panel's width and height, springing (DIP, DIP/s)
    std::vector<CatalogApp> apps_, pins_;
    std::vector<Tile> tiles_;
    std::vector<Row> results_, list_;
    std::vector<std::pair<std::wstring, int>> visibleIcons_;
    std::wstring query_;
    bool allApps_ = false; // the A-Z list
    int page_ = 0;
    float pageShown_ = 0;               // displayed page, gliding to page_
    float wheelAcc_ = 0;
    float scroll_ = 0, scrollTarget_ = 0, contentH_ = 0;
    int sel_ = -1;
    std::wstring hover_, pressed_;
    std::vector<Hit> hits_;
    SystemStatus status_;
    HWND previous_ = nullptr;
    RECT bar_{}, logo_{};
    bool menuOpen_ = false;
    bool tracking_ = false;
    bool stale_ = false;    // the catalog changed while a tile was carried
    ULONGLONG lastClosed_ = 0;
    float radius_ = 30, barH_ = 60;
    // The open folder.
    int folder_ = -1;
    bool folderClosing_ = false;
    double folderMs_ = 0;               // opened (its apps pop up from then)
    float folderGrow_ = 0, folderV_ = 0; // the card: 0 its tile .. 1 open, springing
    D2D1_RECT_F folderFrom_{};          // its tile's icon, where the card grows from
    int folderPage_ = 0;
    float folderPageShown_ = 0;
    bool naming_ = false;
    std::wstring nameDraft_;
    // Dragging and resizing.
    Drag drag_;
    POINT pressPt_{};
    std::map<std::wstring, D2D1_POINT_2F> tilePos_; // where each tile is drawn, gliding to its slot
    bool sizing_ = false;
    POINT sizeFrom_{};
    int sizeStart_ = 0;
    // Picking a letter of the index.
    bool scrubbing_ = false;
    wchar_t scrubLetter_ = 0;
    float scrubY_ = 0, hintY_ = 0;
    // Animation.
    std::map<std::wstring, motion::Follow> hoverAnim_;
    double stageMs_ = 0;    // content (re)appeared: tiles slide in staggered
    double lastPaint_ = 0;
    float dt_ = 16;         // this frame's step (ms)
    std::wstring rippleId_;
    float rippleX_ = 0, rippleY_ = 0;
    double rippleMs_ = 0;
    bool powerOpen_ = false;
    int powerSel_ = -1;
    double powerMs_ = 0;
    // Account shown on the shelf.
    std::wstring userName_;
    IconImage avatar_;
    bool private_ = false; // ATRIUM_PRIVACY: no account or network names
};

} // namespace atrium
