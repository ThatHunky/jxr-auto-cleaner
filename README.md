# JxrAutoCleaner

A lightweight Windows background service that automatically converts NVIDIA ShadowPlay JXR screenshots to **Ultra HDR JPEGs**.

Vibe coded, but I don't care.

## Why JxrAutoCleaner?

High Dynamic Range (HDR) screenshots taken via NVIDIA ShadowPlay are saved in the `.jxr` (JPEG XR) format. While JXR preserves HDR data, it is not widely supported by web browsers, social media, or standard image viewers.

**JxrAutoCleaner** solves this by monitoring your Videos folder and automatically converting new JXR files into the **Ultra HDR JPEG** format. This format embeds an HDR gain map into a standard JPEG, allowing the image to look great on SDR displays while "popping" with full HDR luminance on supported devices (like modern Android phones, iPhones, and Chrome/Edge on Windows).

## Key Features

- **Seamless Conversion**: Converts JXR to Ultra HDR JPEG with ~90% file size reduction while preserving HDR luminance.
- **Background Autopilot**: Runs as a low-resource background service with a system tray icon.
- **Intelligent Idle Detection**: Automatically pauses conversion when you are gaming (fullscreen) or the CPU load is high to ensure zero impact on your performance.
- **Startup Integration**: Easily toggle "Launch at Startup" directly from the tray icon.
- **Smart File Watcher**: Uses low-level Windows APIs (`ReadDirectoryChangesW`) to detect new screenshots instantly.
- **Atomic Operations**: Safe file replacement logic ensures your original screenshots are never corrupted.

## Usage

1. **Install**: Run the `JxrAutoCleaner-v1.1.2.msi` installer. It will install to your local AppData folder and register itself to run at startup.
2. **Setup**: By default, it monitors your Windows "Videos" library (where ShadowPlay typically saves screenshots).
3. **Tray Icon**: Look for the icon in your system tray. Right-click it to:
   - **Force Run Now**: Manually trigger a scan of your folders.
   - **Toggle Startup**: Enable or disable launch at login.
   - **Exit**: Stop the service.

## Manual Mode

You can also use the service as a CLI tool for manual conversions:

```powershell
.\JxrAutoCleaner.exe --convert "C:\Path\To\Screenshot.jxr"
```

## Build Instructions

Requirements:

- Windows 10/11
- Visual Studio 2022 (with C++ Desktop development)
- CMake 3.20+
- WiX Toolset v4+ (for building the installer)

To build:

1. Run `setup.bat`. This will initialize submodules, configure CMake, and build both the executable and the MSI installer.
2. The output will be in `build/Release/` and `build/`.

## Technical Documentation

For detailed information about the internal architecture, HDR conversion pipeline, threading model, and system integration, see [ARCHITECTURE.md](ARCHITECTURE.md).

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

### Third-Party Licenses

- **libultrahdr**: [Apache License 2.0](https://github.com/google/libultrahdr/blob/main/LICENSE)
- **libjpeg-turbo**: [IJG / BSD-style](https://github.com/libjpeg-turbo/libjpeg-turbo/blob/main/LICENSE.md)

## Credits

- **libultrahdr**: Used for Ultra HDR JPEG encoding and gain map generation.
- **Windows Imaging Component (WIC)**: Used for high-depth JXR decoding.
- Developed by **ThatHunky** & **Antigravity**.
