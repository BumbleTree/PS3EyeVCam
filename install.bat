@echo off
rem Installs PSCam4Win (the multi-device virtual camera: PS3 Eye + PS2 EyeToy).
rem Must run as Administrator.
rem - self-elevates if run as standard user
rem - UPGRADES a legacy "PS3EyeVCam" install in place (migrates saved settings,
rem   removes the old app/task/registration), then installs PSCam4Win
rem - binaries to Program Files (Frame Server can't read user folders)
rem - registers the media source DLL (HKLM)
rem - installs the WinUSB drivers + signing certificates (both cameras)
rem - seeds default settings, enables silent elevated start-at-logon
rem - adds an Add/Remove Programs entry, starts the tray app
setlocal

net session >nul 2>&1
if %errorlevel% neq 0 (
    echo Requesting administrative privileges...
    powershell -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)

cd /d "%~dp0"

set "SRC=%~dp0build"
set "DEST=%ProgramFiles%\PSCam4Win"
set "OLDDEST=%ProgramFiles%\PS3EyeVCam"

if not exist "%SRC%\PSCam4Win.dll" (
    echo error: build output not found -- run build.bat first
    pause
    exit /b 1
)
if not exist "%SRC%\PSCam4WinTray.exe" (
    echo error: build output not found -- run build.bat first
    pause
    exit /b 1
)

echo Stopping Camera Services...
net stop FrameServerMonitor /y >nul 2>&1
net stop FrameServer /y >nul 2>&1

echo Stopping any running host app instance...
taskkill /im PSCam4WinTray.exe >nul 2>&1
taskkill /im PS3EyeVCamTray.exe >nul 2>&1
timeout /t 2 /nobreak >nul
taskkill /f /im PSCam4WinTray.exe >nul 2>&1
taskkill /f /im PS3EyeVCamTray.exe >nul 2>&1

rem ---- upgrade a legacy PS3EyeVCam install in place ------------------------
rem The PS3 Eye WinUSB driver + its certificate are shared (same usb_device.*),
rem so they are left alone; the new install re-adds them idempotently.
echo Checking for a previous "PS3 Eye Virtual Camera" install...
schtasks /query /tn "PS3EyeVCam" >nul 2>&1
if %errorlevel%==0 (
    echo - removing the old logon task
    schtasks /delete /tn "PS3EyeVCam" /f >nul 2>&1
)
if exist "%OLDDEST%\PS3EyeVCam.dll" (
    echo - unregistering the old media source DLL
    regsvr32 /u /s "%OLDDEST%\PS3EyeVCam.dll"
)
reg query "HKLM\SOFTWARE\PS3EyeVCam" >nul 2>&1
if %errorlevel%==0 (
    echo - migrating saved camera settings to PSCam4Win
    reg copy "HKLM\SOFTWARE\PS3EyeVCam" "HKLM\SOFTWARE\PSCam4Win" /s /f >nul 2>&1
    reg delete "HKLM\SOFTWARE\PS3EyeVCam" /f >nul 2>&1
)
reg delete "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\PS3EyeVCam" /f >nul 2>&1
if exist "%OLDDEST%" (
    echo - removing the old install folder
    rmdir /s /q "%OLDDEST%" >nul 2>&1
)

echo Creating destination directories...
if not exist "%DEST%" mkdir "%DEST%"
if not exist "%DEST%\driver" mkdir "%DEST%\driver"
if not exist "%DEST%\driver\amd64" mkdir "%DEST%\driver\amd64"

echo Copying files...
call :copyFile "%SRC%\PSCam4Win.dll"                         "%DEST%\"              || exit /b 1
call :copyFile "%SRC%\PSCam4WinTray.exe"                     "%DEST%\"              || exit /b 1
call :copyFile "%~dp0uninstall.bat"                          "%DEST%\"              || exit /b 1
call :copyFile "%~dp0driver\usb_device.inf"                  "%DEST%\driver\"       || exit /b 1
call :copyFile "%~dp0driver\usb_device.cat"                  "%DEST%\driver\"       || exit /b 1
call :copyFile "%~dp0driver\usb_device.cer"                  "%DEST%\driver\"       || exit /b 1
call :copyFile "%~dp0driver\eyetoy_device.inf"               "%DEST%\driver\"       || exit /b 1
call :copyFile "%~dp0driver\eyetoy_device.cat"               "%DEST%\driver\"       || exit /b 1
call :copyFile "%~dp0driver\eyetoy_device.cer"               "%DEST%\driver\"       || exit /b 1
call :copyFile "%~dp0driver\amd64\WdfCoInstaller01011.dll"   "%DEST%\driver\amd64\" || exit /b 1
call :copyFile "%~dp0driver\amd64\winusbcoinstaller2.dll"    "%DEST%\driver\amd64\" || exit /b 1

echo Registering Virtual Camera DLL...
regsvr32 /s "%DEST%\PSCam4Win.dll"
if errorlevel 1 (
    echo error: regsvr32 failed
    pause
    exit /b 1
)

echo Registering WinUSB Driver Certificate...
certutil -addstore "Root" "%DEST%\driver\usb_device.cer" >nul
if errorlevel 1 (
    echo error: could not add the driver certificate to the Root store.
    echo        Without it the driver catalog will not validate and the
    echo        WinUSB driver cannot install.
    pause
    exit /b 1
)
certutil -addstore "TrustedPublisher" "%DEST%\driver\usb_device.cer" >nul
if errorlevel 1 (
    echo error: could not add the driver certificate to the TrustedPublisher store.
    pause
    exit /b 1
)
certutil -addstore "Root" "%DEST%\driver\eyetoy_device.cer" >nul
if errorlevel 1 (
    echo error: could not add the EyeToy driver certificate to the Root store.
    pause
    exit /b 1
)
certutil -addstore "TrustedPublisher" "%DEST%\driver\eyetoy_device.cer" >nul
if errorlevel 1 (
    echo error: could not add the EyeToy driver certificate to the TrustedPublisher store.
    pause
    exit /b 1
)

echo Installing WinUSB Video Driver (PS3 Eye)...
pnputil /add-driver "%DEST%\driver\usb_device.inf" /install >nul
set "PNP_RESULT=%errorlevel%"
if "%PNP_RESULT%"=="0" goto driverOk
if "%PNP_RESULT%"=="259" (
    rem ERROR_NO_MORE_ITEMS: staged, but no matching device was connected.
    echo note: driver staged. It will bind automatically when a PS3 Eye is plugged in.
    goto driverOk
)
if "%PNP_RESULT%"=="3010" (
    echo note: driver installed -- a reboot is required before the camera works.
    goto driverOk
)
echo error: driver installation failed ^(pnputil exit code %PNP_RESULT%^).
echo        Run "pnputil /add-driver "%DEST%\driver\usb_device.inf" /install"
echo        manually to see details, and check that Secure Boot policy allows
echo        the self-signed driver certificate.
pause
exit /b 1
:driverOk

echo Installing WinUSB Video Driver (PS2 EyeToy)...
pnputil /add-driver "%DEST%\driver\eyetoy_device.inf" /install >nul
set "PNP_RESULT=%errorlevel%"
if "%PNP_RESULT%"=="0" goto eyetoyOk
if "%PNP_RESULT%"=="259" (
    echo note: EyeToy driver staged. It will bind automatically when an EyeToy is plugged in.
    goto eyetoyOk
)
if "%PNP_RESULT%"=="3010" (
    echo note: EyeToy driver installed -- a reboot is required before the camera works.
    goto eyetoyOk
)
echo error: EyeToy driver installation failed ^(pnputil exit code %PNP_RESULT%^).
echo        Run "pnputil /add-driver "%DEST%\driver\eyetoy_device.inf" /install"
echo        manually to see details, and check that Secure Boot policy allows
echo        the self-signed driver certificate.
pause
exit /b 1
:eyetoyOk

echo Writing default registry settings (missing values only)...
"%DEST%\PSCam4WinTray.exe" --seed-defaults

echo Registering Startup Task in Task Scheduler...
"%DEST%\PSCam4WinTray.exe" --enable-autostart
if errorlevel 1 (
    echo warning: could not create the logon task - toggle it in the app
)

echo Registering with Add/Remove Programs...
set "ARP=HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\PSCam4Win"
reg add "%ARP%" /v DisplayName     /t REG_SZ    /d "PSCam4Win Virtual Camera"      /f >nul
reg add "%ARP%" /v DisplayVersion  /t REG_SZ    /d "3.0.0"                          /f >nul
reg add "%ARP%" /v Publisher       /t REG_SZ    /d "PSCam4Win"                      /f >nul
reg add "%ARP%" /v InstallLocation /t REG_SZ    /d "%DEST%"                         /f >nul
reg add "%ARP%" /v DisplayIcon     /t REG_SZ    /d "%DEST%\PSCam4WinTray.exe"       /f >nul
reg add "%ARP%" /v UninstallString /t REG_SZ    /d "\"%DEST%\uninstall.bat\""       /f >nul
reg add "%ARP%" /v NoModify        /t REG_DWORD /d 1                                /f >nul
reg add "%ARP%" /v NoRepair        /t REG_DWORD /d 1                                /f >nul

echo Restarting Camera Services...
net start FrameServer >nul 2>&1
net start FrameServerMonitor >nul 2>&1

echo Starting camera host app...
start "" "%DEST%\PSCam4WinTray.exe"

echo.
echo Installed successfully!
echo - Camera files copied to: %DEST%
echo - Supports the PS3 Eye and the PS2 EyeToy (each plugged-in camera appears
echo   as its own virtual camera, configurable from the tray Settings dialog).
echo - System tray app registered to start elevated on Windows logon.
echo - Camera host started. Look for the camera icon in your System Tray.
echo.
pause
endlocal
exit /b 0

rem ---------------------------------------------------------------------------
rem copyFile <source> <destination-dir>  -- copy with a loud failure
:copyFile
copy /y %1 %2 >nul
if errorlevel 1 (
    echo error: could not copy %~nx1 - file in use?
    pause
    exit /b 1
)
exit /b 0
