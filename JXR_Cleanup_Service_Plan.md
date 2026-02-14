# Plan: JXR → Ultra HDR JPEG Background Converter

## 1. Overview

A lightweight, invisible background utility for Windows (C++17, Win32) that monitors NVIDIA ShadowPlay's output folders for new `.jxr` HDR screenshots. When the system is idle, it converts each JXR file to an **Ultra HDR JPEG** (gain-map JPEG) and replaces the original.

### Why Ultra HDR JPEG instead of standard JPEG?

> **⚠ CRITICAL ISSUE IN PREVIOUS PLAN:** Standard JPEG is an 8-bit, SDR-only format.
> Converting a JXR's 10–16-bit HDR pixel data directly into a standard JPEG would
> **permanently destroy all HDR information** — clipping highlights, crushing shadows,
> and discarding the wide color gamut. This defeats the entire purpose of HDR screenshots.

**Solution: Ultra HDR JPEG (Gain Map JPEG)**

- Embeds a standard 8-bit SDR JPEG as the "base image" (backward compatible — opens in any viewer).
- Also embeds a **gain map** (a low-res grayscale image) that allows HDR-capable displays to reconstruct the full HDR rendition.
- Standardized under **ISO 21496-1**; supported by Android 14+, iOS 18+, Chrome, Edge, Windows 11 Photos.
- File extension remains `.jpg` — completely transparent to the user.

### Project Goals

| Property                | Value                                            |
| ----------------------- | ------------------------------------------------ |
| **Target OS**           | Windows 10 22H2+ / Windows 11                    |
| **Language**            | C++17 (Win32 API + one external lib)             |
| **External Dependency** | `libultrahdr` (Google, Apache 2.0) — static link |
| **Memory Safety**       | All COM via `Microsoft::WRL::ComPtr` (RAII)      |
| **Footprint**           | < 5 MB RAM idle, ~0% CPU when idle               |

---

## 2. Architecture

### A. Process Model — Background Application (Not a "Service")

A Windows Service runs in **Session 0**, which cannot access the interactive user's `%USERPROFILE%` folders reliably. Instead, we build a **hidden GUI-less application** (`WinMain` subsystem:windows).

| Aspect          | Detail                                                                                                                       |
| --------------- | ---------------------------------------------------------------------------------------------------------------------------- |
| Entry point     | `wWinMain` (no console window)                                                                                               |
| Auto-start      | Registry key `HKCU\...\Run` or Startup folder shortcut                                                                       |
| Visibility      | No window, no tray icon (true invisible)                                                                                     |
| Shutdown        | Responds to `WM_ENDSESSION` / `WM_QUERYENDSESSION` for clean exit; also `SetConsoleCtrlHandler` if debugging in console mode |
| Single instance | Named mutex (`Global\JxrAutoCleanerMutex`) prevents duplicate launches                                                       |

### B. Thread Model

```
┌─────────────────────────────────────────────────────┐
│  Main Thread (Message Pump)                         │
│  - Creates hidden HWND for WM_ENDSESSION            │
│  - Initializes COM (COINIT_MULTITHREADED)            │
│  - Spawns Watcher & Worker threads                   │
│  - Waits on shutdown event                           │
└──────────────┬──────────────────┬────────────────────┘
               │                  │
    ┌──────────▼──────┐  ┌────────▼─────────────────┐
    │  Watcher Thread  │  │  Worker Thread            │
    │  (I/O bound)     │  │  (CPU bound, throttled)   │
    │                  │  │                           │
    │  ReadDirectory-  │  │  while (!shutdown) {      │
    │  ChangesW loop   │  │    WaitOnQueue(30s)       │
    │                  │  │    if (IsSystemBusy())    │
    │  → pushes paths  │  │      continue;            │
    │    into Queue     │  │    path = queue.pop()     │
    │                  │  │    ConvertJxrToUltraHdr() │
    └──────────────────┘  │  }                        │
                          └───────────────────────────┘
```

