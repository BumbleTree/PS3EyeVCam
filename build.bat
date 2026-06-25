@echo off
rem Builds PSCam4Win.dll (MF virtual camera media source) and
rem PSCam4WinTray.exe (tray app: capture, sleep/wake, settings, autostart)
rem with VS2019 Build Tools. Static CRT (/MT) matches the prebuilt libusb.
setlocal

rem ---- Locate VS compiler using vswhere -------------------------------------
set "VS_PATH="
set "VCVARS_PATH="
if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" (
    "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath > "%temp%\vs_path.txt" 2>nul
    if exist "%temp%\vs_path.txt" (
        set /p VS_PATH=<"%temp%\vs_path.txt"
        del "%temp%\vs_path.txt"
    )
)

if defined VS_PATH (
    if exist "%VS_PATH%\VC\Auxiliary\Build\vcvars64.bat" (
        set "VCVARS_PATH=%VS_PATH%\VC\Auxiliary\Build\vcvars64.bat"
    )
)

rem Fallback to default hardcoded paths if vswhere couldn't find it
if not defined VCVARS_PATH (
    if exist "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat" (
        set "VCVARS_PATH=C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
    ) else if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" (
        set "VCVARS_PATH=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
    )
)

if not defined VCVARS_PATH (
    echo error: could not locate Visual Studio C++ build tools: vcvars64.bat
    echo Please install Visual Studio 2019 or 2022 with the "Desktop development with C++" workload.
    pause
    exit /b 1
)

call "%VCVARS_PATH%" >nul
if errorlevel 1 (
    echo error: could not initialize the Visual Studio x64 toolchain
    pause
    exit /b 1
)

set "ROOT=%~dp0"
rem libusb is vendored in-repo (third_party/libusb): prebuilt 1.0.27 static /MT x64,
rem header in include/. See third_party/libusb/README.md. No external repo needed.
set "LIBUSB_INC=%ROOT%third_party\libusb\include"
set "LIBUSB_LIB=%ROOT%third_party\libusb\lib\x64\libusb-1.0.lib"
rem libjpeg-turbo is vendored too (third_party/libjpeg-turbo): prebuilt 3.1.4
rem static /MT x64 (TurboJPEG API). Used only by the EyeToy JFIF->YUY2 decode.
rem Each path is quoted (the repo path contains spaces) so the two libs survive
rem as separate tokens on the link line.
set TJPEG_LIB="%ROOT%third_party\libjpeg-turbo\lib\x64\turbojpeg.lib" "%ROOT%third_party\libjpeg-turbo\lib\x64\jpeg.lib"
set "OUT=%ROOT%build"
if not exist "%OUT%" mkdir "%OUT%"

rem /utf-8 is required: sources are UTF-8 without BOM and contain non-ASCII
rem string literals (em-dashes in tray tooltips); without it MSVC decodes them
rem as Windows-1252 and the tooltips show mojibake.
set CFLAGS=/nologo /c /O2 /MT /EHsc /std:c++17 /utf-8 /W3 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS

echo === compiling vendored PS3EYEDriver ===
cl %CFLAGS% /I "%LIBUSB_INC%" "%ROOT%third_party\ps3eye\ps3eye.cpp" /Fo"%OUT%\ps3eye.obj"
if errorlevel 1 exit /b 1

echo === compiling PSCam4Win.dll ===
cl %CFLAGS% "%ROOT%source\VCamSource.cpp" /Fo"%OUT%\VCamSource.obj"
if errorlevel 1 exit /b 1
cl %CFLAGS% "%ROOT%source\dllmain.cpp" /Fo"%OUT%\dllmain.obj"
if errorlevel 1 exit /b 1

link /nologo /DLL /DEF:"%ROOT%source\PSCam4Win.def" /OUT:"%OUT%\PSCam4Win.dll" ^
    "%OUT%\VCamSource.obj" "%OUT%\dllmain.obj" ^
    mfplat.lib mfuuid.lib ole32.lib advapi32.lib
if errorlevel 1 exit /b 1

echo === compiling resources ===
rc /nologo /fo "%OUT%\app.res" "%ROOT%res\app.rc"
if errorlevel 1 exit /b 1

