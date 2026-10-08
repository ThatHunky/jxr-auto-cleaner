#include "Converter.h"
#include "FileWatcher.h"
#include "SystemCheck.h"
#include "ThreadSafeQueue.h"
#include "Utils.h"
#include "resource.h"

#include <chrono>
#include <filesystem>
#include <shellapi.h>
#include <string>
#include <thread>
#include <windows.h>

namespace fs = std::filesystem;
using namespace jxr;

// ============================================================================
// Globals
// ============================================================================
static HANDLE g_shutdownEvent = nullptr;
static ThreadSafeQueue<std::wstring> g_queue;
static NOTIFYICONDATAW g_nid = {};
static std::wstring g_videosDir;
static HINSTANCE g_hInstance = nullptr;

// ============================================================================
// Registry helpers for startup toggle
// ============================================================================
static const wchar_t *kRunKeyPath =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t *kAppName = L"JxrAutoCleaner";

static bool IsInStartup() {
  HKEY hKey = nullptr;
  if (::RegOpenKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, KEY_READ, &hKey) !=
      ERROR_SUCCESS)
    return false;

  DWORD type = 0;
  DWORD size = 0;
  LONG result =
      ::RegQueryValueExW(hKey, kAppName, nullptr, &type, nullptr, &size);
  ::RegCloseKey(hKey);
  return (result == ERROR_SUCCESS);
}

static void AddToStartup() {
  wchar_t exePath[MAX_PATH];
  if (!::GetModuleFileNameW(nullptr, exePath, MAX_PATH))
    return;
  // Quoted, so a space in the profile path can't be split by CreateProcess
  const std::wstring command = L"\"" + std::wstring(exePath) + L"\"";

  HKEY hKey = nullptr;
  if (::RegOpenKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, KEY_SET_VALUE,
                      &hKey) == ERROR_SUCCESS) {
    DWORD len = static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t));
    ::RegSetValueExW(hKey, kAppName, 0, REG_SZ,
                     reinterpret_cast<const BYTE *>(command.c_str()), len);
    ::RegCloseKey(hKey);
    LogMsg(L"Added to startup: %s", command.c_str());
  }
}

static void RemoveFromStartup() {
  HKEY hKey = nullptr;
  if (::RegOpenKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, KEY_SET_VALUE,
                      &hKey) == ERROR_SUCCESS) {
    ::RegDeleteValueW(hKey, kAppName);
    ::RegCloseKey(hKey);
    LogMsg(L"Removed from startup");
  }
}

// ============================================================================
// Force scan: queue all existing JXR files in the watched folder
// ============================================================================
static void ForceScanNow() {
  LogMsg(L"Force scan requested");
  size_t count = QueueUnconvertedJxrFiles(g_videosDir, g_queue);
  LogMsg(L"Force scan: queued %zu files", count);
}

// ============================================================================
// Startup: deal with "<name>.tmp.jpg" files left by a crash mid-conversion
// ============================================================================
static void CleanUpOrphanTempFiles(const std::wstring &dir) {
  std::error_code ec;
  fs::recursive_directory_iterator it(
      dir, fs::directory_options::skip_permission_denied, ec);
  for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
    const fs::path &tmp = it->path();
    std::error_code fileEc;
    if (!it->is_regular_file(fileEc) || !HasExtension(tmp, L".jpg") ||
        !HasExtension(tmp.stem(), L".tmp"))
      continue;

    fs::path jxr = tmp.parent_path() / tmp.stem();
    jxr.replace_extension(L".jxr");
    fs::path jpg = jxr;
    jpg.replace_extension(L".jpg");

    if (fs::exists(jxr, fileEc)) {
      // The source survived, so the temp file is a partial write: drop it.
      LogMsg(L"Removing partial temp file: %s", tmp.wstring().c_str());
      fs::remove(tmp, fileEc);
    } else if (!fs::exists(jpg, fileEc)) {
      // Versions before 1.1.2 deleted the JXR before renaming the finished
      // temp file, so this may be the only copy left. Recover it.
      LogMsg(L"Recovering orphan temp file: %s", tmp.wstring().c_str());
      fs::rename(tmp, jpg, fileEc);
    }
  }
}

