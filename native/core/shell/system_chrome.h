// The parts of Windows' own shell that Atrium can stand in for, hidden on the
// user's request (both settings are off by default): the system taskbar
// (Shell_TrayWnd and the secondary ones are hidden while Atrium runs, and set
// to auto-hide so they leave no gap) and the system's notification banners
// (Do Not Disturb through the shell's QuietHoursSettings service: the
// notifications still reach the notification centre, where the listener reads
// them for Atrium's cards). Everything is put back when the setting is turned
// off or Atrium exits; what was changed is kept in system.json, so a run that
// did not end cleanly is undone by the next start, or at once by the guard
// (`Atrium.exe --guard`, main.cpp) that watches a run hiding them.
#pragma once

#include <windows.h>

#include <atomic>
#include <string>

namespace atrium {

class SystemChrome {
public:
    SystemChrome();
    ~SystemChrome(); // puts everything back

    void SetTaskbarHidden(bool hidden);
    void SetBannersHidden(bool hidden);
    // Hides the taskbars again if Explorer has shown them (a restart of
    // Explorer, a display change); follows the user turning Do Not Disturb off.
    void Enforce();
    // Irreversible for this instance: disable every hide entry point, then
    // restore Windows before joining workers or waiting for the renderer.
    void RestoreForExit();
    // Do Not Disturb is on because Atrium turned it on (Atrium's own cards
    // still pop up then).
    bool QuietIsOurs() const { return quietOurs_; }
    // From a crash handler: stops hiding, restores our auto-hide change and
    // shows the taskbars (no allocation, no COM).
    static void EmergencyRestore();
    // What a run that did not end cleanly left changed (system.json), put back; false if nothing
    // was. COM must be initialised (Do Not Disturb).
    static bool UndoLeftover();
    // The taskbars shown, nothing else changed (Atrium is not responding).
    static void ShowTaskbarsNow();

private:
    void HideTaskbars();
    void Watch();
    void Save() const;
    static void CALLBACK OnShow(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD);

    std::atomic_bool taskbarHidden_ = false; // also cleared by a crashing worker
    std::atomic_bool autoHideOurs_ = false; // Atrium switched the taskbar to auto-hide
    bool quietOurs_ = false;
    bool exiting_ = false;
    HWINEVENTHOOK hook_ = nullptr;
    DWORD hookedPid_ = 0;
};

// Do Not Disturb: the user's selected profile, e.g.
// "Microsoft.QuietHoursProfile.Unrestricted"; empty if the service is unavailable.
std::wstring QuietProfile();
bool SetQuietProfile(const wchar_t* profile);

} // namespace atrium
