#include "SettingsDialog.h"

#include <commctrl.h>
#include <cstdio>
#include <memory>

#include "TrayUI.h"
#include "Autostart.h"
#include "CameraPreview.h"
#include "DeviceRegistry.h"
#include "../common/VCamGuids.h"
#include "../res/resource.h"

namespace
{

HWND               g_dlg = nullptr;
CaptureController* g_controller = nullptr;
HINSTANCE          g_instance = nullptr;
bool               g_suppressNotifications = false;  // while programmatically setting controls
int                g_selectedCameraIndex = 0;
// One bit per slot: set when that slot currently holds a camera, as last
// reflected in the camera combo. Lets RefreshCameraList() skip rebuilds during
// ordinary Asleep/Streaming churn (which doesn't change membership) and only
// touch the dropdown when a device is actually plugged in or removed.
unsigned           g_comboSlotMask = 0;

// Per-selected-camera capability state, refreshed by LoadControls from the
// occupying device's DeviceProfile. The video-mode combo lists g_modeList (the
// device's own modes, not the global PS3 table), and g_controlMask gates which
// sensor controls are enabled. Defaults are the PS3 set so an empty slot still
// shows a sensible dialog.
const VideoMode*   g_modeList    = kVideoModes;
int                g_modeCount   = kVideoModeCount;
uint32_t           g_controlMask =
    CTRL_FLIP | CTRL_GAIN | CTRL_WHITEBAL | CTRL_WB_MANUAL | CTRL_TESTPATTERN;

// Index of (w,h,fps) within the active g_modeList, or -1.
int FindModeInList(uint32_t w, uint32_t h, uint32_t fps)
{
    for (int i = 0; i < g_modeCount; ++i)
        if (g_modeList[i].width == w && g_modeList[i].height == h && g_modeList[i].fps == fps)
            return i;
    return -1;
}

constexpr UINT_PTR kPersistTimerId = 1;
constexpr UINT     kPersistDelayMs = 500;
constexpr UINT_PTR kStatusTimerId  = 2;

TrayUI* g_tray = nullptr;  // for ApplySettings routing (set via Show)

// Live preview widget. Constructed in WM_INITDIALOG, destroyed in WM_DESTROY.
// While it exists it owns a worker thread that blocks on the FrameBus
// FrameReady event and a DIB section the worker fills with converted BGRA.
// While null (dialog closed) there is no thread and no Reader mapping — the
// zero-overhead-when-closed guarantee.
std::unique_ptr<CameraPreview> g_preview;

Settings SnapshotFromControls(HWND dlg)
{
    Settings s = (g_controller + g_selectedCameraIndex)->ActiveSettings();  // carries idleTimeout etc.

    const int sel = static_cast<int>(SendDlgItemMessageW(dlg, IDC_MODECOMBO, CB_GETCURSEL, 0, 0));
    if (sel >= 0 && sel < g_modeCount)
    {
        s.width  = g_modeList[sel].width;
        s.height = g_modeList[sel].height;
        s.fps    = g_modeList[sel].fps;
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

    s.brightness   = static_cast<uint32_t>(SendDlgItemMessageW(dlg, IDC_BRIGHTNESS_SLIDER, TBM_GETPOS, 0, 0));
    s.saturation   = static_cast<uint32_t>(SendDlgItemMessageW(dlg, IDC_SATURATION_SLIDER, TBM_GETPOS, 0, 0));

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

    swprintf_s(buf, L"%d", static_cast<int>(SendDlgItemMessageW(dlg, IDC_BRIGHTNESS_SLIDER, TBM_GETPOS, 0, 0)));
    SetDlgItemTextW(dlg, IDC_BRIGHTNESS_LABEL, buf);
    swprintf_s(buf, L"%d", static_cast<int>(SendDlgItemMessageW(dlg, IDC_SATURATION_SLIDER, TBM_GETPOS, 0, 0)));
    SetDlgItemTextW(dlg, IDC_SATURATION_LABEL, buf);
}

void SetSliderEnabled(HWND dlg, int sliderId, int labelId, bool enabled)
{
    EnableWindow(GetDlgItem(dlg, sliderId), enabled);
    EnableWindow(GetDlgItem(dlg, labelId), enabled);
}

void UpdateEnables(HWND dlg)
{
    // A control is interactive only if the selected device backs it
    // (g_controlMask) AND the auto/manual interlock allows it. Controls a device
    // doesn't support are greyed so users only touch settings that take effect.
    const bool hasFlip = (g_controlMask & CTRL_FLIP) != 0;
    EnableWindow(GetDlgItem(dlg, IDC_FLIPH), hasFlip);
    EnableWindow(GetDlgItem(dlg, IDC_FLIPV), hasFlip);

    const bool hasGain = (g_controlMask & CTRL_GAIN) != 0;
    EnableWindow(GetDlgItem(dlg, IDC_AUTOGAIN), hasGain);
    const bool manualGain = hasGain && IsDlgButtonChecked(dlg, IDC_AUTOGAIN) != BST_CHECKED;
    SetSliderEnabled(dlg, IDC_GAIN_SLIDER, IDC_GAIN_LABEL, manualGain);
    SetSliderEnabled(dlg, IDC_EXPOSURE_SLIDER, IDC_EXPOSURE_LABEL, manualGain);

    EnableWindow(GetDlgItem(dlg, IDC_AWB), (g_controlMask & CTRL_WHITEBAL) != 0);
    // Manual R/G/B is a separate capability from the AWB toggle: a device can
    // offer auto-WB on/off without manual per-channel gains (the EyeToy's
    // OV7648 has no R/G/B gain registers wired). The sliders are live only when
    // the device backs manual WB AND auto-WB is currently off.
    const bool manualWb = (g_controlMask & CTRL_WB_MANUAL) != 0 &&
                          IsDlgButtonChecked(dlg, IDC_AWB) != BST_CHECKED;
    SetSliderEnabled(dlg, IDC_RED_SLIDER, IDC_RED_LABEL, manualWb);
    SetSliderEnabled(dlg, IDC_BLUE_SLIDER, IDC_BLUE_LABEL, manualWb);
    SetSliderEnabled(dlg, IDC_GREEN_SLIDER, IDC_GREEN_LABEL, manualWb);

    EnableWindow(GetDlgItem(dlg, IDC_TESTPATTERN), (g_controlMask & CTRL_TESTPATTERN) != 0);

    SetSliderEnabled(dlg, IDC_BRIGHTNESS_SLIDER, IDC_BRIGHTNESS_LABEL,
                     (g_controlMask & CTRL_BRIGHTNESS) != 0);
    SetSliderEnabled(dlg, IDC_SATURATION_SLIDER, IDC_SATURATION_LABEL,
                     (g_controlMask & CTRL_SATURATION) != 0);
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
        wcscpy_s(text, L"No camera detected — the virtual camera is hidden until one is plugged in.");
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

    if (activeController->HasPendingModeChange() && !activeController->IsPreviewOnly())
        wcscat_s(text, L"\nNew mode applies when no app is using the camera.");

    SetDlgItemTextW(dlg, IDC_STATUS_TEXT, text);
}

// Bitmask of slots that currently hold a camera (state != CameraMissing).
// Cheap (atomic state reads only); used to detect membership changes without
// rebuilding the dropdown.
unsigned ConnectedSlotMask()
{
    unsigned mask = 0;
    for (int i = 0; i < kVCamCount; ++i)
        if ((g_controller + i)->GetState() != CaptureController::State::CameraMissing)
            mask |= (1u << i);
    return mask;
}

// (Re)builds IDC_CAMCOMBO to list ONLY the slots that currently hold a physical
// camera (the virtual camera is registered only while a device is attached, so
// an empty slot has nothing to configure — and the user can't predict which
// camera would ever occupy it). Connected cameras therefore appear contiguously
// regardless of their internal slot index, which is what keeps a PS3 Eye and an
// EyeToy next to each other. The TRUE slot index — which keys the CLSID, the
// per-slot settings, and the controller array — is carried in item data, so the
// combo's display order is independent of it. Updates g_comboSlotMask and, when
// the prior selection is gone, repoints g_selectedCameraIndex at a live slot.
// Caller owns g_suppressNotifications.
void PopulateCameraCombo(HWND dlg)
{
    SendDlgItemMessageW(dlg, IDC_CAMCOMBO, CB_RESETCONTENT, 0, 0);
    unsigned mask = 0;
    int connectedCount = 0;
    int selItem = -1;
    for (int i = 0; i < kVCamCount; ++i)
    {
        if ((g_controller + i)->GetState() == CaptureController::State::CameraMissing)
            continue;  // no physical camera in this slot — omit it
        mask |= (1u << i);
        wchar_t name[48];
        deviceregistry::SlotDisplayName(i, name, 48);
        const int item = static_cast<int>(SendDlgItemMessageW(
            dlg, IDC_CAMCOMBO, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name)));
        SendDlgItemMessageW(dlg, IDC_CAMCOMBO, CB_SETITEMDATA, item, i);
        if (i == g_selectedCameraIndex)
            selItem = item;
        ++connectedCount;
    }
    g_comboSlotMask = mask;

    if (connectedCount == 0)
    {
        // Nothing attached: one inert placeholder so the combo isn't blank.
        // Item data -1 marks it as "not a slot" for the selection handler.
        const int item = static_cast<int>(SendDlgItemMessageW(
            dlg, IDC_CAMCOMBO, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(L"No camera connected")));
        SendDlgItemMessageW(dlg, IDC_CAMCOMBO, CB_SETITEMDATA, item, -1);
        SendDlgItemMessageW(dlg, IDC_CAMCOMBO, CB_SETCURSEL, item, 0);
        return;
    }
    // If the previously-selected slot is no longer listed (e.g. it was
    // unplugged), fall back to the first connected camera so callers reflect a
    // real device.
    if (selItem < 0)
    {
        selItem = 0;
        g_selectedCameraIndex = static_cast<int>(
            SendDlgItemMessageW(dlg, IDC_CAMCOMBO, CB_GETITEMDATA, 0, 0));
    }
    SendDlgItemMessageW(dlg, IDC_CAMCOMBO, CB_SETCURSEL, selItem, 0);
}

void LoadControls(HWND dlg)
{
    g_suppressNotifications = true;

    PopulateCameraCombo(dlg);

    // Video modes + which sensor controls are interactive come from the device
    // occupying this slot, so an EyeToy shows its own modes (and no PS3-only
    // controls), not the global PS3 table. Empty slots fall back to the PS3 set.
    const DeviceProfile* prof = deviceregistry::ProfileForSlot(g_selectedCameraIndex);
    if (prof)
    {
        g_modeList    = prof->modes;
        g_modeCount   = static_cast<int>(prof->modeCount);
        g_controlMask = prof->controlMask;
    }
    else
    {
        g_modeList    = kVideoModes;
        g_modeCount   = kVideoModeCount;
        g_controlMask =
            CTRL_FLIP | CTRL_GAIN | CTRL_WHITEBAL | CTRL_WB_MANUAL | CTRL_TESTPATTERN;
    }

    SendDlgItemMessageW(dlg, IDC_MODECOMBO, CB_RESETCONTENT, 0, 0);
    for (int i = 0; i < g_modeCount; ++i)
    {
        wchar_t item[48];
        swprintf_s(item, L"%u x %u  @  %u fps", g_modeList[i].width,
                   g_modeList[i].height, g_modeList[i].fps);
        SendDlgItemMessageW(dlg, IDC_MODECOMBO, CB_ADDSTRING, 0,
                            reinterpret_cast<LPARAM>(item));
    }

    const Settings s = settings::Load(g_selectedCameraIndex);
    int sel = FindModeInList(s.width, s.height, s.fps);
    if (sel < 0 && prof)  // saved mode isn't one this device serves: use its default
        sel = FindModeInList(prof->defaultMode.width, prof->defaultMode.height, prof->defaultMode.fps);
    if (sel < 0) sel = 0;
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

    SendDlgItemMessageW(dlg, IDC_BRIGHTNESS_SLIDER, TBM_SETRANGE, TRUE, MAKELPARAM(0, 255));
    SendDlgItemMessageW(dlg, IDC_BRIGHTNESS_SLIDER, TBM_SETTICFREQ, 32, 0);
    SendDlgItemMessageW(dlg, IDC_BRIGHTNESS_SLIDER, TBM_SETPOS, TRUE, s.brightness);
    SendDlgItemMessageW(dlg, IDC_SATURATION_SLIDER, TBM_SETRANGE, TRUE, MAKELPARAM(0, 255));
    SendDlgItemMessageW(dlg, IDC_SATURATION_SLIDER, TBM_SETTICFREQ, 32, 0);
    SendDlgItemMessageW(dlg, IDC_SATURATION_SLIDER, TBM_SETPOS, TRUE, s.saturation);

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
    Settings defaults = settings::Defaults();
    // Use the occupying device's default mode so resetting an EyeToy doesn't
    // write the PS3's 640x480@60 (which the EyeToy can't serve).
    if (const DeviceProfile* prof = deviceregistry::ProfileForSlot(g_selectedCameraIndex))
    {
        defaults.width  = prof->defaultMode.width;
        defaults.height = prof->defaultMode.height;
        defaults.fps    = prof->defaultMode.fps;
    }
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

        // Spin up the live preview. The worker thread opens a read-only
        // FrameBus mapping for the selected camera and blocks on its
        // FrameReady event; a per-camera preview-hold atomic keeps the
        // CaptureController streaming while the dialog is open. All of this
        // is torn down in WM_DESTROY so nothing runs while the dialog is
        // closed.
        g_preview = std::make_unique<CameraPreview>();
        g_preview->SetControllerArray(g_controller);
        g_preview->Attach(dlg, IDC_PREVIEW, g_instance);
        g_preview->SetCamera(g_selectedCameraIndex);  // also sets preview hold

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
                // The chosen row's item data is the true slot index (not its
                // position, since the combo lists only connected cameras). -1 is
                // the "No camera connected" placeholder — ignore it.
                const int item = static_cast<int>(SendDlgItemMessageW(dlg, IDC_CAMCOMBO, CB_GETCURSEL, 0, 0));
                const int slot = static_cast<int>(SendDlgItemMessageW(dlg, IDC_CAMCOMBO, CB_GETITEMDATA, item, 0));
                if (slot < 0 || slot == g_selectedCameraIndex)
                    return TRUE;
                // Flush a pending debounced save for the camera we're leaving;
                // otherwise the timer fires after the index changed and the
                // previous camera's last slider tweaks are silently dropped.
                if (KillTimer(dlg, kPersistTimerId))
                    settings::Save(g_selectedCameraIndex, SnapshotFromControls(dlg));
                g_selectedCameraIndex = slot;
                LoadControls(dlg);

                // Re-target the preview at the newly selected camera. This
                // also toggles the per-camera preview-hold flag off on the
                // old slot and on for the new one.
                if (g_preview)
                    g_preview->SetCamera(g_selectedCameraIndex);
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
                                L"PSCam4Win", MB_ICONWARNING | MB_OK);
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

