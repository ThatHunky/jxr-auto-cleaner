# JxrAutoCleaner v1.0

**Automatic JXR to Ultra HDR JPEG conversion for NVIDIA ShadowPlay screenshots**

## 🎉 Initial Release

JxrAutoCleaner is a lightweight Windows background service that automatically converts NVIDIA ShadowPlay JXR screenshots to Ultra HDR JPEGs, preserving HDR information while reducing file size by ~90%.

## ✨ Features

- **Seamless Background Conversion**: Monitors your Videos folder and automatically converts new `.jxr` files
- **HDR Preservation**: Uses Google's `libultrahdr` to create Ultra HDR JPEGs with embedded gain maps
- **Smart Idle Detection**: Pauses conversion when gaming or CPU is busy (>25%)
- **System Tray Integration**: Easy access to Force Run, Toggle Startup, and Exit
- **File Size Reduction**: ~89% smaller files (11 MB JXR → 1.3 MB Ultra HDR JPEG)
- **Per-User Installation**: No admin/UAC required, installs to `%LOCALAPPDATA%`

## 📦 Installation

1. Download `JxrAutoCleaner-v1.0.msi` from the Assets below
2. Run the installer (no admin rights needed)
3. Follow the on-screen prompts to install
4. **Launch JxrAutoCleaner** directly from the success screen, or find it in your Start Menu
5. Look for the tray icon in your notification area

## 🔧 System Requirements

- Windows 10/11 (64-bit)
- .NET Framework 4.8+ (usually pre-installed)
- ~15 MB disk space

## 📖 Documentation

- [README.md](https://github.com/YOUR_USERNAME/JxrAutoCleaner/blob/main/README.md) — User guide and features
- [ARCHITECTURE.md](https://github.com/YOUR_USERNAME/JxrAutoCleaner/blob/main/ARCHITECTURE.md) — Technical documentation

## 📜 License

Released under the **MIT License**. See [LICENSE](LICENSE) for details.

## 🐛 Known Issues

None reported yet. Please open an issue if you encounter any problems!

## 🙏 Credits

- **libultrahdr** — Google's Ultra HDR JPEG library
- **Windows Imaging Component (WIC)** — JXR decoding
- Developed by **ThatHunky** & **Antigravity**

---

**Full Changelog**: Initial release
