# PlayStation 3 Eye → Windows Virtual Camera

This project allows you to use your classic PlayStation 3 Eye camera and its built-in microphone array as a standard web camera and audio input device on modern Windows 11 systems. 

Once installed, the PS3 Eye appears to apps like **Discord, Zoom, OBS, RPCS3 (PS3 Emulator), and web browsers** as a regular built-in camera called **"PS3 Eye (Windows Virtual Camera)"**, and its integrated 4-channel microphone array is exposed as a standard recording device named **"USB Camera-B4.09.24.1"**. It operates entirely in user space (no risky kernel drivers) and is fully safe for Memory Integrity / Core Isolation settings.

```
                              ┌── Interface 0 (MI_00) ── WinUSB ── libusb ── PS3EYEDriver ──► PS3EyeVCamTray.exe
                              │                                                                 │  Bayer→YUY2
                              │                                                                 ▼
                              │                                         Global\PS3EyeVCam.FrameBus + .Control (shared memory)
                              │                                                                 │
                              │                                                                 ▼
PS3 Eye (Composite Device) ───┤                                                           PS3EyeVCam.dll (in Camera Frame Server)
                              │                                                                 │  YUY2 native / NV12 (on-the-fly)
                              │                                                                 ▼
                              │                                             "PS3 Eye (Windows Virtual Camera)" in every app
                              │
                              └── Interface 1 (MI_01) ── usbaudio.sys ──► "USB Camera-B4.09.24.1" (Microphone) in every app
```

---

## What It Does For You

* **Dynamic Multi-Camera Routing:** Supports up to 8 simultaneous physical PS3 Eye cameras. The system tray controller registers the virtual camera (`IMFVirtualCamera`) dynamically *only* when a physical camera is connected to a slot, and unregisters it on removal. This ensures client applications see exactly the number of cameras physically plugged in. Camera arrival and removal are event-driven via WinUSB device interface notifications fanned out to per-slot controllers (with a 5-second polling fallback for empty slots).
* **Automatic Sleep & Wake:** The cameras only turn on when a program is actually using them. A physical camera is powered down (0% CPU, LED off) when not in use. It wakes up in under a second when needed.
* **Built-in Microphone Array Support:** Exposes the PS3 Eye's high-quality 4-channel microphone array as a standard Windows audio input device (**"USB Camera-B4.09.24.1"**) using Windows' native USB Audio Class driver, working seamlessly out-of-the-box.
* **Easy Access Settings:** Control camera settings directly from a system tray icon.
* **Settings Persistence:** Mirroring, gain, exposure, and white balance settings are saved and remembered automatically.
* **Automatic Silent Start:** Starts automatically when you sign into Windows without triggering any popups (UAC prompt-free).
* **Zero Driver Hassles:** No need to download Zadig or other custom installer programs. The installation script handles the entire video driver setup automatically.

---

## How to Install & Uninstall

### Installation
1. Make sure you have built the binaries (run `build.bat` first if you are compiling from source).
2. Right-click [install.bat] and choose **Run as administrator** (or double-click it and accept the prompt).
3. The installer will:
   * Copy files to `C:\Program Files\PS3EyeVCam` (required for system camera integration).
   * Install the necessary WinUSB video driver automatically.
   * Register the Virtual Camera DLL (handling all 8 camera CLSIDs).
   * Create an Add/Remove Programs entry in the Windows Registry.
   * Set up a silent logon task to launch the control tray app at Windows startup.
   * Launch the system tray controller.

### Uninstallation
If you want to cleanly remove it from your system:
1. Double-click [uninstall.bat] (or run the copy inside `C:\Program Files\PS3EyeVCam`).
2. It will stop the services, delete scheduled tasks, remove registry entries (including the Add/Remove Programs entry), remove the video driver, unregister all CLSIDs, clean up the certificate, and delete all copied files.

---

## Using the Camera and Settings

### Accessing Settings
Once installed, look for the **PS3 Eye camera icon** in your Windows System Tray (near the clock).
<img width="401" height="174" alt="SystemTray" src="https://github.com/user-attachments/assets/694ab46e-8f88-4642-b00e-b44de9beb11a" />

* **Right-click the tray icon** to:
  * Open **Settings** (or double-click the icon).
  * Toggle **Start with Windows** on/off.
  * Exit the tray application.
* **Settings Dialog**:
  * Choose which camera to configure using the **Camera dropdown selector** (independent settings are loaded and persisted per camera).
  * Adjust **Gain**, **Exposure**, **Brightness**, **Contrast**, **Sharpness**, **Hue**, **Red/Blue/Green Balance** manually via sliders in real time.
  * Toggle **Auto Gain & Exposure** and **Auto White Balance**.
  * Enable **Test Pattern** output for debugging or virtual camera validation.
  * Set **Video Presets** (e.g., standard 640×480 @ 60fps up to 75fps, or high-speed 320×240 up to 187fps).
  * Enable **Mirroring** (horizontal or vertical flip).
<img width="376" height="420" alt="camera settings (1)" src="https://github.com/user-attachments/assets/71843b34-0c2c-494c-b3e3-68eac5a2a482" />


*Note: If a program is currently streaming video from the selected camera, mode/preset changes for that camera are queued and will automatically apply as soon as you close/restart the stream.*

### Setting up with Emulators (e.g., RPCS3)
1. Ensure the tray app is running (check your system tray).
2. Open **RPCS3** and go to **Settings** > **I/O**.
3. Set **Camera Handler** to **Qt** and select **PS3 Eye (Windows Virtual Camera)** as the device.
4. Ensure camera access is allowed under Windows Settings (*Privacy & security* > *Camera* > *Allow desktop apps to access your camera*).
5. For microphone support in RPCS3, go to **Settings** > **Audio**, configure your microphone handler (e.g., **USBD** or standard Windows audio input), and select the **USB Camera-B4.09.24.1** as the recording device.