        // Tear down the preview first: joins the worker thread, releases the
        // DIB, and clears the per-camera preview-hold flag so the
        // CaptureController can go back to sleep. After this returns there is
        // no preview thread and no Reader mapping — the zero-overhead-when-
        // closed guarantee.
        if (g_preview)
            g_preview.reset();

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

void RefreshCameraList()
{
    // Called on every controller state change. Cheap no-op unless the set of
    // connected cameras actually changed — ordinary Asleep<->Streaming churn
    // doesn't touch the dropdown, only a physical plug/unplug does.
    if (!g_dlg)
        return;
    const unsigned now = ConnectedSlotMask();
    if (now == g_comboSlotMask)
        return;  // membership unchanged — nothing to refresh

    // Keep the current view (refresh only the dropdown membership) when the
    // camera being shown was already a real connected device (its bit set in the
    // OLD mask) and still is. That deliberately excludes the placeholder/empty ->
    // camera-arrived case, which needs a full reload of that slot's modes and
    // controls.
    const bool selValid = g_selectedCameraIndex >= 0 && g_selectedCameraIndex < kVCamCount;
    const bool keepView = selValid &&
        (g_comboSlotMask & (1u << g_selectedCameraIndex)) &&
        (now            & (1u << g_selectedCameraIndex));

    if (keepView)
    {
        // A camera was added or removed in some OTHER slot: rebuild just the
        // list. The viewed camera's controls, mode list, and live preview are
        // left untouched.
        g_suppressNotifications = true;
        PopulateCameraCombo(g_dlg);
        g_suppressNotifications = false;
    }
    else
    {
        // The viewed camera was unplugged (or the first camera just arrived):
        // flush any debounced save for it, then point the dialog at a live slot
        // (or the placeholder) and reload its controls + preview.
        if (KillTimer(g_dlg, kPersistTimerId))
            settings::Save(g_selectedCameraIndex, SnapshotFromControls(g_dlg));
        LoadControls(g_dlg);
        if (g_preview)
            g_preview->SetCamera(g_selectedCameraIndex);
    }
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
