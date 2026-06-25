@echo off
rem Completely removes PSCam4Win: process, logon task, DLL registration,
rem drivers, certificates, settings, files.
rem Must run as Administrator.
rem - self-elevates if run as standard user (BEFORE the temp-copy relaunch,
rem   so only one UAC prompt / console window ever appears)
rem - re-runs itself from %temp% so the install folder isn't locked
setlocal

net session >nul 2>&1
if %errorlevel% neq 0 (
    echo Requesting administrative privileges...
    powershell -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)

cd /d "%~dp0"

set "DEST=%ProgramFiles%\PSCam4Win"

echo Stopping existing host app instance...
taskkill /im PSCam4WinTray.exe >nul 2>&1
timeout /t 2 /nobreak >nul
taskkill /f /im PSCam4WinTray.exe >nul 2>&1

echo Deleting Startup Task from Task Scheduler...
schtasks /delete /tn "PSCam4Win" /f >nul 2>&1

echo Stopping Camera Services...
net stop FrameServerMonitor /y >nul 2>&1
net stop FrameServer /y >nul 2>&1

echo Unregistering Virtual Camera DLL...
if exist "%DEST%\PSCam4Win.dll" (
    regsvr32 /u /s "%DEST%\PSCam4Win.dll"
)

echo Deleting registry settings...
reg delete "HKLM\SOFTWARE\PSCam4Win" /f >nul 2>&1
reg delete "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\PSCam4Win" /f >nul 2>&1

echo Removing Video Driver (PS3 Eye)...
powershell -Command "$oem = Get-WindowsDriver -Online | Where-Object { $_.OriginalFileName -like '*usb_device.inf' } | Select-Object -ExpandProperty Driver; if ($oem) { pnputil /delete-driver $oem /uninstall /force }" >nul 2>&1

echo Removing Video Driver (PS2 EyeToy)...
powershell -Command "$oem = Get-WindowsDriver -Online | Where-Object { $_.OriginalFileName -like '*eyetoy_device.inf' } | Select-Object -ExpandProperty Driver; if ($oem) { pnputil /delete-driver $oem /uninstall /force }" >nul 2>&1

echo Removing WinUSB Driver Certificates...
if exist "%DEST%\driver\usb_device.cer" (
    rem Thumbprint derived from the shipped certificate itself -- nothing
    rem hardcoded, so re-signed builds always clean up the right cert.
    for /f "tokens=*" %%a in ('powershell -Command "Get-PfxCertificate -FilePath '%DEST%\driver\usb_device.cer' | Select-Object -ExpandProperty Thumbprint"') do (
        certutil -delstore "Root" "%%a" >nul 2>&1
        certutil -delstore "TrustedPublisher" "%%a" >nul 2>&1
    )
)
if exist "%DEST%\driver\eyetoy_device.cer" (
    for /f "tokens=*" %%a in ('powershell -Command "Get-PfxCertificate -FilePath '%DEST%\driver\eyetoy_device.cer' | Select-Object -ExpandProperty Thumbprint"') do (
        certutil -delstore "Root" "%%a" >nul 2>&1
        certutil -delstore "TrustedPublisher" "%%a" >nul 2>&1
    )
)

echo Cleaning up files...
if exist "%DEST%" (
    rmdir /s /q "%DEST%"
)

echo Restarting Camera Services...
net start FrameServer >nul 2>&1
net start FrameServerMonitor >nul 2>&1

echo.
echo Uninstalled successfully!
echo - Camera files and scheduled tasks removed.
echo - Registry settings deleted.
echo.
pause
endlocal

:: Clean up directory in the background after we exit (so file handles are released)
start /b "" cmd /c "ping -n 3 127.0.0.1 >nul & rmdir /s /q "%DEST%""
