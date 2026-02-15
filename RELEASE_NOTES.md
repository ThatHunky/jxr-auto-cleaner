# JxrAutoCleaner v1.1

**Improved HDR conversion quality — accurate tone mapping and richer dynamic range**

## 🔧 What's Changed

### HDR Conversion Quality Overhaul

The previous version had a luminance range mismatch that caused overblown highlights, washed-out SDR fallback, and underutilized HDR dynamic range. This release fixes all three root causes:

- **Fixed scRGB → libultrahdr luminance mapping**: JXR screenshots use scRGB where SDR white = 1.0 (~80 nits), but libultrahdr expects 1.0 = 203 nits (BT.2408). Pixel values are now correctly rescaled by 80/203 before encoding, eliminating the ~2.5× brightness overestimation.
- **Reduced target display peak brightness**: Changed from 10,000 nits (wasteful for gaming displays) to 4,000 nits, focusing gain map precision on the range monitors actually support (600–2,000 nits).
- **Enabled multi-channel gain map**: Preserves per-channel color accuracy in bright highlights, preventing hue shifts in colored specular areas.
- **Best quality encoder preset**: Now uses `UHDR_USAGE_BEST_QUALITY` for optimized encoding.
- **Increased gain map quality**: 85 → 95, improving HDR reconstruction with negligible file size impact.

### Result

| Aspect       | v1.0                       | v1.1                                         |
| ------------ | -------------------------- | -------------------------------------------- |
| Highlights   | Overblown / clipped        | Accurate, matching original JXR              |
| SDR fallback | Harsh clipping, washed out | Natural tone mapping                         |
| HDR pop      | Underutilized range        | Full dynamic range                           |
| Gain map     | Single-channel, 85 quality | Multi-channel, 95 quality                    |
| File sizes   | ~1.3 MB                    | ~1.5 MB (slightly larger for better quality) |

## 📦 Upgrade Instructions

**Existing v1.0 users**: Simply run `JxrAutoCleaner-v1.1.msi`. The installer will automatically upgrade -- no need to uninstall the old version first.

**New users**: Download `JxrAutoCleaner-v1.1.msi` from the Assets below and run it.

## 🔧 System Requirements

- Windows 10/11 (64-bit)
- ~15 MB disk space

## 📖 Documentation

- [README.md](https://github.com/ThatHunky/jxr-auto-cleaner/blob/main/README.md) — User guide and features
- [ARCHITECTURE.md](https://github.com/ThatHunky/jxr-auto-cleaner/blob/main/ARCHITECTURE.md) — Technical documentation

## 📜 License

Released under the **MIT License**. See [LICENSE](LICENSE) for details.

## 🙏 Credits

- **libultrahdr** — Google's Ultra HDR JPEG library
- **Windows Imaging Component (WIC)** — JXR decoding
- Developed by **ThatHunky** & **Antigravity**

---

**Full Changelog**: [v1.0...v1.1](https://github.com/ThatHunky/jxr-auto-cleaner/compare/v1.0...v1.1)
