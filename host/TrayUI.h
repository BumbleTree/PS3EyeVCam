#pragma once
//
// System tray icon + context menu + hidden message window. UI thread only.
//
#include <windows.h>
#include "CaptureController.h"

class TrayUI
{
public:
    static constexpr UINT WM_TRAY             = WM_APP + 1;  // Shell_NotifyIcon callback
    static constexpr UINT WM_CONTROLLER_STATE = WM_APP + 2;  // posted by CaptureController
    static constexpr UINT WM_SHOW_SETTINGS    = WM_APP + 3;  // posted by a second instance

    static constexpr wchar_t kWindowClass[] = L"PS3EyeVCamTrayWnd";

    bool Create(HINSTANCE instance, CaptureController* controllers);
    void Destroy();
    HWND Hwnd() const { return _hwnd; }

    // Apply a settings snapshot to the controller and persist it; shows the
    // "mode change deferred" balloon when applicable. Used by both the menu
    // and the settings dialog.
    void ApplySettings(int cameraIndex, const Settings& s, bool persistNow);

private:
    static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(HWND, UINT, WPARAM, LPARAM);

    void AddTrayIcon();
    void RemoveTrayIcon();
    void UpdateTooltip();
    void ShowBalloon(const wchar_t* title, const wchar_t* text);
    void ShowContextMenu(POINT anchor);
    void OnCommand(int id);

    HINSTANCE          _instance = nullptr;
    HWND               _hwnd = nullptr;
    HICON              _icon = nullptr;
    CaptureController* _controllers = nullptr;
    CaptureController* _controller = nullptr;
    UINT               _taskbarCreatedMsg = 0;
    bool               _iconAdded = false;
    HDEVNOTIFY         _devNotify = nullptr;  // PS3 Eye interface arrival/removal
};
