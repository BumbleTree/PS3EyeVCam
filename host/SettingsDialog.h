#pragma once
//
// Modeless settings dialog (single instance). Lives on the UI thread.
//
#include <windows.h>
#include "CaptureController.h"

class TrayUI;

namespace settingsdialog
{
    // Must be called once before Show (settings changes route through the
    // tray so balloon/persist logic lives in one place).
    void SetTray(TrayUI* tray);

    // Creates the dialog or brings the existing one to the foreground.
    void Show(HINSTANCE instance, CaptureController* controller);

    // Refresh the status line / checkbox enables after a controller state
    // change or external settings change. No-op when the dialog is closed.
    void RefreshStatus();

    // Refresh the camera dropdown after a device plug/unplug. Cheap no-op when
    // the dialog is closed or the set of connected cameras is unchanged; only
    // rebuilds the list (and, if the viewed camera itself vanished, reloads its
    // controls/preview) when membership actually changed. No-op when closed.
    void RefreshCameraList();

    HWND Hwnd();
    void Close();
}