// ============================================================================
// Tray icon management
// ============================================================================
static void CreateTrayIcon(HWND hwnd) {
  ::ZeroMemory(&g_nid, sizeof(g_nid));
  g_nid.cbSize = sizeof(NOTIFYICONDATAW);
  g_nid.hWnd = hwnd;
  g_nid.uID = 1;
  g_nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_SHOWTIP;
  g_nid.uCallbackMessage = WM_TRAYICON;
  g_nid.hIcon = ::LoadIconW(g_hInstance, MAKEINTRESOURCEW(IDI_ICON1));
  wcscpy_s(g_nid.szTip, L"JxrAutoCleaner v1.1.2");

  ::Shell_NotifyIconW(NIM_ADD, &g_nid);

  // Set version for modern behavior
  g_nid.uVersion = NOTIFYICON_VERSION_4;
  ::Shell_NotifyIconW(NIM_SETVERSION, &g_nid);
}

static void RemoveTrayIcon() { ::Shell_NotifyIconW(NIM_DELETE, &g_nid); }

static void ShowTrayMenu(HWND hwnd) {
  HMENU hMenu = ::CreatePopupMenu();
  if (!hMenu)
    return;

  ::AppendMenuW(hMenu, MF_STRING, ID_TRAY_FORCE_RUN, L"Force Run Now");
  ::AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);

  // Dynamic label for startup toggle
  if (IsInStartup()) {
    ::AppendMenuW(hMenu, MF_STRING, ID_TRAY_TOGGLE_STARTUP,
                  L"Remove from Startup");
  } else {
    ::AppendMenuW(hMenu, MF_STRING, ID_TRAY_TOGGLE_STARTUP, L"Add to Startup");
  }

  ::AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
  ::AppendMenuW(hMenu, MF_STRING, ID_TRAY_EXIT, L"Exit");

  // Required for TrackPopupMenu to work correctly from a tray icon
  ::SetForegroundWindow(hwnd);

  POINT pt;
  ::GetCursorPos(&pt);
  ::TrackPopupMenu(hMenu, TPM_RIGHTALIGN | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hwnd,
                   nullptr);

  ::DestroyMenu(hMenu);
}

// ============================================================================
// Worker Thread: processes queued JXR files when the system is idle
// ============================================================================
static void WorkerThread() {
  ComInit com;
  if (!com) {
    LogMsg(L"Worker: COM init failed");
    return;
  }

  LogMsg(L"Worker: started");
  constexpr int MAX_RETRIES = 5;
  bool wasBusy = false;

  while (::WaitForSingleObject(g_shutdownEvent, 0) != WAIT_OBJECT_0) {
    // Wait for a file to appear in the queue (30 second timeout)
    auto item = g_queue.wait_and_pop(std::chrono::seconds(30));
    if (!item.has_value())
      continue;

    // Check if system is busy
    if (IsSystemBusy()) {
      // Log only the transition; this re-checks every ~30s while busy.
      if (!wasBusy)
        LogMsg(L"Worker: system busy, deferring %zu file(s)",
               g_queue.size() + 1);
      wasBusy = true;
      g_queue.push_front(std::move(*item));
      if (::WaitForSingleObject(g_shutdownEvent, 30000) == WAIT_OBJECT_0)
        break;
      continue;
    }
    if (wasBusy) {
      LogMsg(L"Worker: system idle, resuming");
      wasBusy = false;
    }

    std::wstring filePath = std::move(*item);

    // Check if file is ready (not locked by ShadowPlay)
    bool fileReady = false;
    for (int retry = 0; retry < MAX_RETRIES; ++retry) {
      HANDLE hFile =
          ::CreateFileW(filePath.c_str(), GENERIC_READ,
                        0, // No sharing — exclusive access test
                        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

      if (hFile != INVALID_HANDLE_VALUE) {
        ::CloseHandle(hFile);
        fileReady = true;
        break;
      }

      DWORD err = ::GetLastError();
      if (err == ERROR_SHARING_VIOLATION) {
        LogMsg(L"Worker: file locked (attempt %d/%d): %s", retry + 1,
               MAX_RETRIES, filePath.c_str());
        if (::WaitForSingleObject(g_shutdownEvent, 2000) == WAIT_OBJECT_0)
          break; // fileReady stays false; shutdown check below exits quietly
      } else if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
        LogMsg(L"Worker: file no longer exists: %s", filePath.c_str());
        break;
      } else {
        LogMsg(L"Worker: unexpected error %u opening: %s", err,
               filePath.c_str());
        break;
      }
    }

    if (::WaitForSingleObject(g_shutdownEvent, 0) == WAIT_OBJECT_0)
      break;

    if (!fileReady) {
      LogMsg(L"Worker: skipping file (not accessible): %s", filePath.c_str());
      continue;
    }

    // Check if file still exists
    std::error_code ec;
    if (!fs::exists(filePath, ec)) {
      LogMsg(L"Worker: file disappeared before conversion: %s",
             filePath.c_str());
      continue;
    }

    // Convert
    bool success = ConvertJxrToUltraHdrJpeg(filePath);
    if (!success) {
      LogMsg(L"Worker: conversion failed for %s", filePath.c_str());
    }
  }

  LogMsg(L"Worker: exited");
}

