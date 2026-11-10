// Stage Manager, after macOS's: the app in front stays on the stage, the others wait in a strip at
// the side of the screen, their windows standing at an angle in perspective, each with its shadow and
// its app's icon, nothing behind them. A click there swaps the sets; Shift and a click brings a set
// onto the stage beside the one there, and the apps on the stage together go back to the strip as
// one set, to come back together. Activating a window any other way (the bar, Alt+Tab, a
// notification) changes the stage too.
//
// Waiting windows are minimized without activation: Windows' own state, so nothing is lost if
// Atrium goes away. A minimized window has no picture, so one is taken just before
// (ui/window_snapshot.h). The window then goes at once (its own minimize animation turned off) and
// its picture flies from where it was into its place in the strip; picked there, the picture flies
// back and the window comes up under it as it lands. Which windows Atrium put away is kept in %LOCALAPPDATA%\Atrium\stage.json,
// so that a restart picks them up again; turning Stage Manager off or quitting Atrium brings them
// all back.
//
// Only the selected desktop display is managed. Always shown, the strip steps aside while a window on the stage
// covers its place; auto-hidden, it only comes out while the pointer rests on that edge of the screen.
#pragma once

#include "motion.h"
#include "shell/task_model.h"
#include "ui/surface.h"
#include "ui/window_snapshot.h"

#include <shobjidl.h>
#include <winrt/base.h>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace atrium {

class StageManager {
public:
    StageManager(HINSTANCE instance, TaskModel& tasks);
    ~StageManager() = default;
    StageManager(const StageManager&) = delete;
    StageManager& operator=(const StageManager&) = delete;

    void SetEnabled(bool on);
    bool Enabled() const { return enabled_; }
    void SetOnRight(bool onRight);
    void SetAutoHide(bool autoHide);
    void SetMonitor(HMONITOR monitor, bool refresh = false);
    // The task model changed (every group, also those the bar does not show).
    void Update(const std::vector<TaskItem>& items);
    // Everything put away comes back (on exit).
    void RestoreAll();

private:
    struct Parked {
        std::wstring key;
        uint64_t seq = 0; // when it was put away
        ULONGLONG since = 0;
        bool down = true; // minimized (it waits for its picture first)
        RECT frame{};     // where it was on the screen, px (it comes back there)
    };
    struct Picture {
        std::shared_ptr<const WindowPicture> picture;
        unsigned gen = 0; // its images' cache keys change with it
        ULONGLONG taken = 0; // when it was taken (or asked for)
    };
    // A picture in perspective: `rect` (DIP) turned `angle` degrees about the upright axis through
    // (cx, cy), `z` towards the eye, seen from in front of (ox, oy).
    struct Pose {
        D2D1_RECT_F rect{};
        float cx = 0, cy = 0, ox = 0, oy = 0, angle = 0, z = 0;
    };
    // A window's picture on its way between the window's place and the strip, on the flight layer
    // (DIP from the monitor's top left).
    struct Flight {
        HWND window = nullptr;
        std::shared_ptr<const WindowPicture> picture;
        std::wstring key;
        bool toStrip = true;
        Pose from;        // fromStrip: where it was in the strip
        Pose place;       // the window's place
        double elapsed = 0, last = 0; // ms of the flight drawn, and when the last frame was
        bool restored = false, activate = false;
    };
    using Set = std::vector<std::wstring>; // apps that come and go together
    struct Entry {
        Set keys;
        std::vector<HWND> windows; // front first, at most kStack
        motion::Follow hover;
        // Cards glide to their place (a spring) and come and go with a fade and a little scale.
        float y = 0, vy = 0, targetY = 0;
        float appear = 0, va = 0, targetAppear = 1;
    };

    void Stage(const Set& keys, bool add, HWND front, bool activate, const std::unordered_map<HWND, Pose>* fly = nullptr);
    void Park(HWND window, const std::wstring& key);
    void Unpark(const Set& keys, HWND front, bool activate, const std::unordered_map<HWND, Pose>* fly = nullptr);
    void Pick(const Set& keys, bool add);
    void PictureTaken();       // WM_STAGE_PICTURE
    void Launch();             // the flight layer, up for a flight
    void Foresee();            // pictures of the stage's windows, ahead of a pick
    void PutDown(HWND window, bool fly); // minimizes a window put away, once; its picture flies to the strip
    Set SetOf(const std::wstring& key) const; // the set in the strip `key` belongs to, or just it
    void Prune();
    void Save();
    void Load();
    int Capacity() const;