echo === compiling PSCam4WinTray.exe ===
set HOSTFLAGS=%CFLAGS% /I "%ROOT%third_party\ps3eye" /I "%LIBUSB_INC%"
cl %HOSTFLAGS% "%ROOT%host\Main.cpp"                /Fo"%OUT%\Main.obj"                || exit /b 1
cl %HOSTFLAGS% "%ROOT%host\CaptureController.cpp"   /Fo"%OUT%\CaptureController.obj"   || exit /b 1
cl %HOSTFLAGS% "%ROOT%host\TrayUI.cpp"              /Fo"%OUT%\TrayUI.obj"              || exit /b 1
cl %HOSTFLAGS% "%ROOT%host\SettingsDialog.cpp"      /Fo"%OUT%\SettingsDialog.obj"      || exit /b 1
cl %HOSTFLAGS% "%ROOT%host\CameraPreview.cpp"       /Fo"%OUT%\CameraPreview.obj"       || exit /b 1
cl %HOSTFLAGS% "%ROOT%host\FrameBusPreviewSource.cpp" /Fo"%OUT%\FrameBusPreviewSource.obj" || exit /b 1
cl %HOSTFLAGS% "%ROOT%host\Autostart.cpp"           /Fo"%OUT%\Autostart.obj"           || exit /b 1
cl %HOSTFLAGS% "%ROOT%common\Settings.cpp"          /Fo"%OUT%\Settings.obj"            || exit /b 1
cl %HOSTFLAGS% "%ROOT%host\DeviceProfiles.cpp"      /Fo"%OUT%\DeviceProfiles.obj"      || exit /b 1
cl %HOSTFLAGS% "%ROOT%host\DeviceRegistry.cpp"      /Fo"%OUT%\DeviceRegistry.obj"      || exit /b 1
cl %HOSTFLAGS% "%ROOT%transports\usb_bulk\Ps3EyeDevice.cpp" /Fo"%OUT%\Ps3EyeDevice.obj" || exit /b 1
cl %HOSTFLAGS% "%ROOT%transports\usb_iso\EyeToyUsb.cpp"    /Fo"%OUT%\EyeToyUsb.obj"    || exit /b 1
cl %HOSTFLAGS% "%ROOT%transports\usb_iso\EyeToyDevice.cpp" /Fo"%OUT%\EyeToyDevice.obj" || exit /b 1

rem /LTCG: the vendored libusb is built with whole-program optimization (/GL);
rem stating /LTCG here avoids the linker's automatic "restarting link" pass.
link /nologo /LTCG /SUBSYSTEM:WINDOWS /OUT:"%OUT%\PSCam4WinTray.exe" ^
    "%OUT%\Main.obj" "%OUT%\CaptureController.obj" "%OUT%\TrayUI.obj" ^
    "%OUT%\SettingsDialog.obj" "%OUT%\CameraPreview.obj" "%OUT%\FrameBusPreviewSource.obj" ^
    "%OUT%\Autostart.obj" "%OUT%\Settings.obj" ^
    "%OUT%\DeviceProfiles.obj" "%OUT%\DeviceRegistry.obj" "%OUT%\Ps3EyeDevice.obj" ^
    "%OUT%\EyeToyUsb.obj" "%OUT%\EyeToyDevice.obj" ^
    "%OUT%\ps3eye.obj" "%OUT%\app.res" ^
    "%LIBUSB_LIB%" %TJPEG_LIB% ^
    mfplat.lib mfuuid.lib ole32.lib oleaut32.lib advapi32.lib setupapi.lib ^
    user32.lib gdi32.lib shell32.lib comctl32.lib secur32.lib taskschd.lib uuid.lib ^
    /MANIFEST:EMBED /MANIFESTINPUT:"%ROOT%res\app.manifest" /MANIFESTUAC:NO
if errorlevel 1 exit /b 1

echo.
echo build OK:
echo   %OUT%\PSCam4Win.dll
echo   %OUT%\PSCam4WinTray.exe
echo next: run install.bat as Administrator
endlocal
