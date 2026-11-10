// Notifications above the bar by default, or at the primary display's top right.
// Top-right arrivals push older cards down; the notification centre uses the same position.
// New notices pop up without taking the focus and time out; the bell opens
// the list, with a capsule like the bar below the cards for its controls,
// unread marks, dismiss and clear. Senders are internal
// (tests, Atrium itself) or, experimentally, the Windows notifications read
// through UserNotificationListener.
#pragma once

#include "ui/surface.h"

#include <windows.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace atrium {

struct Notice {
    uint64_t id = 0;
    std::wstring source;     // "test", "atrium", "windows"
    std::wstring app;        // sender's display name
    std::wstring appId;      // sender's AUMID, used to activate it
    std::wstring title, body;
    std::wstring action;     // "uri:<target>", "app:<aumid>" or empty
    uint32_t externalId = 0; // the listener's id (de-duplication)
    ULONGLONG time = 0;      // GetTickCount64 of the latest update
    int count = 1;           // merged notices
    bool unread = true;
};

class Notifications {
public:
    explicit Notifications(HINSTANCE instance);

    std::function<RECT()> bellRect; // screen px
    std::function<RECT()> barRect;
    std::function<void(int unread)> onUnread;
    std::function<bool(const Notice&)> onActivate; // runs the notice's action
    // True while another of Atrium's panels is open: new cards wait for it to
    // close instead of overlapping it (checked again until then).
    std::function<bool()> held;
    // True while Do Not Disturb is on only to hide the system's banners
    // (Atrium's setting): the cards still pop up then.
    std::function<bool()> quietIsOurs;

    void Post(Notice n, bool silent = false);
    void ClearPreparedUpdate();
    void PanelsChanged(); // a panel opened: the cards on screen step aside
    void ToggleCenter(bool expand = false);
    bool CenterVisible() const { return center_.surface && center_.surface->Visible(); }
    void Clear();
    void SetOptions(double timeoutSeconds, bool hideBody, const std::wstring& placement);
    // Corner radius (DIP), the bar's; the cards are concentric with it.
    void SetRadius(float dip);
    int Unread() const;

private:
    enum class Mode { Popup, Center };
    struct Hit {
        D2D1_RECT_F rect;
        std::wstring id;
    };
    // A card's displayed place; it eases towards where the layout wants it,
    // so arrivals, expanding, collapsing and closing all glide.
    struct CardAnim {
        float y = 0, inset = 0, alpha = 0;
        float content = 1; // the card's text (cards behind the stack's front show none)
        bool placed = false;
        float x = 0; // top-right arrivals slide in from the screen edge
    };
    struct View {
        std::unique_ptr<Surface> surface;
        std::vector<Hit> hits;
        std::wstring hover, pressed;
        bool tracking = false;
        int wheel = 0; // wheel delta not yet turned into steps
        float scroll = 0, scrollTarget = 0, contentH = 0, viewH = 0; // the wheel sets the target; the list glides there
        float contentTop = 0; // top-right centre: cards start below the control capsule
        std::map<uint64_t, CardAnim> cards;
        std::map<std::wstring, float> glow; // eased hover per element
        double lastPaint = 0;
        std::wstring rippleId;
        float rippleX = 0, rippleY = 0;
        double rippleMs = 0;
        int regionTop = -1; // window px where the window's region starts (at rest), -1: none
        int regionBottom = -1; // top-right popups leave the desktop below their cards clickable
        // The stack's place: the card in front, in cards (0 the newest),
        // fractional while it moves. The wheel pushes it (momentum, so a
        // flick goes past several cards), then a spring settles it on a
        // card - back from past either end too.
        float stackPos = 0, stackVel = 0;
        float stackGoal = -1;  // the card it settles on; -1: not chosen yet
        int stackFrom = 0;     // the card in front when the wheel started: one notch moves at least one
        bool stackMoving = false;
        ULONGLONG stackFollow = 0; // the last fine wheel step (a touchpad): the stack follows it, then settles
        bool stacked = false;  // the cards are a stack (as last painted)
        size_t count = 0;      // cards shown (as last painted)
        int front = 0;         // the card in front (its text shows)
    };

    void Paint(Mode mode, Canvas& c);
    void PaintCard(Canvas& c, const Notice& n, D2D1_RECT_F r, int index, View& v, bool full, float alpha, bool hideBody);
    float Ease(View& v, Canvas& c, const std::wstring& id, bool on, float dt);
    void PushStack(View& v, size_t n, int wheelDelta); // the wheel over a stack
    bool StepStack(View& v, size_t n, float dtMs);     // true while the stack moves
    // A card at `index` of what `mode` shows was added (+1) or goes (-1):
    // the card in front stays in front.
    void ShiftStack(Mode mode, size_t index, int by);
    void Sweep(); // drops the cards whose closing animation has finished
    bool Message(Mode mode, UINT m, WPARAM w, LPARAM l, LRESULT& result);
    void Click(Mode mode, const std::wstring& id);
    void Activate(uint64_t id);
    void Remove(uint64_t id);
    void Changed();       // unread count, layout of visible windows
    void Layout(Mode mode);
    float CardHeight() const;
    float CardRadius() const; // the bar's, as far as the card's height allows
    std::vector<const Notice*> Shown(Mode mode) const;
    int QuietState() const;
    bool BodyHidden() const;
    void HidePopup(const wchar_t* why);
    View& ViewOf(Mode mode) { return mode == Mode::Popup ? popup_ : center_; }
    const View& ViewOf(Mode mode) const { return mode == Mode::Popup ? popup_ : center_; }

    View popup_, center_;
    std::vector<Notice> notices_;  // newest first
    std::vector<uint64_t> popupIds_; // shown by the popup, newest first
    bool expanded_ = false;
    uint64_t nextId_ = 1;
    double timeout_ = 6;
    bool hideBody_ = false;
    bool topRight_ = false;
    ULONGLONG centerClosed_ = 0;
    float radius_ = 20;
    std::map<uint64_t, double> leaving_; // closing cards: ClockMs() when closed
    // Expanding the popup's stack: the cards move over to the list where
    // they are, and glide into the column (no second pop).
    std::map<uint64_t, CardAnim> handoff_;
    RECT handoffRect_{};
    bool handoffPending_ = false;
    uint64_t handoffFront_ = 0;
    bool handoffPopup_ = false; // the popup waits for the list's glass, then goes
};

} // namespace atrium
