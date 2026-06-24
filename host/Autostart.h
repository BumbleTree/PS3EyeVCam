#pragma once
//
// "Start with Windows" via a Task Scheduler logon task (\PSCam4Win).
//
// Why a scheduled task and not Run-key/Startup-folder: the app must launch
// ELEVATED with no UAC prompt. A logon task with RunLevel=Highest, created by
// an elevated process, is the supported way to do that.
//
// Why ITaskService COM and not `schtasks /create`: schtasks defaults cannot
// be overridden from its command line and are product-killers here —
// DisallowStartIfOnBatteries (camera never starts on a laptop on battery)
// and ExecutionTimeLimit=72h (the tray app gets killed after 3 days).
//
// Task existence IS the setting (no registry mirror to drift).
//
namespace autostart
{
    bool IsEnabled();
    bool Enable();    // create or update the task for the current user
    bool Disable();   // remove the task
}
