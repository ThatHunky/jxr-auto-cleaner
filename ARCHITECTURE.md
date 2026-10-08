# JxrAutoCleaner — Technical Documentation

## Table of Contents

1. [Architecture Overview](#architecture-overview)
2. [HDR Conversion Pipeline](#hdr-conversion-pipeline)
3. [Threading Model](#threading-model)
4. [System Integration](#system-integration)
5. [File Operations](#file-operations)
6. [Build System](#build-system)

---

## Architecture Overview

JxrAutoCleaner is a Windows background service built in C++17 using Win32 APIs and the Windows Imaging Component (WIC). It operates as a hidden GUI application (`WinMain`) rather than a true Windows Service to avoid Session 0 isolation issues.

### Core Components

```
┌─────────────────────────────────────────────────────────────┐
│                         Main Thread                          │
│  • Window message pump (tray icon, shutdown signals)        │
│  • System tray icon management                              │
│  • COM initialization (COINIT_APARTMENTTHREADED)            │
└─────────────────────────────────────────────────────────────┘
                              │
                ┌─────────────┴─────────────┐
                │                           │
┌───────────────▼──────────────┐  ┌────────▼────────────────┐
│      Watcher Thread          │  │     Worker Thread       │
│  • ReadDirectoryChangesW     │  │  • Queue consumer       │
│  • Recursive monitoring      │  │  • Idle detection       │
│  • Pushes paths to queue     │  │  • JXR → JPEG convert   │
└──────────────────────────────┘  └─────────────────────────┘
                │                           │
                └──────────┬────────────────┘
                           │
                ┌──────────▼──────────┐
                │  ThreadSafeQueue    │
                │  (std::deque-based) │
                └─────────────────────┘
```

### Key Design Decisions

| Decision                  | Rationale                                                                                                                       |
| ------------------------- | ------------------------------------------------------------------------------------------------------------------------------- |
| **GUI app (not Service)** | Services run in Session 0 and cannot access user's Videos folder or show tray icons. A hidden GUI app runs in the user session. |
| **Per-thread COM init**   | Each thread that uses WIC must call `CoInitializeEx`. Managed via RAII `ComInit` struct.                                        |
| **RAII everywhere**       | All Win32 handles (`HANDLE`, `HKEY`) and COM objects (`IWICBitmapDecoder`, etc.) use RAII wrappers to prevent leaks.            |
| **Static CRT (`/MT`)**    | Matches `libultrahdr`'s build settings to avoid runtime conflicts.                                                              |

---

## HDR Conversion Pipeline

### Input: JXR (JPEG XR)

- **Format**: Microsoft's JPEG XR codec, used by NVIDIA ShadowPlay for HDR screenshots
- **Pixel Formats**: Typically `GUID_WICPixelFormat64bppRGBAHalf` (16-bit half-float per channel)
- **Color Space**: scRGB (linear, BT.709 primaries, extended range beyond [0,1])

### Output: Ultra HDR JPEG

- **Format**: Standard JPEG with embedded ISO 21496-1 gain map
- **Structure**:
  - **Base Image**: 8-bit SDR JPEG (tone-mapped from HDR)
  - **Gain Map**: Embedded metadata describing how to reconstruct HDR from SDR
- **Compatibility**: Displays as normal JPEG on SDR screens, "pops" with HDR on supported devices

### Conversion Steps

```
┌──────────────────────────────────────────────────────────────┐
│ 1. WIC Decode (JXR → Raw Pixels)                            │
│    • CreateDecoderFromFilename(jxrPath)                     │
│    • GetFrame(0) → IWICBitmapFrameDecode                    │
│    • GetPixelFormat() → Check if HDR (64bpp/128bpp)         │
└──────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌──────────────────────────────────────────────────────────────┐
│ 2. Format Conversion (if needed)                            │
│    • If HDR: Convert to GUID_WICPixelFormat64bppRGBAHalf    │
│    • If SDR: Simple JPEG transcode (skip libultrahdr)       │
└──────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌──────────────────────────────────────────────────────────────┐
│ 3. Copy Pixels to Memory                                    │
│    • CopyPixels(nullptr, stride, bufferSize, hdrPixels)     │
│    • Result: std::vector<uint8_t> with raw RGBA half-float  │
└──────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌──────────────────────────────────────────────────────────────┐
│ 4. scRGB → libultrahdr Luminance Rescaling                  │
│    • Scale every pixel by 80/203 (≈0.3941)                  │
│    • Maps scRGB SDR white (1.0 = 80 nits) to libultrahdr's  │
│      expected range (1.0 = 203 nits per BT.2408)            │
│    • Clamp negatives to 0 (out-of-gamut values)             │
└──────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌──────────────────────────────────────────────────────────────┐
│ 5. libultrahdr Encoding (HDR-only mode)                     │
│    • uhdr_create_encoder()                                  │
│    • uhdr_enc_set_raw_image(enc, &hdrImg, UHDR_HDR_IMG)     │
│    • uhdr_enc_set_target_display_peak_brightness(4000 nits) │
│    • uhdr_enc_set_using_multi_channel_gainmap(true)         │
│    • uhdr_enc_set_preset(UHDR_USAGE_BEST_QUALITY)           │
│    •   → Library internally tone-maps to SDR                │
│    •   → Generates multi-channel gain map                   │
│    • uhdr_encode() → produces Ultra HDR JPEG                │
└──────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌──────────────────────────────────────────────────────────────┐
│ 6. Atomic File Replacement                                  │
│    • Write to "original.tmp.jpg"                            │
│    • Rename "original.tmp.jpg" → "original.jpg"             │
│    • Delete "original.jxr" (kept if locked)                 │
└──────────────────────────────────────────────────────────────┘
```

### HDR Preservation Details

**Color Space Mapping**:

- **Input (scRGB)**: Linear RGB, BT.709 primaries, SDR white = 1.0 (~80 nits)
- **libultrahdr expects**: Linear RGB, BT.709, range [0.0..10000/203], where 1.0 = 203 nits (BT.2408)
- **Rescaling**: Multiply all pixel values by `80/203 ≈ 0.3941` to align SDR white points
- **Metadata**: `UHDR_CT_LINEAR`, `UHDR_CG_BT_709`, `UHDR_CR_FULL_RANGE`
- **Negatives**: scRGB allows negative values (out-of-gamut); these are clamped to 0

**Tone Mapping**:

- Performed internally by `libultrahdr` when only `UHDR_HDR_IMG` is provided
- Target display peak brightness set to **4000 nits** (vs. 10000 default)
- Multi-channel gain map enabled for per-channel color accuracy
- Gain map stores the "recovery function" to reconstruct HDR from SDR

**Quality Settings**:

- **Base (SDR) JPEG**: 95 (configurable, default from `jpegQuality` parameter)
- **Gain Map**: 95 (high quality for accurate HDR reconstruction)
- **Encoder Preset**: `UHDR_USAGE_BEST_QUALITY`

---

## Threading Model

### Main Thread

- **Purpose**: UI/message handling, tray icon, shutdown coordination
- **Message Loop**: `GetMessageW` / `DispatchMessageW`
- **Handles**:
  - `WM_TRAYICON` — Tray icon events (right-click menu)
  - `WM_COMMAND` — Menu selections (Force Run, Toggle Startup, Exit)
  - `WM_ENDSESSION` — Windows shutdown/logoff (threads are stopped inside the handler)
  - `TaskbarCreated` — Explorer restarted; re-adds the tray icon
  - `WM_CLOSE` / `WM_DESTROY` — Application exit

### Watcher Thread

- **Purpose**: Monitor the Videos folder for new `.jxr` files
- **API**: `ReadDirectoryChangesW` with `FILE_FLAG_OVERLAPPED`
- **Behavior**:
  - Recursive monitoring (`bWatchSubtree = TRUE`)
  - Filter: `FILE_NOTIFY_CHANGE_FILE_NAME` (only adds/renames matter)
  - On new `.jxr` detected → push full path to `g_queue`
  - Waits on `{hEvent, g_shutdownEvent}` to handle both file changes and shutdown
- **Buffer Overflow Handling**: If too many changes occur at once (`ERROR_NOTIFY_ENUM_DIR` or 0 bytes returned), performs a full directory scan

### Worker Thread

- **Purpose**: Process queued files and perform conversions
- **Flow**:
  1. `g_queue.wait_and_pop(30s)` — blocks until a file is available
  2. **Idle Check**: `IsSystemBusy()` — checks gaming state and CPU load
     - If busy → re-queue file, sleep 30s, retry (logged once per busy period)
  3. **File Lock Check**: Attempts exclusive `CreateFileW` with retries (ShadowPlay may still be writing)
  4. **Conversion**: `ConvertJxrToUltraHdrJpeg(filePath)`
  5. Repeat until `g_shutdownEvent` is signaled

### Synchronization

| Primitive                                      | Purpose                                           |
| ---------------------------------------------- | ------------------------------------------------- |
| `g_shutdownEvent` (manual-reset event)         | Signals all threads to exit gracefully            |
| `ThreadSafeQueue` (mutex + condition_variable) | Thread-safe FIFO for file paths                   |
| Per-thread `ComInit`                           | Ensures each thread initializes COM independently |

---

## System Integration

### Startup Mechanism

- **Registry Key**: `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`
- **Value Name**: `JxrAutoCleaner`
- **Value Data**: Full path to `JxrAutoCleaner.exe`
- **Toggle**: Via tray menu or MSI installer

### System Tray Icon

- **API**: `Shell_NotifyIconW` with `NOTIFYICON_VERSION_4`
- **Icon**: Loaded from embedded resource (`IDI_ICON1`)
- **Tooltip**: "JxrAutoCleaner v1.1.2"
- **Explorer restarts**: the hidden window handles the `TaskbarCreated` broadcast and re-adds the icon
- **Context Menu**:
  - **Force Run Now** → `ForceScanNow()` — scans Videos folder, queues all unconverted `.jxr` files
  - **Toggle Startup** → `AddToStartup()` / `RemoveFromStartup()`
  - **Exit** → `RemoveTrayIcon()`, `SetEvent(g_shutdownEvent)`, `PostQuitMessage(0)`

### Idle Detection

**Gaming / Fullscreen Detection**:

```cpp
QUERY_USER_NOTIFICATION_STATE state;
SHQueryUserNotificationState(&state);
bool isGaming = (state == QUNS_BUSY ||
                 state == QUNS_RUNNING_D3D_FULL_SCREEN ||
                 state == QUNS_PRESENTATION_MODE);
```

**CPU Load Sampling**:

```cpp
GetSystemTimes(&idleA, &kernelA, &userA);
Sleep(1000); // 1-second sample window
GetSystemTimes(&idleB, &kernelB, &userB);
double cpuPercent = (1.0 - (double)idle / (double)total) * 100.0;
```

**Threshold**: Conversion is deferred if CPU > 25% or gaming is detected.

---

## File Operations

### Atomic Replacement Strategy

To prevent data loss or corruption:

1. **Write to Temp**: `original.tmp.jpg` (write errors such as disk full delete the temp and keep the original)
2. **Rename Temp**: `fs::rename(original.tmp.jpg, original.jpg)`
   - If this fails → delete the temp, keep the original
3. **Delete Original**: `fs::remove(original.jxr)`
   - If locked → log warning, keep both files

The rename happens before the delete so there is no moment where neither a finished `.jpg` nor the original `.jxr` exists.

### File Lock Handling

**Problem**: ShadowPlay may still be writing the JXR when the watcher detects it.

**Solution**: Retry loop with exclusive access test:

```cpp
for (int retry = 0; retry < 5; ++retry) {
  HANDLE hFile = CreateFileW(path, GENERIC_READ, 0, ...); // No sharing
  if (hFile != INVALID_HANDLE_VALUE) {
    CloseHandle(hFile);
    fileReady = true;
    break;
  }
  if (GetLastError() == ERROR_SHARING_VIOLATION) {
    Sleep(2000); // Wait and retry
  }
}
```

### Orphan Cleanup

When the worker thread starts, before it converts anything, it scans for leftover `.tmp.jpg` files from previous crashes. The worker is the only writer of temp files, so none can be mid-write at that point.

- If the matching `.jxr` still exists, the temp file is a partial write and is deleted.
- If the `.jxr` is gone, the temp file ends with the JPEG EOI marker (`FF D9`), and no `.jpg` exists, it is the finished output left by pre-1.1.2 versions (which deleted before renaming). It is renamed to `.jpg` instead of being thrown away.
- In every other case (truncated, name taken, or the existence check errored), the file is left alone and logged.

---

## Build System

### CMake Configuration

```cmake
# C++17, static CRT to match libultrahdr
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# libultrahdr as git submodule
add_subdirectory(third_party/libultrahdr)

# Main executable (WIN32 = no console)
add_executable(JxrAutoCleaner WIN32
    src/main.cpp
    src/Converter.cpp
    src/SystemCheck.cpp
    src/FileWatcher.cpp
    src/resources.rc
)

target_link_libraries(JxrAutoCleaner PRIVATE
    uhdr-static      # libultrahdr
    windowscodecs    # WIC
    ole32 shlwapi shell32
)
```

### Dependencies

| Library                             | Purpose                            | Integration                          |
| ----------------------------------- | ---------------------------------- | ------------------------------------ |
| **libultrahdr**                     | Ultra HDR JPEG encoding            | Git submodule, static lib            |
| **libjpeg-turbo**                   | JPEG codec (pulled by libultrahdr) | Transitive dependency                |
| **Windows Imaging Component (WIC)** | JXR decoding                       | System library (`windowscodecs.lib`) |
| **Shell APIs**                      | Tray icon, notification state      | System library (`shell32.lib`)       |

### MSI Installer (WiX v6)

```xml
<Package Scope="perUser">
  <StandardDirectory Id="LocalAppDataFolder">
    <Directory Id="INSTALLFOLDER" Name="JxrAutoCleaner">
      <Component>
        <File Source="JxrAutoCleaner.exe">
          <Shortcut Directory="ProgramMenuFolder" />
        </File>
        <RegistryValue Root="HKCU" Key="...\Run" />
      </Component>
    </Directory>
  </StandardDirectory>
</Package>
```

**Features**:

- Per-user install (no admin/UAC)
- Installs to `%LOCALAPPDATA%\JxrAutoCleaner\`
- Adds startup registry key
- Creates Start Menu shortcut
- Clean uninstall via "Apps & Features"

---

## Performance Characteristics

| Metric                  | Value                                          |
| ----------------------- | ---------------------------------------------- |
| **Memory Footprint**    | ~15 MB (mostly libultrahdr)                    |
| **CPU (Idle)**          | <0.1% (event-driven, no polling)               |
| **CPU (Converting)**    | 5-15% (single-threaded, depends on image size) |
| **Conversion Speed**    | ~2-3 seconds for 4K HDR screenshot             |
| **File Size Reduction** | ~89% (11 MB JXR → 1.3 MB Ultra HDR JPEG)       |

---

## Error Handling

### Memory Safety

- **RAII**: All resources (handles, COM objects) use RAII wrappers
- **No raw pointers**: `std::unique_ptr`, `std::vector`, `ComPtr<T>`
- **Exception safety**: Minimal use of exceptions; most errors return `bool` or `HRESULT`

### Logging

- **Location**: `%LOCALAPPDATA%\JxrAutoCleaner\log.txt`
- **Format**: `[YYYY-MM-DD HH:MM:SS] message`
- **Thread-safe**: Writes are serialized by a mutex, per-call `fopen`/`fclose` in append mode
- **Encoding**: UTF-8 (non-ASCII paths such as Cyrillic user names log correctly)
- **Rotation**: Trimmed to the last 500 lines on startup (temp file + atomic replace)

### Known Edge Cases

| Case                                   | Handling                                  |
| -------------------------------------- | ----------------------------------------- |
| **File locked by ShadowPlay**          | Retry 5 times with 2s delay, then skip    |
| **Disk full during write**             | Temp file write fails, original preserved |
| **HDR format WIC can't convert**       | Logs error, keeps the JXR untouched       |
| **Corrupt JXR**                        | WIC decode fails, logs error, skips file  |
| **Non-HDR JXR**                        | Falls back to simple WIC JPEG transcode   |
| **Buffer overflow (too many changes)** | Fallback to full directory scan           |

---

## Future Enhancements

- [ ] Support for other HDR formats (AVIF, HEIF)
- [ ] Configurable quality settings via tray menu
- [ ] Batch conversion UI mode
- [ ] Automatic backup before deletion (optional)
- [ ] Multi-threaded conversion (worker pool)
