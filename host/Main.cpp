//
// PSCam4WinTray — entry point.
//
// CLI (all run elevated via the embedded manifest):
//   (none)               start the tray app
//   --autostart          same, marker used by the logon scheduled task
//   --console            attach a debug console (state machine logging)
//   --seed-defaults      write missing registry defaults and exit (installer)
//   --enable-autostart   create the logon task and exit (installer)
//   --disable-autostart  delete the logon task and exit
//
#include <windows.h>
#include <objbase.h>
#include <commctrl.h>
#include <shellapi.h>
#include <cstdio>
#include <string>

#include "CaptureController.h"
#include "TrayUI.h"
#include "SettingsDialog.h"
#include "Autostart.h"
#include "../common/Settings.h"
#include "../common/VCamGuids.h"

namespace
{
constexpr wchar_t kSingleInstanceMutex[] = L"Global\\PSCam4Win.Tray.SingleInstance";

bool HasArg(int argc, wchar_t** argv, const wchar_t* name)
{
    for (int i = 1; i < argc; ++i)
        if (_wcsicmp(argv[i], name) == 0)
            return true;
    return false;
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    // ---- one-shot installer verbs -------------------------------------------
    if (HasArg(argc, argv, L"--seed-defaults"))
    {
        for (int i = 0; i < kVCamCount; ++i)
            settings::SeedDefaults(i);
        return 0;
    }
    if (HasArg(argc, argv, L"--enable-autostart") || HasArg(argc, argv, L"--disable-autostart"))
    {
        const bool enable = HasArg(argc, argv, L"--enable-autostart");
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        const bool ok = enable ? autostart::Enable() : autostart::Disable();
        CoUninitialize();
        return ok ? 0 : 1;
    }

    if (HasArg(argc, argv, L"--console"))
    {
        AllocConsole();
        FILE* unused;
        freopen_s(&unused, "CONOUT$", "w", stdout);
        freopen_s(&unused, "CONOUT$", "w", stderr);
    }

    // ---- single instance -----------------------------------------------------
    HANDLE mutex = CreateMutexW(nullptr, TRUE, kSingleInstanceMutex);
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        // Hand off to the running instance: pop its settings dialog.
        if (HWND existing = FindWindowW(TrayUI::kWindowClass, nullptr))
            PostMessageW(existing, TrayUI::WM_SHOW_SETTINGS, 0, 0);
        return 0;
    }

    // Refuse to run alongside a pre-rebrand PS3EyeVCam tray: it uses different
    // IPC names but registers the SAME virtual-camera CLSIDs, so two live
    // instances would collide on MFCreateVirtualCamera. The installer removes
    // the legacy app, but its logon task may still fire once before that.
    if (HANDLE legacy = OpenMutexW(SYNCHRONIZE, FALSE, L"Global\\PS3EyeVCam.Tray.SingleInstance"))
    {
        CloseHandle(legacy);
        MessageBoxW(nullptr,
                    L"An older \"PS3 Eye Virtual Camera\" is still running. "
                    L"Close it (and rerun the installer to remove it), then start PSCam4Win.",
                    L"PSCam4Win", MB_ICONWARNING | MB_OK);
        if (mutex)
            CloseHandle(mutex);
        return 0;
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);  // UI thread STA
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    for (int i = 0; i < kVCamCount; ++i)
        settings::SeedDefaults(i);  // first run on a clean machine self-initializes

    CaptureController controllers[kVCamCount];
    TrayUI tray;
    settingsdialog::SetTray(&tray);

    if (!tray.Create(instance, controllers))
    {
        MessageBoxW(nullptr, L"Failed to create the tray window.", L"PSCam4Win",
                    MB_ICONERROR | MB_OK);
        return 1;
    }

    bool anyStarted = false;
    for (int i = 0; i < kVCamCount; ++i)
    {
        if (controllers[i].Start(i, tray.Hwnd(), TrayUI::WM_CONTROLLER_STATE))
            anyStarted = true;
    }
    if (!anyStarted)
    {
        MessageBoxW(nullptr, L"Failed to start the camera threads.", L"PSCam4Win",
                    MB_ICONERROR | MB_OK);
        tray.Destroy();
        return 1;
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        if (settingsdialog::Hwnd() && IsDialogMessageW(settingsdialog::Hwnd(), &msg))
            continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // ---- ordered shutdown: UI gone first, then the camera threads -------------
    settingsdialog::Close();
    tray.Destroy();
    for (int i = 0; i < kVCamCount; ++i)
    {
        controllers[i].Stop();   // joins; tears down vcam -> camera -> shared memory
    }
    CoUninitialize();
    if (mutex)
        CloseHandle(mutex);
    return 0;
}