**Thread synchronization:**

- `std::mutex` + `std::condition_variable` protect the shared `std::deque<std::wstring>` queue.
- A manual-reset `HANDLE` event (`CreateEvent`) signals shutdown to all threads.

### C. Core Loop (Detailed)

#### Step 1 — Startup

1. `CreateMutex` — exit if already running.
2. Resolve `Videos` path via `SHGetKnownFolderPath(FOLDERID_Videos, ...)`.
3. Initialize COM on the worker thread: `CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)`.
4. Create `IWICImagingFactory` once (reuse for all conversions).
5. Spawn Watcher thread, spawn Worker thread.

#### Step 2 — File Watching (Watcher Thread)

1. `CreateFile` on the Videos folder with `FILE_LIST_DIRECTORY` access.
2. Loop: `ReadDirectoryChangesW(hDir, ..., TRUE, FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE, ...)`.
3. For each `FILE_ACTION_ADDED` or `FILE_ACTION_RENAMED_NEW_NAME`:
   - Check if extension is `.jxr` (case-insensitive).
   - Push full absolute path to the thread-safe queue.
   - Signal the condition variable.
4. Wait on both the directory handle and the shutdown event (`WaitForMultipleObjects`).

#### Step 3 — Processing (Worker Thread)

1. Wait on the condition variable (with 30-second timeout so we can re-check shutdown).
2. **Busy Check** (see Section 4) — if busy, skip this cycle.
3. Peek the queue; dequeue one file path.
4. **File-readiness check**: ShadowPlay may still be writing the file.
   - Attempt to open the file with `GENERIC_READ | GENERIC_WRITE` and `FILE_SHARE_NONE`.
   - If it fails with `ERROR_SHARING_VIOLATION`, requeue and sleep 2 seconds. Retry up to 5 times.
5. **Convert**: Call `ConvertJxrToUltraHdrJpeg(inputPath)`.
6. **Replace**: Write to `<same_name>.jpg` in a temp file first, then:
   - `DeleteFile(original.jxr)`
   - `MoveFile(temp.jpg, final.jpg)`
   - This atomic-ish swap prevents data loss if the process crashes mid-write.

---

## 3. HDR Preservation Pipeline (The Critical Part)

### The Problem

JXR files from ShadowPlay contain **HDR pixel data**:

- Typically 16-bit half-float per channel (scRGB or HDR10/BT.2100 PQ).
- WIC pixel formats: `GUID_WICPixelFormat64bppRGBAHalf` or `GUID_WICPixelFormat128bppRGBAFloat`.

### The Solution: Two-Rendition Encoding

```
┌──────────────────────────────────────────────────────────────┐
│                    JXR Input File                             │
│          (scRGB, 16-bit half float, HDR)                     │
└─────────────────────┬────────────────────────────────────────┘
                      │  WIC Decode
                      ▼
            ┌─────────────────────┐
            │  HDR Pixel Buffer   │
            │  (128bpp RGBA Float)│
            └────┬────────────┬───┘
                 │            │
          Tone Map            │  (keep original)
          to SDR              │
                 │            │
            ┌────▼────┐  ┌───▼────────────┐
            │ SDR buf  │  │ HDR buf        │
            │ 8-bit    │  │ float/half     │
            │ sRGB     │  │ scRGB/BT.2100  │
            └────┬─────┘  └───┬────────────┘
                 │            │
                 └──────┬─────┘
                        │
               libultrahdr encode
                        │
                        ▼
            ┌───────────────────────┐
            │  Ultra HDR JPEG       │
            │  (SDR base + gain map │
            │   + HDR metadata)     │
            └───────────────────────┘
```

### Step-by-Step WIC + libultrahdr Pipeline

