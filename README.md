# PlayStation 3 Eye → Windows Virtual Camera

This project allows you to use your classic PlayStation 3 Eye camera and its built-in microphone array as a standard web camera and audio input device on modern Windows 11 systems. 

Once installed, the PS3 Eye appears to apps like **Discord, Zoom, OBS, RPCS3 (PS3 Emulator), and web browsers** as a regular built-in camera called **"PS3 Eye (Windows Virtual Camera)"**, and its integrated 4-channel microphone array is exposed as a standard recording device named **"PS3 Eye Camera"**. It operates entirely in user space (no risky kernel drivers) and is fully safe for Memory Integrity / Core Isolation settings.

```
                              ┌── Interface 0 (MI_00) ── WinUSB ── libusb ── PS3EYEDriver ──► PS3EyeVCamTray.exe
                              │                                                                 │  BGRA→NV12
                              │                                                                 ▼
                              │                                         Global\PS3EyeVCam.FrameBus + .Control (shared memory)
                              │                                                                 │
                              │                                                                 ▼
PS3 Eye (Composite Device) ───┤                                                           PS3EyeVCam.dll (in Camera Frame Server)
                              │                                                                 │
                              │                                                                 ▼
                              │                                             "PS3 Eye (Windows Virtual Camera)" in every app
                              │
                              └── Interface 1 (MI_01) ── usbaudio.sys ──► "PS3 Eye Camera" (Microphone) in every app
```

---

## What It Does For You

* **Automatic Sleep & Wake:** The camera only turns on when a program is actually using it. The physical camera is powered down (0% CPU, LED off) when not in use. It wakes up in under a second when needed.
* **Built-in Microphone Array Support:** Automatically installs a custom audio driver configuration that exposes the PS3 Eye's high-quality microphone array as a standard Windows audio input device (**"PS3 Eye Camera"**), working seamlessly out-of-the-box.
* **Easy Access Settings:** Control camera settings directly from a system tray icon.
* **Settings Persistence:** Mirroring, gain, exposure, and white balance settings are saved and remembered automatically.
* **Automatic Silent Start:** Starts automatically when you sign into Windows without triggering any popups (UAC prompt-free).
* **Zero Driver Hassles:** No need to download Zadig or other custom installer programs. The installation script handles the entire driver setup automatically for both video and audio.

---

## How to Install & Uninstall

### Installation
1. Make sure you have built the binaries (run `build.bat` first if you are compiling from source).
2. Right-click [install.bat] and choose **Run as administrator** (or double-click it and accept the prompt).
3. The installer will:
   * Copy files to `C:\Program Files\PS3EyeVCam` (required for system camera integration).
   * Install the necessary WinUSB video and USB audio drivers automatically.
   * Register the Virtual Camera DLL.
   * Set up a silent logon task to launch the control tray app at Windows startup.
   * Launch the system tray controller.

### Uninstallation
If you want to cleanly remove it from your system:
1. Double-click [uninstall.bat] (or run the copy inside `C:\Program Files\PS3EyeVCam`).
2. It will stop the services, delete scheduled tasks, remove registry entries, remove the video and audio drivers, and delete all copied files.

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
  * Adjust **Gain** and **Exposure** manually via sliders in real time, or toggle Auto-Gain/Auto-Exposure.
  * Adjust **White Balance** settings.
  * Set **Video Presets** (e.g., standard 640×480 @ 60fps up to 75fps, or high-speed 320×240 up to 187fps).
  * Enable **Mirroring** (horizontal or vertical flip).
<img width="376" height="420" alt="camera settings (1)" src="https://github.com/user-attachments/assets/71843b34-0c2c-494c-b3e3-68eac5a2a482" />


*Note: If a program is currently streaming video, mode/preset changes are queued and will automatically apply as soon as you close/restart the stream.*

### Setting up with Emulators (e.g., RPCS3)
1. Ensure the tray app is running (check your system tray).
2. Open **RPCS3** and go to **Settings** > **I/O**.
3. Set **Camera Handler** to **Qt** and select **PS3 Eye (Windows Virtual Camera)** as the device.
4. Ensure camera access is allowed under Windows Settings (*Privacy & security* > *Camera* > *Allow desktop apps to access your camera*).
5. For microphone support in RPCS3, go to **Settings** > **Audio**, configure your microphone handler (e.g., **USBD** or standard Windows audio input), and select the **PS3 Eye Camera** as the recording device.

---

## Troubleshooting

| Symptom | Fix |
|---|---|
| Camera missing in apps | Check if the tray icon is present and has a green status. |
| Tooltip says "camera not detected" | Try unplugging and replugging the USB cable. The app will automatically find the camera once connected. |
| Apps show black screen | The camera takes ~1 second to wake up from sleep mode. Wait a moment; if it stays black, ensure the tray app is running. |
| Settings status shows registration fail | Go to Windows Settings > Privacy & security > Camera and ensure "Allow desktop apps to access your camera" is enabled. |

---

## Architecture & Technical Notes (For Developers)

* The PS3 Eye is a composite USB device. Interface 0 (`MI_00`) handles video streaming via WinUSB and the tray app, while Interface 1 (`MI_01`) is mapped to the standard Windows USB Audio Class driver (`usbaudio.sys`) via a custom INF to expose the 4-channel microphone array.
* The virtual camera media source DLL (`PS3EyeVCam.dll`) runs inside the **Camera Frame Server service** (`LOCAL SERVICE`).
* Video frames are written by the tray app and read by the DLL via a high-performance lock-free shared memory queue (`Global\PS3EyeVCam.FrameBus`).
* Sleep/wake state is coordinated via a shared keepalive timestamp (`common\ControlBus.h`). If the virtual camera DLL stops requesting frames, the tray app puts the hardware to sleep after ~3 seconds.
* The startup task uses the `ITaskService` COM API instead of `schtasks.exe` to bypass battery limits and execution duration limits.
* The tray app requires admin rights to create objects in the `Global\` kernel namespace.