// ============================================================================
// Watcher Thread
// ============================================================================
static void WatcherThread(const std::wstring &videosDir) {
  FileWatcher watcher;
  watcher.Run(videosDir, g_queue, g_shutdownEvent);
}

// ============================================================================
// Window proc for tray icon and shutdown
// ============================================================================
static LRESULT CALLBACK HiddenWndProc(HWND hwnd, UINT msg, WPARAM wp,
                                      LPARAM lp) {
  // Broadcast by Explorer when the taskbar is (re)created, e.g. after a crash.
  static const UINT s_taskbarCreated =
      ::RegisterWindowMessageW(L"TaskbarCreated");
  if (msg == s_taskbarCreated && msg != 0) {
    CreateTrayIcon(hwnd);
    return 0;
  }

  switch (msg) {
  case WM_TRAYICON:
    // NOTIFYICON_VERSION_4: LOWORD(lp) = event, HIWORD(lp) = icon id
    switch (LOWORD(lp)) {
    case WM_RBUTTONUP:
    case WM_CONTEXTMENU:
      ShowTrayMenu(hwnd);
      return 0;
    }
    return 0;

  case WM_COMMAND:
    switch (LOWORD(wp)) {
    case ID_TRAY_FORCE_RUN:
      ForceScanNow();
      return 0;
    case ID_TRAY_TOGGLE_STARTUP:
      if (IsInStartup())
        RemoveFromStartup();
      else
        AddToStartup();
      return 0;
    case ID_TRAY_EXIT:
      RemoveTrayIcon();
      if (g_shutdownEvent)
        ::SetEvent(g_shutdownEvent);
      ::PostQuitMessage(0);
      return 0;
    }
    break;

  case WM_ENDSESSION:
    // wParam == FALSE means the session end was cancelled; keep running.
    if (!wp)
      return 0;
    [[fallthrough]];
  case WM_CLOSE:
    RemoveTrayIcon();
    if (g_shutdownEvent)
      ::SetEvent(g_shutdownEvent);
    // Leave the message loop so wWinMain runs the shutdown sequence.
    ::PostQuitMessage(0);
    return 0;

  case WM_DESTROY:
    ::PostQuitMessage(0);
    return 0;
  }
  return ::DefWindowProcW(hwnd, msg, wp, lp);
}

// ============================================================================
// CLI mode: --convert <file>
// ============================================================================
static int RunCliConvert(const std::wstring &filePath) {
  // This is a WIN32-subsystem exe, so stdout/stderr go nowhere unless we
  // attach to the console of the shell that launched us.
  if (::AttachConsole(ATTACH_PARENT_PROCESS)) {
    FILE *ignored = nullptr;
    freopen_s(&ignored, "CONOUT$", "w", stdout);
    freopen_s(&ignored, "CONOUT$", "w", stderr);
  }

  ComInit com;
  if (!com) {
    fwprintf(stderr, L"COM initialization failed\n");
    return 1;
  }

  fwprintf(stdout, L"Converting: %s\n", filePath.c_str());
  bool ok = ConvertJxrToUltraHdrJpeg(filePath);
  if (ok) {
    fwprintf(stdout, L"Success!\n");
    return 0;
  } else {
    fwprintf(stderr, L"Conversion failed. Check log at "
                     L"%%LOCALAPPDATA%%\\JxrAutoCleaner\\log.txt\n");
    return 1;
  }
}