1. **Decode JXR via WIC:**
   ```
   ComPtr<IWICBitmapDecoder>    → CreateDecoderFromFilename(path, CLSID_WICJxrDecoder)
   ComPtr<IWICBitmapFrameDecode> → decoder->GetFrame(0, &frame)
   ```
2. **Read HDR pixels into a float buffer:**
   ```
   ComPtr<IWICFormatConverter> → Convert to GUID_WICPixelFormat128bppRGBAFloat
   frame->CopyPixels(nullptr, stride, bufferSize, (BYTE*)hdrBuffer.data())
   ```
3. **Tone-map HDR → SDR (for the base JPEG layer):**
   - Use WIC's `IWICColorTransform` to convert from scRGB → sRGB (8-bit).
   - Or perform a simple Reinhard / ACES filmic tone map in a loop over the float buffer.
   - Clamp to [0, 1], gamma-correct, quantize to 8-bit.
   - This SDR image is what non-HDR viewers will see.
4. **Feed both buffers to `libultrahdr`:**
   ```cpp
   ultrahdr_codec codec;
   // set HDR intent (scRGB linear float)
   // set SDR rendition (sRGB 8-bit)
   // libultrahdr computes the gain map automatically
   // outputs a single JPEG buffer with embedded gain map + metadata
   ```
5. **Write the output buffer to disk** as `filename.jpg`.

### Tone Mapping Strategy

For the SDR base layer, we need a tone mapper. Options:

| Method                                | Quality                    | Complexity                        |
| ------------------------------------- | -------------------------- | --------------------------------- |
| WIC `IWICColorTransform` (scRGB→sRGB) | Medium — clips highlights  | Very low                          |
| Reinhard global                       | Good                       | Low (10 lines of code)            |
| ACES Filmic                           | Excellent — cinematic look | Low (20 lines of code)            |
| **libultrahdr built-in**              | Good — Google's default    | Zero (it can tone-map internally) |

**Recommendation:** Use `libultrahdr`'s built-in tone mapper. The library's API supports providing _only_ the HDR image, and it will internally generate the SDR rendition and compute the gain map. This eliminates the need for a custom tone mapper entirely.

---

## 4. System Load Detection (`IsSystemBusy()`)

### Check 1: Gaming / Fullscreen Detection

```cpp
QUERY_USER_NOTIFICATION_STATE state;
SHQueryUserNotificationState(&state);

bool isGaming = (state == QUNS_BUSY ||                    // generic fullscreen app
                 state == QUNS_RUNNING_D3D_FULL_SCREEN ||  // D3D exclusive fullscreen
                 state == QUNS_PRESENTATION_MODE);         // presentation mode
```

### Check 2: CPU Load

```cpp
// Sample GetSystemTimes() twice, 1 second apart
FILETIME idleA, kernelA, userA, idleB, kernelB, userB;
GetSystemTimes(&idleA, &kernelA, &userA);
Sleep(1000);
GetSystemTimes(&idleB, &kernelB, &userB);

ULONGLONG idle   = SubtractFileTimes(idleB, idleA);
ULONGLONG kernel = SubtractFileTimes(kernelB, kernelA);
ULONGLONG user   = SubtractFileTimes(userB, userA);
ULONGLONG total  = kernel + user;

double cpuPercent = (1.0 - (double)idle / (double)total) * 100.0;
bool isCpuBusy = cpuPercent > 25.0;  // configurable threshold
```

### Combined Logic

```cpp
bool IsSystemBusy() {
    if (IsGamingOrFullscreen()) return true;
    if (GetCpuUsagePercent() > 25.0) return true;
    return false;
}
```

When busy: the worker thread sleeps for **30 seconds** before re-checking. Files remain safely in the queue.

---

## 5. Memory Safety & Leak Prevention

### Rule 1: All COM pointers use `ComPtr<T>` (RAII)

