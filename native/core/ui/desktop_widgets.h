// Desktop widgets after macOS's (Sonoma and later): pieces of the bar's glass
// on the desktop, in a grid on the selected desktop display whose halves hug its edges. They
// stand just above the desktop, under every window, always clear glass with
// its ink and a few of macOS's colours (widget_faces). Widgets never become
// topmost. Drag one to move it (it snaps to the
// grid), or by its bottom corner to resize it; right-click for its sizes, to
// remove it or to add another. Notes and reminders take typing (the input
// method too) after a click. The layout and the widgets' content are saved in
// %LOCALAPPDATA%\Atrium\widgets.json.
#pragma once

#include "ui/glass_menu.h"
#include "ui/widget_faces.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace atrium {

class DesktopWidgets {
public:
    explicit DesktopWidgets(HINSTANCE instance);
    ~DesktopWidgets();
    DesktopWidgets(const DesktopWidgets&) = delete;
    DesktopWidgets& operator=(const DesktopWidgets&) = delete;

    void SetEnabled(bool on);
    void SetStatus(const SystemStatus& status);
    void SetOnRight(bool onRight);
    // The bar's monitor: the widgets live in its work area, where its capture gives them glass.
    void SetMonitor(HMONITOR monitor, bool refresh = false);
    void SetMaterial(WidgetMaterial material);
    // Adds a widget in the first free place; false when there is no room.
    bool Add(WidgetKind kind, WidgetSize size);
    // The same by name ("clock", "medium"; the settings app's gallery).
    bool AddNamed(const std::wstring& kind, const std::wstring& size);
    static bool ParseNames(const std::wstring& kind, const std::wstring& size, WidgetKind& outKind, WidgetSize& outSize);

    std::function<void()> onCalendar;    // a calendar widget was clicked
    std::function<void()> onAddWidgets;  // "添加小组件…": the settings app's widget gallery
    std::function<void()> onStatusRequest;
    GlassMenu* menu = nullptr;           // a widget's menu (main's)

private:
    struct Widget;
    static void CALLBACK OnWinEvent(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD);
    static LRESULT CALLBACK HostProc(HWND, UINT, WPARAM, LPARAM);
    void Build();
    void Clear();
    void Load();
    void Save() const;
    void SaveSoon(); // while typing
    Widget& Create(WidgetKind kind, WidgetSize size, int col, int row);
    bool Has(const Widget* w) const;
    void Remove(Widget& w);
    void RemoveNow(Widget& w);
    void Resize(Widget& w, WidgetSize size);
    // The grid in the work area of the bar's monitor.
    float Scale() const;
    RECT WorkArea() const;
    int Cols() const;
    int Rows() const;
    int VisualCol(int col, WidgetSize size) const;
    bool FromRight(int col, WidgetSize size) const;        // placed from the right edge
    RECT CellRect(int col, int row, WidgetSize size) const; // screen px
    bool Fits(int col, int row, WidgetSize size, const Widget* except) const;
    bool FreeCellNear(float col, float row, WidgetSize size, const Widget* except, int& outCol, int& outRow) const;
    void Place(Widget& w, bool glide);
    void Glide();
    void Restack();
    void Tick();
    bool IsOurs(HWND h) const;
    // Bottom corner grips: -1 on the left, 1 on the right, 0 outside either grip.
    int GripAt(const Widget& w, POINT screen) const;
    void DragSize(Widget& w, POINT screen);
    bool Message(Widget& w, UINT m, WPARAM wp, LPARAM lp, LRESULT& r);
    void Menu(Widget& w);
    void Activate(Widget& w);
    void Paint(Widget& w, Canvas& c);
    static void PaintShadow(Canvas& c, D2D1_RECT_F face);
    // Notes and reminders: a click starts typing, keys edit, losing the focus ends it.
    void Click(Widget& w, POINT content);
    void StartEdit(Widget& w, int item);
    void EndEdit(Widget& w);
    bool Key(Widget& w, UINT m, WPARAM wp);
    void Insert(Widget& w, const std::wstring& s);
    void PlaceImeWindow(Widget& w);
    // Photos: a picture from the folder, read and scaled on a worker thread.
    void PickFolder(Widget& w);
    void PickCity(Widget& w, POINT at);
    void LoadPhoto(Widget& w, bool another);
    void SampleSystem();

    HINSTANCE instance_;
    HWND host_ = nullptr; // timers, the menu's owner
    HWINEVENTHOOK hook_ = nullptr, showHook_ = nullptr, uncloakHook_ = nullptr;
    bool enabled_ = false;
    bool onRight_ = false;
    HMONITOR monitor_ = nullptr;
    std::vector<std::unique_ptr<Widget>> widgets_;
    // Saved widgets this screen has no room for (JSON): kept in the layout
    // file, with their notes, until a larger screen shows them again.
    std::vector<std::wstring> unplaced_;
    WidgetWorld world_;
    WidgetMaterial material_ = WidgetMaterial::Glass;
    std::unique_ptr<WeatherSource> weather_;
    std::unique_ptr<BluetoothBatteries> bluetooth_;
    ChineseCalendar lunar_;
    int lastMinute_ = -1;
    uint64_t cpuIdle_ = 0, cpuTotal_ = 0; // the last GetSystemTimes
    unsigned photoGen_ = 0;
    bool restackPosted_ = false;
    // Only Restack may change a widget's place in the z-order: anything else
    // (showing the window, a click, the system) is held back in WM_WINDOWPOSCHANGING.
    bool zMoving_ = false;
};

} // namespace atrium
