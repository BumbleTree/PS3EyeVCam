#include "TrayUI.h"

#include <commctrl.h>
#include <shellapi.h>
#include <windowsx.h>
#include <dbt.h>
#include <cstdio>

#include "SettingsDialog.h"
#include "Autostart.h"
#include "../common/VCamGuids.h"
#include "../res/resource.h"

namespace
{
constexpr UINT kTrayIconId = 1;

// DeviceInterfaceGUIDs value installed by driver/usb_device.inf for the
// PS3 Eye's WinUSB video interface (MI_00). Arrival/removal of this
// interface is exactly the moment libusb can/can't open the camera.
constexpr GUID kPs3EyeInterfaceGuid =
    { 0x4bcc4c51, 0x4249, 0x4ae8, { 0x98, 0xbf, 0x35, 0x6b, 0x2c, 0x53, 0x0e, 0x77 } };
}

bool TrayUI::Create(HINSTANCE instance, CaptureController* controllers)
{
    _instance = instance;
    _controllers = controllers;
    _controller = &controllers[0];

    LoadIconMetric(instance, MAKEINTRESOURCEW(IDI_APP), LIM_SMALL, &_icon);

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProcThunk;
    wc.hInstance = instance;
    wc.lpszClassName = kWindowClass;
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP));
    RegisterClassW(&wc);

    // A normal (never-shown) top-level window, NOT a message-only window:
    // broadcast messages like TaskbarCreated are only delivered to top-level
    // windows.
    _hwnd = CreateWindowExW(0, kWindowClass, L"PS3 Eye Virtual Camera",
                            WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr, instance, this);
    if (!_hwnd)
        return false;

    // Explorer (medium IL) broadcasts TaskbarCreated; without this filter the
    // elevated process never sees it and the icon dies on Explorer restarts.
    _taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");
    ChangeWindowMessageFilterEx(_hwnd, _taskbarCreatedMsg, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(_hwnd, WM_SHOW_SETTINGS, MSGFLT_ALLOW, nullptr);

    // Subscribe to PS3 Eye interface arrival/removal so the capture threads
    // can re-evaluate slot occupancy immediately instead of polling.
    DEV_BROADCAST_DEVICEINTERFACE_W filter{};
    filter.dbcc_size = sizeof(filter);
    filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    filter.dbcc_classguid = kPs3EyeInterfaceGuid;
    _devNotify = RegisterDeviceNotificationW(_hwnd, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);

    AddTrayIcon();
    return true;
}

void TrayUI::Destroy()
{
    RemoveTrayIcon();
    if (_devNotify)
    {
        UnregisterDeviceNotification(_devNotify);
        _devNotify = nullptr;
    }
    if (_hwnd)
    {
        DestroyWindow(_hwnd);
        _hwnd = nullptr;
    }
}

void TrayUI::AddTrayIcon()
{
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = _hwnd;
    nid.uID = kTrayIconId;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = _icon;
    wcscpy_s(nid.szTip, L"PS3 Eye Virtual Camera");
    Shell_NotifyIconW(_iconAdded ? NIM_MODIFY : NIM_ADD, &nid);
    if (!_iconAdded)
    {
        nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &nid);
        _iconAdded = true;
    }
    UpdateTooltip();
}

void TrayUI::RemoveTrayIcon()
{
    if (!_iconAdded)
        return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = _hwnd;
    nid.uID = kTrayIconId;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    _iconAdded = false;
}