```cpp
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;

// GOOD — automatically releases when going out of scope
ComPtr<IWICImagingFactory> factory;
ComPtr<IWICBitmapDecoder> decoder;
ComPtr<IWICBitmapFrameDecode> frame;
ComPtr<IWICFormatConverter> converter;
ComPtr<IWICBitmapEncoder> encoder;
ComPtr<IWICBitmapFrameEncode> encFrame;
ComPtr<IWICStream> stream;

// BAD — never do this:
// IWICImagingFactory* factory = nullptr;
// ...
// factory->Release(); // easy to forget, especially on error paths
```

### Rule 2: Pixel buffers use `std::vector<uint8_t>` or `std::unique_ptr<uint8_t[]>`

Never use raw `new[]` / `malloc` for pixel data.

```cpp
// HDR buffer (128bpp float = 16 bytes per pixel)
std::vector<float> hdrPixels(width * height * 4);

// SDR buffer (32bpp = 4 bytes per pixel)
std::vector<uint8_t> sdrPixels(width * height * 4);
```

### Rule 3: File handles wrapped in RAII

```cpp
struct HandleDeleter {
    void operator()(HANDLE h) {
        if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
};
using UniqueHandle = std::unique_ptr<void, HandleDeleter>;

UniqueHandle hDir(CreateFileW(videosPath.c_str(), ...));
```

### Rule 4: COM initialization is scoped per-thread

```cpp
struct ComInit {
    ComInit()  { CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); }
    ~ComInit() { CoUninitialize(); }
};

// In worker thread:
void WorkerThread() {
    ComInit com;  // released when thread exits
    // ... all WIC work here ...
}
```

### Rule 5: Temp file cleanup on crash

If the process crashes between creating `temp.jpg` and deleting `original.jxr`, we could leave orphan temp files. On startup, scan for `*.tmp.jpg` files in the Videos tree and delete them.

---

## 6. Error Handling & Edge Cases

| Scenario                                     | Handling                                                                                                                            |
| -------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------- |
| **File locked by ShadowPlay**                | Retry up to 5 times with 2s delay, then move to a "failed" list and log                                                             |
| **Corrupt JXR**                              | WIC decoder returns `WINCODEC_ERR_BADIMAGE` → log, skip file, do not retry                                                          |
| **Disk full**                                | `WriteFile` fails → log, keep original JXR intact, retry later                                                                      |
| **Videos folder doesn't exist**              | `SHGetKnownFolderPath` fails → exit with error logged to Event Log                                                                  |
| **No HDR metadata in JXR**                   | Some JXR files may be SDR (8-bit). Detect pixel format; if 8-bit, do a simple WIC transcode (JXR→JPEG) without libultrahdr          |
| **Process already running**                  | Named mutex check → second instance silently exits                                                                                  |
| **User logs off / shutdown**                 | `WM_ENDSESSION` handler sets shutdown event → threads drain cleanly                                                                 |
| **ReadDirectoryChangesW buffer overflow**    | If too many changes happen at once, the API returns 0 bytes. On this event, do a full directory scan for `.jxr` files as a fallback |
| **Extremely large file (100+ MB panoramic)** | Process in tiles or limit max file size to prevent excessive memory usage                                                           |

---

## 7. Folder Structure

```text
jxr-to-jpeg/
├── CMakeLists.txt              // Build system
├── README.md
├── HDR_and_JXR_Study.md        // Research doc (existing)
├── JXR_Cleanup_Service_Plan.md // This file
├── src/
│   ├── main.cpp                // Entry point, message pump, thread orchestration
│   ├── Converter.h             // ConvertJxrToUltraHdr() declaration
│   ├── Converter.cpp           // WIC decode + libultrahdr encode pipeline
│   ├── SystemCheck.h           // IsSystemBusy(), IsGaming(), GetCpuUsage()
│   ├── SystemCheck.cpp
│   ├── FileWatcher.h           // ReadDirectoryChangesW wrapper
│   ├── FileWatcher.cpp
│   ├── ThreadSafeQueue.h       // std::deque + mutex + condition_variable
│   └── Utils.h                 // UniqueHandle, ComInit, logging helpers
├── third_party/
│   └── libultrahdr/            // Git submodule or vendored source
│       ├── CMakeLists.txt
│       └── ...
└── build/                      // CMake output
    └── JxrAutoCleaner.exe
```

