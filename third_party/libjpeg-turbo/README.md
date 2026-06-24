# Vendored libjpeg-turbo (prebuilt)

This directory vendors a prebuilt **libjpeg-turbo 3.1.4** so the project stays self-contained: a
fresh `git clone` of PS3EyeVCam builds with `build.bat` without any external dependency, exactly like
the vendored `third_party/libusb`.

It is used by **one consumer only**: the PS2 EyeToy transport
(`transports/usb_iso/EyeToyDevice.cpp`), which decodes the camera's JFIF frames to YUY2 via the
TurboJPEG API (direct JPEG→planar-YUV, then a lossless 4:2:2 repack to YUY2 — no RGB detour). The
PS3 Eye path does **not** use this: it streams raw Bayer and debayers in the vendored driver, so
there is no JPEG anywhere in the PS3 path.

## Contents

| Path | What |
|---|---|
| `include/turbojpeg.h` | TurboJPEG public API header (self-contained; the only header consumers need) |
| `lib/x64/turbojpeg.lib` | TurboJPEG API static library, **x64, `/MT`** |
| `lib/x64/jpeg.lib` | libjpeg-turbo core static library (TurboJPEG links against it) — **x64, `/MT`** |
| `LICENSE` | libjpeg-turbo licenses (IJG + modified-BSD + zlib) |

Both `.lib` files are linked: `turbojpeg.lib` is the API wrapper and references the core symbols in
`jpeg.lib`. The static `/MT` CRT matches the app (and the vendored libusb).

## Version

- **libjpeg-turbo 3.1.4** (via vcpkg triplet `x64-windows-static`, which defaults to the static `/MT`
  CRT).

## How it was built (to reproduce / upgrade)

```powershell
git clone --depth 1 https://github.com/microsoft/vcpkg.git
.\vcpkg\bootstrap-vcpkg.bat -disableMetrics
.\vcpkg\vcpkg.exe install libjpeg-turbo:x64-windows-static   # self-acquires NASM for the build
# from vcpkg\installed\x64-windows-static\ :
#   include\turbojpeg.h        -> include\turbojpeg.h
#   lib\turbojpeg.lib          -> lib\x64\turbojpeg.lib
#   lib\jpeg.lib               -> lib\x64\jpeg.lib
#   share\libjpeg-turbo\copyright -> LICENSE
```

To upgrade: rebuild via vcpkg (it tracks the current port version) and replace the header + the two
`.lib` files here.