void TrayUI::UpdateTooltip()
{
    if (!_iconAdded)
        return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = _hwnd;
    nid.uID = kTrayIconId;
    nid.uFlags = NIF_TIP | NIF_SHOWTIP;

    int streamingCount = 0;
    int asleepCount = 0;
    int failedCount = 0;
    float maxFps = 0.0f;

    for (int i = 0; i < kVCamCount; ++i)
    {
        switch (_controllers[i].GetState())
        {
        case CaptureController::State::Streaming:
            streamingCount++;
            if (_controllers[i].MeasuredFpsX10() / 10.0f > maxFps)
                maxFps = _controllers[i].MeasuredFpsX10() / 10.0f;
            break;
        case CaptureController::State::Asleep:
            asleepCount++;
            break;
        case CaptureController::State::VCamFailed:
            failedCount++;
            break;
        default:
            break;
        }
    }

    if (streamingCount > 0)
    {
        swprintf_s(nid.szTip, L"PS3 Eye — %d streaming (max %.1f fps), %d idle",
                   streamingCount, maxFps, asleepCount);
    }
    else if (failedCount > 0)
    {
        wcscpy_s(nid.szTip, L"PS3 Eye — virtual camera error (retrying)");
    }
    else if (asleepCount > 0)
    {
        swprintf_s(nid.szTip, L"PS3 Eye — %d idle (camera sleeping)", asleepCount);
    }
    else
    {
        wcscpy_s(nid.szTip, L"PS3 Eye — no cameras detected");
    }

    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void TrayUI::ShowBalloon(const wchar_t* title, const wchar_t* text)
{
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = _hwnd;
    nid.uID = kTrayIconId;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO | NIIF_RESPECT_QUIET_TIME;
    wcscpy_s(nid.szInfoTitle, title);
    wcscpy_s(nid.szInfo, text);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void TrayUI::ApplySettings(int cameraIndex, const Settings& s, bool persistNow)
{
    if (cameraIndex < 0 || cameraIndex >= kVCamCount)
        return;
    _controllers[cameraIndex].UpdateSettings(s);
    if (persistNow)
        settings::Save(cameraIndex, s);

    if (_controllers[cameraIndex].GetState() == CaptureController::State::Streaming && !s.SameMode(_controllers[cameraIndex].ActiveSettings()))
    {
        ShowBalloon(L"Mode change queued",
                    L"The new video mode will apply when no app is using the camera.");
    }
}

void TrayUI::ShowContextMenu(POINT anchor)
{
    HMENU menu = CreatePopupMenu();
    const Settings s = settings::Load(0);
    const int activeMode = settings::FindModeIndex(s.width, s.height, s.fps);

    HMENU modeMenu = CreatePopupMenu();
    for (int i = 0; i < kVideoModeCount; ++i)
    {
        wchar_t item[48];
        swprintf_s(item, L"%u x %u @ %u fps", kVideoModes[i].width,
                   kVideoModes[i].height, kVideoModes[i].fps);
        AppendMenuW(modeMenu, MF_STRING | (i == activeMode ? MF_CHECKED : 0),
                    IDM_MODE_BASE + i, item);
    }

    AppendMenuW(menu, MF_STRING, IDM_SETTINGS, L"&Settings…");
    SetMenuDefaultItem(menu, IDM_SETTINGS, FALSE);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    // Quick toggles act on camera 0 only; per-camera control is in Settings.
    AppendMenuW(menu, MF_STRING | MF_DISABLED, 0, kVCamFriendlyNames[0]);
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(modeMenu), L"Video &mode");
    AppendMenuW(menu, MF_STRING | (s.flipH ? MF_CHECKED : 0), IDM_FLIPH, L"Flip &horizontally");
    AppendMenuW(menu, MF_STRING | (s.flipV ? MF_CHECKED : 0), IDM_FLIPV, L"Flip &vertically");
    AppendMenuW(menu, MF_STRING | (s.autoGain ? MF_CHECKED : 0), IDM_AUTOGAIN, L"&Auto gain && exposure");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (autostart::IsEnabled() ? MF_CHECKED : 0),
                IDM_AUTOSTART, L"Start with &Windows");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"E&xit");

    // Classic tray-menu dance: foreground first or the menu won't dismiss.
    SetForegroundWindow(_hwnd);
    TrackPopupMenuEx(menu, TPM_RIGHTBUTTON, anchor.x, anchor.y, _hwnd, nullptr);
    PostMessageW(_hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);  // also destroys the submenu
}

