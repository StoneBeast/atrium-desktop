// The tray broker: a hidden window of class Shell_TrayWnd that the shell's
// notification icons are sent to (Shell_NotifyIcon finds the tray by that class
// name). It keeps every icon and tooltip it sees and forwards every message to
// Explorer, so the system tray keeps working exactly as before; what it adds is
// the live icon: a program that changes or blinks its icon delivers a new
// HICON with each message, which the registry snapshot can never show.
//
// Nothing is injected: the window is ours and the messages are addressed to it.
// Explorer's own tray window is left alone apart from forwarding.
#pragma once

#include "shell/task_model.h"

#include <windows.h>

#include <memory>
#include <string>

namespace atrium {

class TrayBroker {
public:
    TrayBroker();
    ~TrayBroker();
    TrayBroker(const TrayBroker&) = delete;
    TrayBroker& operator=(const TrayBroker&) = delete;

    // Creates the window on the calling thread, which must run a message loop.
    // `notify` gets `message` (wParam 0) when the live icons changed. False when
    // the broker is unavailable or turned off (ATRIUM_TRAY_BROKER=0).
    bool Start(HWND notify, UINT message);
    // Destroys the window and asks every program to register its icon with
    // Explorer again (TaskbarCreated), so nothing is left pointing at us.
    void Stop();
    bool Running() const;

    // The live icon of an identity: same program and same uID, or the same
    // GUID. Null when the broker has not seen that icon since it started.
    std::shared_ptr<const IconImage> Image(const std::wstring& exeLower, UINT uid, const GUID* guid) const;

    // The icon's own click, through the callback message its program registered
    // (what Explorer sends). False when the icon is unknown, and the caller
    // falls back to UI Automation.
    bool Click(const std::wstring& exeLower, UINT uid, const GUID* guid, bool right) const;

    struct Impl; // defined in the .cpp

private:
    std::unique_ptr<Impl> impl_;
};

// The process-wide broker (one window per Atrium).
TrayBroker& Broker();

} // namespace atrium