---

## Troubleshooting

| Symptom | Fix |
|---|---|
| Camera missing in apps | Check if the tray icon is present and has a green status. |
| Tooltip says "camera not detected" | Try unplugging and replugging the USB cable. The app will automatically find the camera once connected. |
| Apps show black screen | The camera takes ~1 second to wake up from sleep mode. Wait a moment; if it stays black, ensure the tray app is running. |
| Settings status shows registration fail | Go to Windows Settings > Privacy & security > Camera and ensure "Allow desktop apps to access your camera" is enabled. |

---

## Performance, Latency & Resources

* **Sub-Millisecond Latency via Auto-Reset Events:** Video frames are passed from the capture thread to the virtual camera DLL via a lock-free shared-memory queue (`FrameBus`). The reader waits on a per-camera auto-reset event (`Global\PS3EyeVCam[N].FrameReady`) with restricted ACLs rather than polling or sleeping, avoiding the 15.6 ms system timer quantum. This allows high-speed modes (100–187 fps) to achieve their full target framerate with sub-millisecond latency.
* **On-Demand Scaling:** When the camera preset and the application's requested resolution mismatch (e.g., camera is capturing at 320x240 but Discord requests 640x480), the DLL scales 2x on the fly: bilinear interpolation when upscaling and a 2x2 box filter when downscaling, both in integer math in well under a millisecond (`< 2%` of a single CPU core even at 187 FPS). No scaling is performed when the formats match.
* **Native Original Format, On-Demand Conversion:** The shared-memory frame buffer carries `YUY2` (YUYV 4:2:2) — the PS3 Eye's original output format — preserving the sensor's full vertical chroma resolution end to end. A client requesting `YUY2` (such as the RPCS3 emulator) receives the camera frame byte-for-byte with no conversion at all; clients using the modern `NV12` format get a proper chroma-averaged 4:2:2 → 4:2:0 conversion performed on the fly at delivery time.
* **Zero-Resource Idle State:** If no client applications are capturing from the virtual camera, the background tray daemon automatically powers down the physical camera (red LED turns off) after 3 seconds. While idle, the app consumes **0% CPU** and puts the USB controller in a low-power state.
* **Optimized Bandwidth:** In high-speed 320x240 modes, raw data is sent across the memory bus at just 29 MB/s. Scaling occurs within the client process on-demand, saving system memory bandwidth.

---

## Architecture & Technical Notes (For Developers)

* **Multi-Camera Routing:** The host daemon spawns an independent thread for each physical camera `i` (0 to 7) mapped dynamically. Each thread registers a virtual camera using the static class ID `CLSID_PS3EyeVCams[i]`, and communicates over custom slot-specific channels `Global\PS3EyeVCam[i].FrameBus` and `Global\PS3EyeVCam[i].Control`.
* **USB Bandwidth Bottlenecks:** Each PS3 Eye camera requires ~185 Mbps of USB bandwidth at 640x480 @ 60 FPS. Although modern PCs use USB 3.x (xHCI) ports, the PS3 Eye is a USB 2.0 High-Speed device and is restricted to the USB 2.0 protocol layer, which shares a 480 Mbps bandwidth pool on the controller's High-Speed bus. A single physical controller can therefore support at most 2 cameras before saturating the bus. To run 3 to 8 cameras concurrently, you must distribute the cameras across separate USB controllers (e.g., separating them between rear motherboard ports, front panel ports, or dedicated PCIe USB expansion cards).
* The PS3 Eye is a composite USB device. Interface 0 (`MI_00`) handles video streaming via WinUSB and the tray app, while Interface 1 (`MI_01`) is automatically mapped to standard Windows USB Audio (`usbaudio.sys`) to expose the 4-channel microphone array.
* The virtual camera media source DLL (`PS3EyeVCam.dll`) runs inside the **Camera Frame Server service** (`LOCAL SERVICE`).
* Video frames are written by the tray app (as `YUY2`, the camera's native 4:2:2 format) and read by the DLL via a high-performance lock-free shared memory queue (`Global\PS3EyeVCam[0-7].FrameBus`). To minimize CPU and memory footprint, `ps3eye.cpp` uses a fused single-pass Bayer-to-YUY2 debayering pipeline that outputs directly to BT.601 YUY2 via a 1.9 KB cache-resident row scratch buffer, completely bypassing the need for a 1.2 MB BGRA intermediate buffer.
* Sleep/wake state is coordinated via a shared keepalive timestamp (`common\ControlBus.h`). If the virtual camera DLL stops requesting frames, the tray app puts the hardware to sleep after ~3 seconds.
* The startup task uses the `ITaskService` COM API instead of `schtasks.exe` to bypass battery limits and execution duration limits.
* The tray app requires admin rights to create objects in the `Global\` kernel namespace.

---

## License

This project is licensed under the **GNU General Public License v2.0** (GPLv2) - see the [LICENSE](LICENSE) file for details.

### Third-Party Components & Licenses:
* **PS3EYEDriver wrapper** (`third_party/ps3eye/`): Ported from the [inspirit/PS3EYEDriver](https://github.com/inspirit/PS3EYEDriver) library, which is derived from the Linux Kernel `gspca_ov534` driver and licensed under the **GNU General Public License v2.0**.
* **libusb**: Used for low-level USB communications, licensed under the **GNU Lesser General Public License v2.1** (LGPLv2.1) or later.
