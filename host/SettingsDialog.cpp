#include "SettingsDialog.h"

#include <commctrl.h>
#include <cstdio>

#include "TrayUI.h"
#include "Autostart.h"
#include "../common/VCamGuids.h"
#include "../res/resource.h"

namespace
{

HWND               g_dlg = nullptr;
CaptureController* g_controller = nullptr;
HINSTANCE          g_instance = nullptr;
bool               g_suppressNotifications = false;  // while programmatically setting controls
int                g_selectedCameraIndex = 0;

constexpr UINT_PTR kPersistTimerId = 1;
constexpr UINT     kPersistDelayMs = 500;
constexpr UINT_PTR kStatusTimerId  = 2;

TrayUI* g_tray = nullptr;  // for ApplySettings routing (set via Show)

Settings SnapshotFromControls(HWND dlg)
{
    Settings s = (g_controller + g_selectedCameraIndex)->ActiveSettings();  // carries idleTimeout etc.

    const int sel = static_cast<int>(SendDlgItemMessageW(dlg, IDC_MODECOMBO, CB_GETCURSEL, 0, 0));
    if (sel >= 0 && sel < kVideoModeCount)
    {
        s.width  = kVideoModes[sel].width;
        s.height = kVideoModes[sel].height;
        s.fps    = kVideoModes[sel].fps;
    }
    s.flipH    = IsDlgButtonChecked(dlg, IDC_FLIPH) == BST_CHECKED;
    s.flipV    = IsDlgButtonChecked(dlg, IDC_FLIPV) == BST_CHECKED;
    s.autoGain = IsDlgButtonChecked(dlg, IDC_AUTOGAIN) == BST_CHECKED;
    s.autoWhiteBalance = IsDlgButtonChecked(dlg, IDC_AWB) == BST_CHECKED;
    s.gain     = static_cast<uint32_t>(SendDlgItemMessageW(dlg, IDC_GAIN_SLIDER, TBM_GETPOS, 0, 0));
    s.exposure = static_cast<uint32_t>(SendDlgItemMessageW(dlg, IDC_EXPOSURE_SLIDER, TBM_GETPOS, 0, 0));

    s.redBalance   = static_cast<uint32_t>(SendDlgItemMessageW(dlg, IDC_RED_SLIDER, TBM_GETPOS, 0, 0));
    s.blueBalance  = static_cast<uint32_t>(SendDlgItemMessageW(dlg, IDC_BLUE_SLIDER, TBM_GETPOS, 0, 0));
    s.greenBalance = static_cast<uint32_t>(SendDlgItemMessageW(dlg, IDC_GREEN_SLIDER, TBM_GETPOS, 0, 0));
    s.testPattern  = IsDlgButtonChecked(dlg, IDC_TESTPATTERN) == BST_CHECKED;

    return s;
}

void UpdateSliderLabels(HWND dlg)
{
    wchar_t buf[16];
    swprintf_s(buf, L"%d", static_cast<int>(SendDlgItemMessageW(dlg, IDC_GAIN_SLIDER, TBM_GETPOS, 0, 0)));
    SetDlgItemTextW(dlg, IDC_GAIN_LABEL, buf);
    swprintf_s(buf, L"%d", static_cast<int>(SendDlgItemMessageW(dlg, IDC_EXPOSURE_SLIDER, TBM_GETPOS, 0, 0)));
    SetDlgItemTextW(dlg, IDC_EXPOSURE_LABEL, buf);

    swprintf_s(buf, L"%d", static_cast<int>(SendDlgItemMessageW(dlg, IDC_RED_SLIDER, TBM_GETPOS, 0, 0)));
    SetDlgItemTextW(dlg, IDC_RED_LABEL, buf);
    swprintf_s(buf, L"%d", static_cast<int>(SendDlgItemMessageW(dlg, IDC_BLUE_SLIDER, TBM_GETPOS, 0, 0)));
    SetDlgItemTextW(dlg, IDC_BLUE_LABEL, buf);
    swprintf_s(buf, L"%d", static_cast<int>(SendDlgItemMessageW(dlg, IDC_GREEN_SLIDER, TBM_GETPOS, 0, 0)));
    SetDlgItemTextW(dlg, IDC_GREEN_LABEL, buf);
}

void SetSliderEnabled(HWND dlg, int sliderId, int labelId, bool enabled)
{
    EnableWindow(GetDlgItem(dlg, sliderId), enabled);
    EnableWindow(GetDlgItem(dlg, labelId), enabled);
}

void UpdateEnables(HWND dlg)
{
    const bool manualGain = IsDlgButtonChecked(dlg, IDC_AUTOGAIN) != BST_CHECKED;
    SetSliderEnabled(dlg, IDC_GAIN_SLIDER, IDC_GAIN_LABEL, manualGain);
    SetSliderEnabled(dlg, IDC_EXPOSURE_SLIDER, IDC_EXPOSURE_LABEL, manualGain);

    const bool manualWb = IsDlgButtonChecked(dlg, IDC_AWB) != BST_CHECKED;
    SetSliderEnabled(dlg, IDC_RED_SLIDER, IDC_RED_LABEL, manualWb);
    SetSliderEnabled(dlg, IDC_BLUE_SLIDER, IDC_BLUE_LABEL, manualWb);
    SetSliderEnabled(dlg, IDC_GREEN_SLIDER, IDC_GREEN_LABEL, manualWb);
}

void UpdateStatusText(HWND dlg)
{
    wchar_t text[160] = L"";
    CaptureController* activeController = g_controller + g_selectedCameraIndex;
    const Settings s = activeController->ActiveSettings();
    switch (activeController->GetState())
    {
    case CaptureController::State::Streaming:
        swprintf_s(text, L"Streaming %ux%u — %.1f fps captured.",
                   s.width, s.height, activeController->MeasuredFpsX10() / 10.0);
        break;
    case CaptureController::State::Asleep:
        wcscpy_s(text, L"Idle — camera sleeping (LED off) until an app opens it.");
        break;
    case CaptureController::State::Waking:
        wcscpy_s(text, L"Waking camera…");
        break;
    case CaptureController::State::CameraMissing:
        wcscpy_s(text, L"PS3 Eye not detected — the virtual camera is hidden until it is plugged in.");
        break;
    case CaptureController::State::VCamFailed:
        wcscpy_s(text, L"Virtual camera registration failing — retrying…");
        break;
    case CaptureController::State::Fatal:
        wcscpy_s(text, L"Fatal error — see debug log.");
        break;
    default:
        wcscpy_s(text, L"Starting…");
        break;
    }

    if (activeController->HasPendingModeChange())
        wcscat_s(text, L"\nNew mode applies when no app is using the camera.");

    SetDlgItemTextW(dlg, IDC_STATUS_TEXT, text);
}

void LoadControls(HWND dlg)
{
    g_suppressNotifications = true;

    // Rebuilt on every load: the friendly names match what apps see, and the
    // annotation tracks which slots currently have a physical camera (the
    // virtual camera is only registered while the device is attached).
    SendDlgItemMessageW(dlg, IDC_CAMCOMBO, CB_RESETCONTENT, 0, 0);
    for (int i = 0; i < kVCamCount; ++i)
    {
        wchar_t item[64];
        const bool detached =
            (g_controller + i)->GetState() == CaptureController::State::CameraMissing;
        swprintf_s(item, L"%s%s", kVCamFriendlyNames[i],
                   detached ? L"  (not connected)" : L"");
        SendDlgItemMessageW(dlg, IDC_CAMCOMBO, CB_ADDSTRING, 0,
                            reinterpret_cast<LPARAM>(item));
    }
    SendDlgItemMessageW(dlg, IDC_CAMCOMBO, CB_SETCURSEL, g_selectedCameraIndex, 0);

    SendDlgItemMessageW(dlg, IDC_MODECOMBO, CB_RESETCONTENT, 0, 0);
    for (int i = 0; i < kVideoModeCount; ++i)
    {
        wchar_t item[48];
        swprintf_s(item, L"%u x %u  @  %u fps", kVideoModes[i].width,
                   kVideoModes[i].height, kVideoModes[i].fps);
        SendDlgItemMessageW(dlg, IDC_MODECOMBO, CB_ADDSTRING, 0,
                            reinterpret_cast<LPARAM>(item));
    }

    const Settings s = settings::Load(g_selectedCameraIndex);
    int sel = settings::FindModeIndex(s.width, s.height, s.fps);
    if (sel < 0) sel = kDefaultModeIndex;
    SendDlgItemMessageW(dlg, IDC_MODECOMBO, CB_SETCURSEL, sel, 0);

    CheckDlgButton(dlg, IDC_FLIPH, s.flipH ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dlg, IDC_FLIPV, s.flipV ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dlg, IDC_AUTOGAIN, s.autoGain ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dlg, IDC_AWB, s.autoWhiteBalance ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dlg, IDC_TESTPATTERN, s.testPattern ? BST_CHECKED : BST_UNCHECKED);

    SendDlgItemMessageW(dlg, IDC_GAIN_SLIDER, TBM_SETRANGE, TRUE, MAKELPARAM(0, 63));
    SendDlgItemMessageW(dlg, IDC_GAIN_SLIDER, TBM_SETTICFREQ, 8, 0);
    SendDlgItemMessageW(dlg, IDC_GAIN_SLIDER, TBM_SETPOS, TRUE, s.gain);
    SendDlgItemMessageW(dlg, IDC_EXPOSURE_SLIDER, TBM_SETRANGE, TRUE, MAKELPARAM(0, 255));
    SendDlgItemMessageW(dlg, IDC_EXPOSURE_SLIDER, TBM_SETTICFREQ, 32, 0);
    SendDlgItemMessageW(dlg, IDC_EXPOSURE_SLIDER, TBM_SETPOS, TRUE, s.exposure);

    SendDlgItemMessageW(dlg, IDC_RED_SLIDER, TBM_SETRANGE, TRUE, MAKELPARAM(0, 255));
    SendDlgItemMessageW(dlg, IDC_RED_SLIDER, TBM_SETTICFREQ, 32, 0);
    SendDlgItemMessageW(dlg, IDC_RED_SLIDER, TBM_SETPOS, TRUE, s.redBalance);
    SendDlgItemMessageW(dlg, IDC_BLUE_SLIDER, TBM_SETRANGE, TRUE, MAKELPARAM(0, 255));
    SendDlgItemMessageW(dlg, IDC_BLUE_SLIDER, TBM_SETTICFREQ, 32, 0);
    SendDlgItemMessageW(dlg, IDC_BLUE_SLIDER, TBM_SETPOS, TRUE, s.blueBalance);
    SendDlgItemMessageW(dlg, IDC_GREEN_SLIDER, TBM_SETRANGE, TRUE, MAKELPARAM(0, 255));
    SendDlgItemMessageW(dlg, IDC_GREEN_SLIDER, TBM_SETTICFREQ, 32, 0);
    SendDlgItemMessageW(dlg, IDC_GREEN_SLIDER, TBM_SETPOS, TRUE, s.greenBalance);

    CheckDlgButton(dlg, IDC_AUTOSTART, autostart::IsEnabled() ? BST_CHECKED : BST_UNCHECKED);

    UpdateSliderLabels(dlg);
    UpdateEnables(dlg);
    UpdateStatusText(dlg);

    g_suppressNotifications = false;
}

// Live-apply (sliders move the sensor immediately); registry write is
// debounced via the persist timer so HKLM isn't hammered per tick.
void ApplyLive(HWND dlg)
{
    if (g_suppressNotifications)
        return;
    g_tray->ApplySettings(g_selectedCameraIndex, SnapshotFromControls(dlg), false);
    SetTimer(dlg, kPersistTimerId, kPersistDelayMs, nullptr);
}

void ApplyAndPersist(HWND dlg)
{
    if (g_suppressNotifications)
        return;
    g_tray->ApplySettings(g_selectedCameraIndex, SnapshotFromControls(dlg), true);
    UpdateStatusText(dlg);
}

void ResetToDefaults(HWND dlg)
{
    const Settings defaults = settings::Defaults();
    settings::Save(g_selectedCameraIndex, defaults);
    LoadControls(dlg);
    g_tray->ApplySettings(g_selectedCameraIndex, defaults, false);
}

INT_PTR CALLBACK DlgProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        HICON big = static_cast<HICON>(LoadImageW(g_instance, MAKEINTRESOURCEW(IDI_APP),
                                                  IMAGE_ICON, 32, 32, 0));
        HICON small = static_cast<HICON>(LoadImageW(g_instance, MAKEINTRESOURCEW(IDI_APP),
                                                    IMAGE_ICON, 16, 16, 0));
        SendMessageW(dlg, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(big));
        SendMessageW(dlg, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(small));
        LoadControls(dlg);
        SetTimer(dlg, kStatusTimerId, 2000, nullptr);
        return TRUE;
    }

    case WM_HSCROLL:  // trackbar movement
        UpdateSliderLabels(dlg);
        ApplyLive(dlg);
        return TRUE;

    case WM_TIMER:
        if (wParam == kPersistTimerId)
        {
            KillTimer(dlg, kPersistTimerId);
            settings::Save(g_selectedCameraIndex, SnapshotFromControls(dlg));
            UpdateStatusText(dlg);
        }
        else if (wParam == kStatusTimerId)
        {
            UpdateStatusText(dlg);
        }
        return TRUE;

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDC_CAMCOMBO:
            if (HIWORD(wParam) == CBN_SELCHANGE)
            {
                // Flush a pending debounced save for the camera we're leaving;
                // otherwise the timer fires after the index changed and the
                // previous camera's last slider tweaks are silently dropped.
                if (KillTimer(dlg, kPersistTimerId))
                    settings::Save(g_selectedCameraIndex, SnapshotFromControls(dlg));
                g_selectedCameraIndex = static_cast<int>(SendDlgItemMessageW(dlg, IDC_CAMCOMBO, CB_GETCURSEL, 0, 0));
                LoadControls(dlg);
            }
            return TRUE;
        case IDC_MODECOMBO:
            if (HIWORD(wParam) == CBN_SELCHANGE)
                ApplyAndPersist(dlg);
            return TRUE;
        case IDC_FLIPH:
        case IDC_FLIPV:
        case IDC_TESTPATTERN:
            ApplyAndPersist(dlg);
            return TRUE;
        case IDC_AWB:
            UpdateEnables(dlg);
            ApplyAndPersist(dlg);
            return TRUE;
        case IDC_AUTOGAIN:
            UpdateEnables(dlg);
            ApplyAndPersist(dlg);
            return TRUE;
        case IDC_RESET:
            ResetToDefaults(dlg);
            return TRUE;
        case IDC_AUTOSTART:
            if (!g_suppressNotifications)
            {
                const bool want = IsDlgButtonChecked(dlg, IDC_AUTOSTART) == BST_CHECKED;
                const bool ok = want ? autostart::Enable() : autostart::Disable();
                if (!ok)
                {
                    CheckDlgButton(dlg, IDC_AUTOSTART, want ? BST_UNCHECKED : BST_CHECKED);
                    MessageBoxW(dlg, L"Could not update the scheduled task.",
                                L"PS3 Eye Camera", MB_ICONWARNING | MB_OK);
                }
            }
            return TRUE;
        case IDOK:
        case IDCANCEL:
            DestroyWindow(dlg);
            return TRUE;
        }
        return FALSE;

    case WM_CLOSE:
        DestroyWindow(dlg);
        return TRUE;

    case WM_DESTROY:
        // Flush a pending debounced save so quick slider-then-close persists.
        KillTimer(dlg, kStatusTimerId);
        if (KillTimer(dlg, kPersistTimerId))
            settings::Save(g_selectedCameraIndex, SnapshotFromControls(dlg));
        g_dlg = nullptr;
        return TRUE;
    }
    return FALSE;
}

} // namespace

namespace settingsdialog
{

void Show(HINSTANCE instance, CaptureController* controller)
{
    g_instance = instance;
    g_controller = controller;
    if (g_dlg)
    {
        ShowWindow(g_dlg, SW_SHOWNORMAL);
        SetForegroundWindow(g_dlg);
        return;
    }
    g_dlg = CreateDialogParamW(instance, MAKEINTRESOURCEW(IDD_SETTINGS), nullptr, DlgProc, 0);
    if (g_dlg)
    {
        ShowWindow(g_dlg, SW_SHOWNORMAL);
        SetForegroundWindow(g_dlg);
    }
}

void RefreshStatus()
{
    if (g_dlg)
        UpdateStatusText(g_dlg);
}

HWND Hwnd() { return g_dlg; }

void Close()
{
    if (g_dlg)
        DestroyWindow(g_dlg);
}

void SetTray(TrayUI* tray);  // fwd decl satisfied below

} // namespace settingsdialog

// Out-of-line setter to avoid a header cycle between TrayUI and the dialog.
namespace settingsdialog
{
    void SetTray(TrayUI* tray) { g_tray = tray; }
}
