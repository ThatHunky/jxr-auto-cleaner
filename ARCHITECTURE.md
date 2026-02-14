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
│ 4. libultrahdr Encoding (HDR-only mode)                     │
│    • uhdr_create_encoder()                                  │
│    • uhdr_enc_set_raw_image(enc, &hdrImg, UHDR_HDR_IMG)     │
│    •   → Library internally tone-maps to SDR                │
│    •   → Generates gain map                                 │
│    • uhdr_encode() → produces Ultra HDR JPEG                │
└──────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌──────────────────────────────────────────────────────────────┐
│ 5. Atomic File Replacement                                  │
│    • Write to "original.tmp.jpg"                            │
│    • Delete "original.jxr" (with retry for locks)           │
│    • Rename "original.tmp.jpg" → "original.jpg"             │
└──────────────────────────────────────────────────────────────┘
```

### HDR Preservation Details

**Color Space Mapping**:

- **Input (scRGB)**: Linear RGB, BT.709 primaries, range [-0.5, 7.5]
- **libultrahdr expects**: Linear RGB, BT.709, full range
- **Mapping**: Direct pass-through — `UHDR_CT_LINEAR`, `UHDR_CG_BT_709`, `UHDR_CR_FULL_RANGE`

**Tone Mapping**:

- Performed internally by `libultrahdr` when only `UHDR_HDR_IMG` is provided
- Uses a perceptual tone curve optimized for mobile displays
- Gain map stores the "recovery function" to reconstruct HDR from SDR

**Quality Settings**:

- **Base (SDR) JPEG**: 95 (configurable, default from `jpegQuality` parameter)
- **Gain Map**: 85 (fixed, balances quality vs. file size)

---

## Threading Model

### Main Thread

- **Purpose**: UI/message handling, tray icon, shutdown coordination
- **Message Loop**: `GetMessageW` / `DispatchMessageW`
- **Handles**:
  - `WM_TRAYICON` — Tray icon events (right-click menu)
  - `WM_COMMAND` — Menu selections (Force Run, Toggle Startup, Exit)
  - `WM_ENDSESSION` — Windows shutdown/logoff
  - `WM_CLOSE` / `WM_DESTROY` — Application exit

### Watcher Thread

- **Purpose**: Monitor the Videos folder for new `.jxr` files
- **API**: `ReadDirectoryChangesW` with `FILE_FLAG_OVERLAPPED`
- **Behavior**:
  - Recursive monitoring (`bWatchSubtree = TRUE`)
  - Filters: `FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE`
  - On new `.jxr` detected → push full path to `g_queue`
  - Waits on `{hEvent, g_shutdownEvent}` to handle both file changes and shutdown
- **Buffer Overflow Handling**: If too many changes occur at once, performs a full directory scan

### Worker Thread

- **Purpose**: Process queued files and perform conversions
- **Flow**:
  1. `g_queue.wait_and_pop(30s)` — blocks until a file is available
  2. **Idle Check**: `IsSystemBusy()` — checks gaming state and CPU load
     - If busy → re-queue file, sleep 30s, retry
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
- **Tooltip**: "JxrAutoCleaner v1.0"
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

1. **Write to Temp**: `original.tmp.jpg`
2. **Delete Original**: `fs::remove(original.jxr)`
   - If locked → log warning, keep both files
   - Retry logic for transient locks
3. **Rename Temp**: `fs::rename(original.tmp.jpg, original.jpg)`

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

On startup, scans for leftover `.tmp.jpg` files from previous crashes and deletes them.

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
- **Thread-safe**: Uses file append mode with per-call `fopen`/`fclose`

### Known Edge Cases

| Case                                   | Handling                                  |
| -------------------------------------- | ----------------------------------------- |
| **File locked by ShadowPlay**          | Retry 5 times with 2s delay, then skip    |
| **Disk full during write**             | Temp file write fails, original preserved |
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
