#pragma once
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <shlobj.h>
#include <string>
#include <windows.h>


namespace jxr {

// ============================================================================
// RAII wrapper for Win32 HANDLE
// ============================================================================
struct HandleDeleter {
  void operator()(HANDLE h) const noexcept {
    if (h && h != INVALID_HANDLE_VALUE) {
      ::CloseHandle(h);
    }
  }
};
using UniqueHandle =
    std::unique_ptr<std::remove_pointer_t<HANDLE>, HandleDeleter>;

inline UniqueHandle MakeUniqueHandle(HANDLE h) {
  return UniqueHandle((h == INVALID_HANDLE_VALUE) ? nullptr : h);
}

// ============================================================================
// Scoped COM initializer (one per thread)
// ============================================================================
struct ComInit {
  HRESULT hr;
  ComInit() : hr(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
  ~ComInit() {
    if (SUCCEEDED(hr))
      ::CoUninitialize();
  }
  ComInit(const ComInit &) = delete;
  ComInit &operator=(const ComInit &) = delete;
  explicit operator bool() const { return SUCCEEDED(hr); }
};

// ============================================================================
// Simple file logger
// ============================================================================
inline std::wstring GetLogPath() {
  wchar_t *appData = nullptr;
  if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr,
                                       &appData))) {
    std::wstring path(appData);
    ::CoTaskMemFree(appData);
    path += L"\\JxrAutoCleaner";
    std::filesystem::create_directories(path);
    return path + L"\\log.txt";
  }
  return L"JxrAutoCleaner.log";
}

inline void LogMsg(const wchar_t *fmt, ...) {
  static std::wstring logPath = GetLogPath();
  FILE *f = nullptr;
  _wfopen_s(&f, logPath.c_str(), L"a");
  if (!f)
    return;

  // Timestamp
  SYSTEMTIME st;
  ::GetLocalTime(&st);
  fwprintf(f, L"[%04d-%02d-%02d %02d:%02d:%02d] ", st.wYear, st.wMonth, st.wDay,
           st.wHour, st.wMinute, st.wSecond);

  va_list args;
  va_start(args, fmt);
  vfwprintf(f, fmt, args);
  va_end(args);

  fwprintf(f, L"\n");
  fclose(f);
}

// ============================================================================
// Log rotation: keep only the last N lines
// ============================================================================
inline void TrimLog(size_t maxLines = 500) {
  std::wstring logPath = GetLogPath();
  FILE *f = nullptr;
  _wfopen_s(&f, logPath.c_str(), L"r");
  if (!f)
    return;

  std::vector<std::wstring> lines;
  wchar_t buf[1024];
  while (fgetws(buf, _countof(buf), f)) {
    lines.emplace_back(buf);
  }
  fclose(f);

  if (lines.size() <= maxLines)
    return;

  // Rewrite with only the tail
  _wfopen_s(&f, logPath.c_str(), L"w");
  if (!f)
    return;
  for (size_t i = lines.size() - maxLines; i < lines.size(); ++i) {
    fputws(lines[i].c_str(), f);
  }
  fclose(f);
}

// ============================================================================
// Get the user's Videos folder path
// ============================================================================
inline std::wstring GetVideosFolder() {
  wchar_t *path = nullptr;
  if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_Videos, 0, nullptr, &path))) {
    std::wstring result(path);
    ::CoTaskMemFree(path);
    return result;
  }
  return L"";
}

} // namespace jxr