    // The strip.
    void Rebuild();
    void Layout();
    void Show();
    void Hide();
    void Paint(Canvas& c);
    void PaintEntry(Canvas& c, Entry& e, int index, bool live);
    bool Message(UINT m, WPARAM w, LPARAM l, LRESULT& result);
    bool Animate(double nowMs);                  // steps the cards; true while any moves
    float Reveal(int index, double nowMs) const; // how far a card is out of the screen's edge, 0..1
    void UpdateRegion(bool moving);              // clicks around the cards reach what is under them
    void Watch();                                // the strip's timer: is it covered, does the pointer ask for it
    bool Covered();                              // a window on the stage is where the strip goes
    RECT Zone() const;                           // where the strip goes, to the screen's edge, px
    RECT WorkArea() const;
    D2D1_RECT_F Box(const Entry& e) const; // the windows' room, content DIP
    int EntryAt(POINT content) const;
    std::shared_ptr<const WindowPicture> PictureOf(HWND window, const std::wstring& key, std::wstring& cacheKey);
    // Where window j of a card stands at rest, strip DIP; `angle` its turn.
    Pose PoseOf(const Entry& e, size_t j, const WindowPicture& p, float angle) const;
    bool StripPose(HWND window, Pose& pose);   // at rest in the strip, flight DIP
    Pose PlacePose(const RECT& frame) const;  // a window's place on the screen, flight DIP
    D2D1_POINT_2F StripOffset() const;        // strip DIP to flight DIP
    bool Flying(HWND window) const;
    void PaintFlights(Canvas& c);
    void DrawPicture(Canvas& c, const WindowPicture& p, const std::wstring& key, const Pose& pose, float opacity);

    TaskModel& tasks_;
    Surface surface_;
    Surface flight_; // over the whole screen while pictures fly
    WindowSnapshots snapshots_;
    winrt::com_ptr<IVirtualDesktopManager> desktops_; // the strip shows the current virtual desktop's windows
    bool enabled_ = false, onRight_ = false, autoHide_ = false;
    bool applied_ = false;             // the settings have said once whether it is on
    std::wstring saved_;               // stage.json as last written
    HWND raise_ = nullptr;             // to put back on top once the windows coming back have
    HMONITOR monitor_ = nullptr;
    std::vector<TaskItem> items_;
    Set stage_;                        // the apps on the stage
    std::vector<Set> recent_;          // waiting sets, most recent first
    std::unordered_map<HWND, Parked> parked_;
    std::unordered_map<HWND, Picture> pictures_;
    std::unordered_map<std::wstring, std::shared_ptr<const WindowPicture>> placeholders_; // by app
    unsigned pictureGen_ = 0;
    uint64_t seq_ = 0;
    Set pending_;                      // picked in the strip, not in front yet
    ULONGLONG pendingUntil_ = 0;

    std::vector<Entry> entries_;
    std::vector<Entry> leaving_;       // cards fading out (picked, or their windows gone)
    std::vector<Flight> flights_;
    double lastStep_ = 0;
    RECT region_{};                    // the window region last set, client px
    RECT rect_{};                      // the strip, screen px: room for Capacity() cards
    D2D1_SIZE_F size_{};               // DIP
    int hover_ = -1, pressed_ = -1;
    bool tracking_ = false, inside_ = false;
    // Out (or on its way out) of the screen's edge; the cards slide out one after the other.
    bool shown_ = false;
    float revealFrom_ = 0, reveal_ = 0;
    double revealStart_ = 0;
    bool peek_ = false;                // shown over the stage because the pointer asked
    ULONGLONG edgeSince_ = 0, leftAt_ = 0;
    bool onDesktop_ = false;           // the desktop was in front at the last look
    bool covered_ = false;
    ULONGLONG coveredAt_ = 0;
};

} // namespace atrium
