@echo off
rem Installs the PS3 Eye virtual camera. Must run as Administrator.
rem - self-elevates if run as standard user
rem - binaries to Program Files (Frame Server can't read user folders)
rem - registers the media source DLL (HKLM)
rem - installs the WinUSB driver + signing certificate (verified, loud failures)
rem - seeds default settings (640x480@60, autogain)
rem - enables silent elevated start-at-logon (in-app toggle can disable it)
rem - adds an Add/Remove Programs entry
rem - starts the tray app
setlocal

net session >nul 2>&1
if %errorlevel% neq 0 (
    echo Requesting administrative privileges...
    powershell -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)

cd /d "%~dp0"

set "SRC=%~dp0build"
set "DEST=%ProgramFiles%\PS3EyeVCam"

if not exist "%SRC%\PS3EyeVCam.dll" (
    echo error: build output not found -- run build.bat first
    pause
    exit /b 1
)
if not exist "%SRC%\PS3EyeVCamTray.exe" (
    echo error: build output not found -- run build.bat first
    pause
    exit /b 1
)

echo Stopping Camera Services...
net stop FrameServerMonitor /y >nul 2>&1
net stop FrameServer /y >nul 2>&1

echo Stopping existing host app instance...
taskkill /im PS3EyeVCamTray.exe >nul 2>&1
timeout /t 2 /nobreak >nul
taskkill /f /im PS3EyeVCamTray.exe >nul 2>&1

echo Creating destination directories...
if not exist "%DEST%" mkdir "%DEST%"
if not exist "%DEST%\driver" mkdir "%DEST%\driver"
if not exist "%DEST%\driver\amd64" mkdir "%DEST%\driver\amd64"

echo Copying files...
call :copyFile "%SRC%\PS3EyeVCam.dll"                        "%DEST%\"              || exit /b 1
call :copyFile "%SRC%\PS3EyeVCamTray.exe"                    "%DEST%\"              || exit /b 1
call :copyFile "%~dp0uninstall.bat"                          "%DEST%\"              || exit /b 1
call :copyFile "%~dp0driver\usb_device.inf"                  "%DEST%\driver\"       || exit /b 1
call :copyFile "%~dp0driver\usb_device.cat"                  "%DEST%\driver\"       || exit /b 1
call :copyFile "%~dp0driver\usb_device.cer"                  "%DEST%\driver\"       || exit /b 1
call :copyFile "%~dp0driver\amd64\WdfCoInstaller01011.dll"   "%DEST%\driver\amd64\" || exit /b 1
call :copyFile "%~dp0driver\amd64\winusbcoinstaller2.dll"    "%DEST%\driver\amd64\" || exit /b 1

echo Registering Virtual Camera DLL...
regsvr32 /s "%DEST%\PS3EyeVCam.dll"
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

echo Installing WinUSB Video Driver...
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

echo Writing default registry settings (Highest Resolution: 640x480@60)...
"%DEST%\PS3EyeVCamTray.exe" --seed-defaults

echo Registering Startup Task in Task Scheduler...
"%DEST%\PS3EyeVCamTray.exe" --enable-autostart
if errorlevel 1 (
    echo warning: could not create the logon task - toggle it in the app
)

echo Registering with Add/Remove Programs...
set "ARP=HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\PS3EyeVCam"
reg add "%ARP%" /v DisplayName     /t REG_SZ    /d "PS3 Eye Virtual Camera"        /f >nul
reg add "%ARP%" /v DisplayVersion  /t REG_SZ    /d "1.0.0"                          /f >nul
reg add "%ARP%" /v Publisher       /t REG_SZ    /d "PS3EyeVCam"                     /f >nul
reg add "%ARP%" /v InstallLocation /t REG_SZ    /d "%DEST%"                         /f >nul
reg add "%ARP%" /v DisplayIcon     /t REG_SZ    /d "%DEST%\PS3EyeVCamTray.exe"      /f >nul
reg add "%ARP%" /v UninstallString /t REG_SZ    /d "\"%DEST%\uninstall.bat\""       /f >nul
reg add "%ARP%" /v NoModify        /t REG_DWORD /d 1                                /f >nul
reg add "%ARP%" /v NoRepair        /t REG_DWORD /d 1                                /f >nul

echo Restarting Camera Services...
net start FrameServer >nul 2>&1
net start FrameServerMonitor >nul 2>&1

echo Starting camera host app...
start "" "%DEST%\PS3EyeVCamTray.exe"

echo.
echo Installed successfully!
echo - Camera files copied to: %DEST%
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