void TrayUI::OnCommand(int id)
{
    if (id >= IDM_MODE_BASE && id < IDM_MODE_BASE + kVideoModeCount)
    {
        Settings s = settings::Load(0);
        const VideoMode& m = kVideoModes[id - IDM_MODE_BASE];
        s.width = m.width; s.height = m.height; s.fps = m.fps;
        ApplySettings(0, s, true);
        settingsdialog::RefreshStatus();
        return;
    }

    switch (id)
    {
    case IDM_SETTINGS:
        settingsdialog::Show(_instance, _controller);
        break;
    case IDM_FLIPH:
    {
        Settings s = settings::Load(0);
        s.flipH = !s.flipH;
        ApplySettings(0, s, true);
        break;
    }
    case IDM_FLIPV:
    {
        Settings s = settings::Load(0);
        s.flipV = !s.flipV;
        ApplySettings(0, s, true);
        break;
    }
    case IDM_AUTOGAIN:
    {
        Settings s = settings::Load(0);
        s.autoGain = !s.autoGain;
        ApplySettings(0, s, true);
        break;
    }
    case IDM_AUTOSTART:
        if (autostart::IsEnabled())
            autostart::Disable();
        else if (!autostart::Enable())
            ShowBalloon(L"PS3 Eye Camera", L"Could not update the scheduled task.");
        break;
    case IDM_EXIT:
        PostQuitMessage(0);
        break;
    }
}

LRESULT CALLBACK TrayUI::WndProcThunk(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    TrayUI* self;
    if (msg == WM_NCCREATE)
    {
        self = static_cast<TrayUI*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->_hwnd = hwnd;
    }
    else
    {
        self = reinterpret_cast<TrayUI*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    return self ? self->WndProc(hwnd, msg, wParam, lParam)
                : DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT TrayUI::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == _taskbarCreatedMsg && _taskbarCreatedMsg != 0)
    {
        _iconAdded = false;  // Explorer restarted: the icon is gone, re-add
        AddTrayIcon();
        return 0;
    }

    switch (msg)
    {
    case WM_COMMAND:   // context-menu selections from TrackPopupMenuEx
        OnCommand(LOWORD(wParam));
        return 0;

    case WM_TRAY:
        switch (LOWORD(lParam))
        {
        case NIN_SELECT:
        case NIN_KEYSELECT:
        case WM_LBUTTONDBLCLK:
            settingsdialog::Show(_instance, _controller);
            break;
        case WM_CONTEXTMENU:
        {
            POINT pt{ GET_X_LPARAM(wParam), GET_Y_LPARAM(wParam) };
            ShowContextMenu(pt);
            break;
        }
        }
        return 0;

    case WM_DEVICECHANGE:
        if ((wParam == DBT_DEVICEARRIVAL || wParam == DBT_DEVICEREMOVECOMPLETE) && lParam)
        {
            const auto* hdr = reinterpret_cast<const DEV_BROADCAST_HDR*>(lParam);
            if (hdr->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE && _controllers)
            {
                for (int i = 0; i < kVCamCount; ++i)
                    _controllers[i].NotifyDeviceChange();
            }
        }
        return TRUE;

    case WM_CONTROLLER_STATE:
        UpdateTooltip();
        settingsdialog::RefreshStatus();
        if (static_cast<CaptureController::State>(wParam) == CaptureController::State::Fatal)
        {
            ShowBalloon(L"PS3 Eye Camera",
                        L"A fatal error occurred — the virtual camera is not available.");
        }
        return 0;

    case WM_SHOW_SETTINGS:  // second app instance launched
        settingsdialog::Show(_instance, _controller);
        return 0;

    case WM_TIMER:
        if (wParam == 1)
            UpdateTooltip();  // periodic fps refresh while streaming
        return 0;

    case WM_CREATE:
        SetTimer(hwnd, 1, 3000, nullptr);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, 1);
        // If the tray window dies for any reason the app must exit with it
        // (redundant during normal shutdown, where the loop has already left).
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