---

## 8. Build System (CMake)

```cmake
cmake_minimum_required(VERSION 3.20)
project(JxrAutoCleaner LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Build libultrahdr as a static library
add_subdirectory(third_party/libultrahdr)

add_executable(JxrAutoCleaner WIN32   # WIN32 = subsystem:windows (no console)
    src/main.cpp
    src/Converter.cpp
    src/SystemCheck.cpp
    src/FileWatcher.cpp
)

target_link_libraries(JxrAutoCleaner PRIVATE
    ultrahdr          # libultrahdr static lib
    windowscodecs     # WIC
    ole32             # COM
    shlwapi           # SHGetKnownFolderPath
    shell32           # SHQueryUserNotificationState
)

target_include_directories(JxrAutoCleaner PRIVATE
    src
    third_party/libultrahdr/lib/include
)
```

**Build commands:**

```bash
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

---

## 9. Implementation Phases

### Phase 1: Standalone Converter (Proof of Concept)

- **Goal**: A console app that takes `input.jxr` and produces `output.jpg` (Ultra HDR).
- **Validates**: WIC JXR decoding, pixel format handling, libultrahdr integration.
- **Test**: Open the output `.jpg` in Windows Photos with an HDR monitor and verify HDR rendition appears.

### Phase 2: System Monitor

- **Goal**: `IsSystemBusy()` working and tested.
- **Test**: Run while playing a game — confirm it returns `true`. Alt-tab out — confirm `false`.

### Phase 3: File Watcher

- **Goal**: `ReadDirectoryChangesW` loop detecting `.jxr` files in Videos subfolders.
- **Test**: Copy a `.jxr` file into `Videos\Test\` and confirm it appears in the queue.

### Phase 4: Full Integration

- **Goal**: All components wired together. Background process converts automatically.
- **Test**: Launch a game, take an ShadowPlay screenshot (Alt+F1), alt-tab, wait ~30 seconds, verify `.jxr` is replaced by `.jpg`.

### Phase 5: Polish

- Add startup registry entry / Task Scheduler task.
- Add minimal logging (to a `.log` file in `%LOCALAPPDATA%\JxrAutoCleaner\`).
- Handle edge cases (disk full, corrupt files, etc.).
- Optional: system tray icon with "Pause" / "Exit" context menu (stretch goal).

---

## 10. Dependencies Summary

| Dependency        | License     | Size                                    | Purpose                                        |
| ----------------- | ----------- | --------------------------------------- | ---------------------------------------------- |
| **Windows SDK**   | —           | System                                  | WIC, COM, `ReadDirectoryChangesW`, Shell APIs  |
| **libultrahdr**   | Apache 2.0  | ~200 KB (static)                        | Gain map computation + Ultra HDR JPEG encoding |
| **libjpeg-turbo** | BSD         | ~300 KB (static, pulled by libultrahdr) | JPEG compression for SDR base + gain map       |
| **WRL (ComPtr)**  | Windows SDK | Header-only                             | RAII for COM pointers                          |

Total additional binary size: **~500 KB** on top of the ~50 KB of our own code.

---

## 11. Open Questions / Decisions Needed

1. **JPEG quality for the SDR base layer**: 90? 95? Higher = larger file but better SDR fallback.
2. **Gain map resolution**: libultrahdr defaults to ¼ resolution. Acceptable for screenshots?
3. **System tray icon**: Do we want a tray icon for manual pause/exit, or truly invisible?
4. **Log rotation**: How many log files / how much log history to keep?
5. **What if the user wants to keep the original JXR?** Add a config option to copy instead of replace?
