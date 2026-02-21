#include "Converter.h"
#include "FileWatcher.h"
#include "SystemCheck.h"
#include "ThreadSafeQueue.h"
#include "Utils.h"
#include "resource.h"

#include <atomic>
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
static std::atomic<bool> g_forceRunActive = false;
static HANDLE g_wakeEvent = nullptr;

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
  ::GetModuleFileNameW(nullptr, exePath, MAX_PATH);

  HKEY hKey = nullptr;
  if (::RegOpenKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, KEY_SET_VALUE,
                      &hKey) == ERROR_SUCCESS) {
    DWORD len = static_cast<DWORD>((wcslen(exePath) + 1) * sizeof(wchar_t));
    ::RegSetValueExW(hKey, kAppName, 0, REG_SZ,
                     reinterpret_cast<const BYTE *>(exePath), len);
    ::RegCloseKey(hKey);
    LogMsg(L"Added to startup: %s", exePath);
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
  g_forceRunActive = true;
  if (g_wakeEvent) {
    ::SetEvent(g_wakeEvent);
  }
  int count = 0;
  try {
    for (const auto &entry : fs::recursive_directory_iterator(g_videosDir)) {
      if (!entry.is_regular_file())
        continue;
      auto ext = entry.path().extension().wstring();
      std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(::towlower(c));
      });
      if (ext == L".jxr") {
        // Skip if already converted
        fs::path jpgPath = entry.path();
        jpgPath.replace_extension(L".jpg");
        if (fs::exists(jpgPath))
          continue;
        g_queue.push(entry.path().wstring());
        ++count;
      }
    }
  } catch (const std::exception &e) {
    LogMsg(L"Force scan error: %hs", e.what());
  }
  LogMsg(L"Force scan: queued %d files", count);
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

  while (::WaitForSingleObject(g_shutdownEvent, 0) != WAIT_OBJECT_0) {
    // Wait for a file to appear in the queue (30 second timeout)
    auto item = g_queue.wait_and_pop(std::chrono::seconds(30));
    if (!item.has_value()) {
      g_forceRunActive = false;
      continue;
    }

    // Check if system is busy
    if (!g_forceRunActive && IsSystemBusy()) {
      LogMsg(L"Worker: system busy, re-queuing %s", item->c_str());
      g_queue.push_front(std::move(*item));
      HANDLE events[2] = {g_shutdownEvent, g_wakeEvent};
      if (::WaitForMultipleObjects(2, events, FALSE, 30000) == WAIT_OBJECT_0)
        break;
      continue;
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
          break;
      } else if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
        LogMsg(L"Worker: file no longer exists: %s", filePath.c_str());
        break;
      } else {
        LogMsg(L"Worker: unexpected error %u opening: %s", err,
               filePath.c_str());
        break;
      }
    }

    if (!fileReady) {
      LogMsg(L"Worker: skipping file (not accessible): %s", filePath.c_str());
      continue;
    }

    // Check if file still exists
    if (!fs::exists(filePath)) {
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
  case WM_CLOSE:
    RemoveTrayIcon();
    if (g_shutdownEvent)
      ::SetEvent(g_shutdownEvent);
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
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR lpCmdLine, int) {
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
  TrimLog();
  LogMsg(L"=== JxrAutoCleaner starting ===");

  // Single-instance check
  HANDLE hMutex = ::CreateMutexW(nullptr, TRUE, L"Global\\JxrAutoCleanerMutex");
  if (::GetLastError() == ERROR_ALREADY_EXISTS) {
    LogMsg(L"Another instance is already running, exiting");
    if (hMutex)
      ::CloseHandle(hMutex);
    return 0;
  }

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
  g_wakeEvent = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);

  // Register hidden window class
  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = HiddenWndProc;
  wc.hInstance = hInstance;
  wc.lpszClassName = L"JxrAutoCleanerHidden";
  ::RegisterClassExW(&wc);

  HWND hwnd = ::CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0,
                                HWND_MESSAGE, nullptr, hInstance, nullptr);

  // Create tray icon
  CreateTrayIcon(hwnd);

  // Start threads
  std::thread watcherThread(WatcherThread, g_videosDir);
  std::thread workerThread(WorkerThread);

  // Clean up any orphan temp files from previous crashes
  try {
    for (const auto &entry : fs::recursive_directory_iterator(g_videosDir)) {
      if (entry.is_regular_file()) {
        // Check if the stem ends with ".tmp" (e.g. "photo.tmp.jpg")
        auto stem = entry.path().stem().wstring();
        auto ext = entry.path().extension().wstring();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c) {
          return static_cast<wchar_t>(::towlower(c));
        });
        if (ext == L".jpg" && stem.size() >= 4 &&
            stem.substr(stem.size() - 4) == L".tmp") {
          LogMsg(L"Cleaning up orphan temp file: %s",
                 entry.path().wstring().c_str());
          std::error_code ec;
          fs::remove(entry.path(), ec);
        }
      }
    }
  } catch (...) {
  }

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
  if (g_wakeEvent)
    ::CloseHandle(g_wakeEvent);
  if (hwnd)
    ::DestroyWindow(hwnd);
  if (hMutex)
    ::CloseHandle(hMutex);

  LogMsg(L"=== JxrAutoCleaner stopped ===");
  return 0;
}
