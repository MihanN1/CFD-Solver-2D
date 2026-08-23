#pragma once

#include <string>

namespace maskui {

// The tray icon, and the progress that goes with it.
//
// A run can take a long time, and a window nobody is looking at reports
// nothing. On Windows this puts an icon in the notification area for the
// length of a run: its tooltip is the simulated time, the taskbar button fills
// up behind the same number, the window can be sent to the tray and brought
// back, and the run can be asked to stop from the menu. On the rest the icon
// does not exist - a tray means a desktop toolkit a static SFML build has no
// business linking - so available() answers false and the caller puts the same
// progress in the window title, which is what the taskbar or the Dock shows
// for that window anyway.
//
// Everything here is safe to call when there is no tray: the whole class turns
// into a handful of no-ops and one false.
class TrayIcon {
public:
    // What the person clicked. The tray lives on its own thread, so commands
    // are queued rather than acted on, and the UI takes them at the top of a
    // frame - which is the only place it is allowed to touch its own window.
    enum class Command {
        None,
        ShowWindow,
        HideWindow,
        OpenOutput,
        StopSimulation,
        Quit
    };

    TrayIcon() = default;
    ~TrayIcon();

    TrayIcon(const TrayIcon&) = delete;
    TrayIcon& operator=(const TrayIcon&) = delete;

    // windowHandle is the platform window this belongs to: on Windows the
    // HWND, cast to void*. Safe to call more than once; the second call does
    // nothing.
    void attach(void* windowHandle, const std::string& tooltip);
    void detach();

    bool available() const;

    // fraction is 0..1, or negative when the length of the run is unknown.
    // tooltip is what the icon says; the caller writes the same text into the
    // window title where there is no tray.
    void setProgress(bool running, double fraction, const std::string& tooltip);

    // A one-line notification. Ignored where there is no tray.
    void notify(const std::string& title, const std::string& text, bool problem);

    // Called by the UI after it hides or shows its window, so the menu offers
    // the other one.
    void setWindowVisible(bool visible);

    // Whether a simulation is running, so the menu can offer to stop it.
    void setSimulationRunning(bool running);

    Command takeCommand();

private:
    bool attached_ = false;
};

}   // namespace maskui