// ============================================================================
// Entry point
// ============================================================================
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int) {
  g_hInstance = hInstance;

  // Parse command line for --convert mode
  int argc = 0;
  LPWSTR *argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
  if (argv) {
    for (int i = 1; i < argc; ++i) {
      if ((wcscmp(argv[i], L"--convert") == 0 || wcscmp(argv[i], L"-c") == 0) &&
          i + 1 < argc) {
        int result = RunCliConvert(argv[i + 1]);
        ::LocalFree(argv);
        return result;
      }
    }
    ::LocalFree(argv);
  }

  // --- Background service mode ---
  // Single-instance check
  // Per-session (Local) mutex: a Global one would stop a second signed-in
  // user from running their own instance for their own Videos folder.
  HANDLE hMutex = ::CreateMutexW(nullptr, TRUE, L"Local\\JxrAutoCleanerMutex");
  if (::GetLastError() == ERROR_ALREADY_EXISTS) {
    LogMsg(L"Another instance is already running, exiting");
    if (hMutex)
      ::CloseHandle(hMutex);
    return 0;
  }

  // Trim only once we own the mutex, so we never rewrite the log while
  // another running instance is appending to it.
  TrimLog();
  LogMsg(L"=== JxrAutoCleaner starting ===");

  // Resolve Videos folder
  g_videosDir = GetVideosFolder();
  if (g_videosDir.empty()) {
    LogMsg(L"Failed to resolve Videos folder, exiting");
    if (hMutex)
      ::CloseHandle(hMutex);
    return 1;
  }
  LogMsg(L"Monitoring: %s", g_videosDir.c_str());

  // Create shutdown event
  g_shutdownEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!g_shutdownEvent) {
    LogMsg(L"Failed to create shutdown event");
    if (hMutex)
      ::CloseHandle(hMutex);
    return 1;
  }

  // Register hidden window class
  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = HiddenWndProc;
  wc.hInstance = hInstance;
  wc.lpszClassName = L"JxrAutoCleanerHidden";
  ::RegisterClassExW(&wc);

  // A hidden top-level window rather than HWND_MESSAGE: message-only windows
  // never receive broadcasts like WM_ENDSESSION or TaskbarCreated.
  HWND hwnd = ::CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName,
                                L"JxrAutoCleaner", WS_POPUP, 0, 0, 0, 0,
                                nullptr, nullptr, hInstance, nullptr);
  if (!hwnd) {
    LogMsg(L"Failed to create window, error %u", ::GetLastError());
    ::CloseHandle(g_shutdownEvent);
    if (hMutex)
      ::CloseHandle(hMutex);
    return 1;
  }

  // Create tray icon
  CreateTrayIcon(hwnd);

  // Must finish before the worker starts, or it could delete a temp file the
  // worker is writing.
  CleanUpOrphanTempFiles(g_videosDir);

  // Start threads
  std::thread watcherThread(WatcherThread, g_videosDir);
  std::thread workerThread(WorkerThread);

  // Message pump (keeps the process alive, handles tray messages)
  MSG msg;
  while (::GetMessageW(&msg, nullptr, 0, 0)) {
    ::TranslateMessage(&msg);
    ::DispatchMessageW(&msg);
  }

  // Shutdown sequence
  LogMsg(L"Shutting down...");
  ::SetEvent(g_shutdownEvent);
  g_queue.shutdown();

  if (watcherThread.joinable())
    watcherThread.join();
  if (workerThread.joinable())
    workerThread.join();

  RemoveTrayIcon();

  ::CloseHandle(g_shutdownEvent);
  ::DestroyWindow(hwnd);
  if (hMutex)
    ::CloseHandle(hMutex);

  LogMsg(L"=== JxrAutoCleaner stopped ===");
  return 0;
}
