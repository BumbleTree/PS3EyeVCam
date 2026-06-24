# PSCam4Win — PlayStation cameras as Windows Virtual Cameras

PSCam4Win lets you use classic PlayStation cameras as standard web cameras (and audio inputs) on modern Windows 11 systems. It currently supports the **PlayStation 3 Eye** and the **PlayStation 2 EyeToy**, behind one common user-space pipeline.

Once installed, each plugged-in camera appears to apps like **Discord, Zoom, OBS, PCSX2 / RPCS3, and web browsers** as a regular built-in camera, named after the device that occupies the slot (e.g. **"PS3 Eye"** or **"PS2 EyeToy"**). The integrated microphones are also exposed as standard recording devices named **"USB Camera-B4.09.24.1"** (for the PS3 Eye's 4-channel array) and **"Logitech EyeToy USB Camera"** (for the PS2 EyeToy). Everything operates in user space (no risky kernel drivers — only Microsoft's inbox WinUSB for video) and is fully safe for Memory Integrity / Core Isolation settings.

> **Device support:** PS3 Eye streams raw Bayer that is debayered to YUY2 in-process; the PS2 EyeToy (OmniVision OV519/OV7648) streams JPEG over WinUSB isochronous and is decoded to YUY2 with libjpeg-turbo. Both ride the same FrameBus → virtual-camera path. PS4/PS5 cameras are planned.

```
                              ┌── Interface 0 (MI_00) ── WinUSB ── libusb ── PS3EYEDriver ──► PSCam4WinTray.exe
                              │                                                                 │  Bayer→YUY2
                              │                                                                 ▼
                              │                                        Global\PSCam4Win{N}.FrameBus + .Control (shared memory)
                              │                                                                 │
                              │                                                                 ▼
PS3 Eye (Composite Device) ───┤                                                            PSCam4Win.dll (in Camera Frame Server)
                              │                                                                 │  YUY2 native / NV12 (on-the-fly)
                              │                                                                 ▼
                              │                                                  "PS3 Eye" virtual camera in every app
                              │
                              └── Interface 1 (MI_01) ── usbaudio.sys ──► "USB Camera-B4.09.24.1" (Microphone) in every app

(The PS2 EyeToy follows the same right-hand path: its JPEG iso stream is reassembled and
 decoded to YUY2 in PSCam4WinTray.exe, then published to its own Global\PSCam4Win{N}.FrameBus.
 Its microphone is also automatically driver-mapped via usbaudio.sys to "Logitech EyeToy USB Camera".)
```

---

## What It Does For You

* **Connect Up to 8 Cameras:** Use up to 8 PlayStation cameras (any mix of PS3 Eye and PS2 EyeToy) at the same time. The virtual cameras appear in your apps automatically only when a physical camera is plugged in, named after whichever device occupies the slot, and disappear when unplugged so you never see clutter.
* **Smart Sleep & Wake:** The physical cameras automatically power down (turning off the red LED and using 0% CPU) when not in use by any app, and wake up instantly when needed.
* **Integrated Microphone Support:** Works seamlessly with the built-in microphones of both devices: the PS3 Eye's 4-channel microphone array (**"USB Camera-B4.09.24.1"**) and the PS2 EyeToy microphone (**"Logitech EyeToy USB Camera"**) are both automatically exposed as standard Windows recording devices.
* **Quick Control from System Tray:** Adjust camera settings easily via a menu right next to your Windows clock.
* **Auto-Saved Settings:** Your adjustments for mirroring, gain, exposure, and white balance are automatically remembered for each camera.
* **Silent Windows Startup:** Launches quietly when you log in without showing any annoying security prompts.
* **No Complicated Setup:** You don't need to manually configure drivers or download third-party tools like Zadig—the installer does everything for you.

---

## How to Install & Uninstall

### Installation
1. Make sure you have built the binaries (run `build.bat` first if you are compiling from source).
2. Right-click [install.bat] and choose **Run as administrator** (or double-click it and accept the prompt).
3. The installer will:
   * Upgrade any older **PS3EyeVCam** install in place (migrating your saved per-camera settings, then removing the old app, task, and registration).
   * Copy files to `C:\Program Files\PSCam4Win` (required for system camera integration).
   * Install the WinUSB video drivers for both the PS3 Eye and the PS2 EyeToy automatically.
   * Register the Virtual Camera DLL (handling all 8 camera CLSIDs).
   * Create an Add/Remove Programs entry in the Windows Registry.
   * Set up a silent logon task to launch the control tray app at Windows startup.
   * Launch the system tray controller.

### Uninstallation
If you want to cleanly remove it from your system:
1. Double-click [uninstall.bat] (or run the copy inside `C:\Program Files\PSCam4Win`).
2. It will stop the services, delete scheduled tasks, remove registry entries (including the Add/Remove Programs entry), remove both video drivers, unregister all CLSIDs, clean up the certificates, and delete all copied files.

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
5. For microphone support in RPCS3 or PCSX2, go to the emulator's audio settings, configure the microphone handler (e.g., **USBD** or standard Windows audio input), and select the appropriate recording device (**USB Camera-B4.09.24.1** for PS3 Eye, or **Logitech EyeToy USB Camera** for PS2 EyeToy).

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

* **Ultra-Low Latency:** Delivers video frames instantly to your apps with no noticeable delay or lag.
* **High Framerates:** Supports standard 60 FPS (up to 75 FPS) at 640×480, and high-speed modes up to 187 FPS at 320×240.
* **Minimal CPU Impact:** Highly optimized video processing uses negligible CPU, even in high-speed modes.
* **Zero Idle Resource Usage:** When no apps are using the camera, the background system uses 0% CPU and puts the camera into low-power mode.

---

## Architecture & Technical Notes (For Developers)

### Multi-Camera Routing
* The host daemon spawns an independent thread for each physical camera `i` (0 to 7) mapped dynamically. 
* Each thread registers a virtual camera using the static class ID `CLSID_PS3EyeVCams[i]` (GUIDs retained across the rebrand to avoid COM re-registration), and communicates over custom slot-specific IPC channels: `Global\PSCam4Win[i].FrameBus` and `Global\PSCam4Win[i].Control`. The friendly name shown to apps is chosen per slot from the occupying device's profile.
* The virtual camera registers (`IMFVirtualCamera`) dynamically *only* when a physical camera is connected to a slot, and unregisters it on removal. This ensures client applications see exactly the number of cameras physically plugged in. Camera arrival and removal are event-driven via WinUSB device interface notifications fanned out to per-slot controllers (with a 5-second polling fallback for empty slots).

### High-Performance IPC (FrameBus)
* **Sub-Millisecond Latency via Auto-Reset Events:** Video frames are passed from the capture thread to the virtual camera DLL via a lock-free shared-memory queue (`FrameBus`). The reader waits on a per-camera auto-reset event (`Global\PSCam4Win[N].FrameReady`) with restricted ACLs rather than polling or sleeping, avoiding the 15.6 ms system timer quantum. This allows high-speed modes (100–187 fps) to achieve their full target framerate with sub-millisecond latency.
* **Sleep/Wake Coordination:** Sleep/wake state is coordinated via a shared keepalive timestamp (`common\ControlBus.h`). If the virtual camera DLL stops requesting frames, the tray app puts the hardware to sleep after ~3 seconds.

### On-the-Fly Image Processing & Color Conversion
* **Fused Debayering Pipeline:** To minimize CPU and memory footprint, `ps3eye.cpp` uses a fused single-pass Bayer-to-YUY2 debayering pipeline that outputs directly to BT.601 YUY2 via a 1.9 KB cache-resident row scratch buffer, completely bypassing the need for a 1.2 MB BGRA intermediate buffer.
* **Native Original Format, On-Demand Conversion:** The shared-memory frame buffer carries `YUY2` (YUYV 4:2:2) — the PS3 Eye's original output format — preserving the sensor's full vertical chroma resolution end to end. A client requesting `YUY2` (such as the RPCS3 emulator) receives the camera frame byte-for-byte with no conversion at all; clients using the modern `NV12` format get a proper chroma-averaged 4:2:2 → 4:2:0 conversion performed on the fly at delivery time.
* **On-Demand Scaling:** When the camera preset and the application's requested resolution mismatch (e.g., camera is capturing at 320x240 but Discord requests 640x480), the DLL scales 2x on the fly: bilinear interpolation when upscaling and a 2x2 box filter when downscaling, both in integer math in well under a millisecond (negligible CPU usage even at 187 FPS). No scaling is performed when the formats match.
* **Optimized Bandwidth:** In high-speed 320x240 modes, raw data is sent across the memory bus at just 29 MB/s. Scaling occurs within the client process on-demand, saving system memory bandwidth.

### USB Bandwidth & Device Configuration
* **USB Bandwidth Bottlenecks:** Each PS3 Eye camera requires ~185 Mbps of USB bandwidth at 640x480 @ 60 FPS. Although modern PCs use USB 3.x (xHCI) ports, the PS3 Eye is a USB 2.0 High-Speed device and is restricted to the USB 2.0 protocol layer, which shares a 480 Mbps bandwidth pool on the controller's High-Speed bus. A single physical controller can therefore support at most 2 cameras before saturating the bus. To run 3 to 8 cameras concurrently, you must distribute the cameras across separate USB controllers (e.g., separating them between rear motherboard ports, front panel ports, or dedicated PCIe USB expansion cards).
* **Composite Device Mapping:** Both the PS3 Eye and the PS2 EyeToy are composite USB devices. For both cameras, Interface 0 (`MI_00`) handles video streaming (via WinUSB and the tray app), while Interface 1 (`MI_01`) is automatically handled by the standard Windows USB Audio driver (`usbaudio.sys`) to expose their respective microphones.

### System Integration & OS Details
* **Security & Permissions:** The virtual camera media source DLL (`PSCam4Win.dll`) runs inside the **Camera Frame Server service** (`LOCAL SERVICE`). The tray app requires admin rights to create IPC objects in the `Global\` kernel namespace.
* **Startup Task:** The startup task uses the `ITaskService` COM API instead of `schtasks.exe` to bypass battery limits and execution duration limits.

---

## License

This project is licensed under the **GNU General Public License v2.0** (GPLv2) - see the [LICENSE](LICENSE) file for details.

### Third-Party Components & Licenses:
* **PS3EYEDriver wrapper** (`third_party/ps3eye/`): Ported from the [inspirit/PS3EYEDriver](https://github.com/inspirit/PS3EYEDriver) library, which is derived from the Linux Kernel `gspca_ov534` driver and licensed under the **GNU General Public License v2.0**.
* **libusb**: Used for low-level USB communications, licensed under the **GNU Lesser General Public License v2.1** (LGPLv2.1) or later.
