#pragma once
#include <cstdarg>
#include <cstdio>
#include <cwchar>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
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
// Simple file logger (UTF-8, safe to call from any thread)
// ============================================================================
inline std::wstring GetLogPath() {
  wchar_t *appData = nullptr;
  if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr,
                                       &appData))) {
    std::wstring path(appData);
    ::CoTaskMemFree(appData);
    path += L"\\JxrAutoCleaner";
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    return path + L"\\log.txt";
  }
  return L"JxrAutoCleaner.log";
}

// Resolved once and shared by LogMsg and TrimLog.
inline const std::wstring &LogPath() {
  static const std::wstring path = GetLogPath();
  return path;
}

// Serializes log writes across threads (and against TrimLog).
inline std::mutex &LogMutex() {
  static std::mutex m;
  return m;
}

inline std::string WideToUtf8(const std::wstring &w) {
  if (w.empty())
    return {};
  int len = ::WideCharToMultiByte(CP_UTF8, 0, w.data(),
                                  static_cast<int>(w.size()), nullptr, 0,
                                  nullptr, nullptr);
  std::string out(len > 0 ? len : 0, '\0');
  if (len > 0)
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                          out.data(), len, nullptr, nullptr);
  return out;
}

inline void LogMsg(const wchar_t *fmt, ...) {
  // Format in memory and write UTF-8 bytes: the CRT's wide text mode would
  // mangle non-ASCII paths (e.g. a Cyrillic user name) under the "C" locale.
  SYSTEMTIME st;
  ::GetLocalTime(&st);
  wchar_t stamp[32];
  swprintf(stamp, _countof(stamp), L"[%04d-%02d-%02d %02d:%02d:%02d] ",
           st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

  std::wstring line(stamp);
  va_list args;
  va_start(args, fmt);
  int len = _vscwprintf(fmt, args);
  va_end(args);
  if (len > 0) {
    std::wstring msg(static_cast<size_t>(len) + 1, L'\0');
    va_start(args, fmt);
    vswprintf(msg.data(), msg.size(), fmt, args);
    va_end(args);
    msg.resize(static_cast<size_t>(len));
    line += msg;
  }
  line += L'\n';
  const std::string utf8 = WideToUtf8(line);

  std::lock_guard<std::mutex> lock(LogMutex());
  FILE *f = nullptr;
  _wfopen_s(&f, LogPath().c_str(), L"ab");
  if (!f)
    return;
  fwrite(utf8.data(), 1, utf8.size(), f);
  fclose(f);
}

// ============================================================================
// Log rotation: keep only the last N lines
// ============================================================================
inline void TrimLog(size_t maxLines = 500) {
  std::lock_guard<std::mutex> lock(LogMutex());
  const std::wstring &logPath = LogPath();
  FILE *f = nullptr;
  _wfopen_s(&f, logPath.c_str(), L"rb");
  if (!f)
    return;

  // Keep only the newest maxLines lines in memory. fgets may return a long
  // line in several chunks, so chunks are joined until the newline is seen.
  std::deque<std::string> lines;
  size_t totalLines = 0;
  std::string current;
  char buf[1024];
  auto commit = [&] {
    ++totalLines;
    lines.push_back(std::move(current));
    current.clear();
    if (lines.size() > maxLines)
      lines.pop_front();
  };
  while (fgets(buf, sizeof(buf), f)) {
    current += buf;
    if (!current.empty() && current.back() == '\n')
      commit();
  }
  if (!current.empty())
    commit();
  fclose(f);

  if (totalLines <= maxLines)
    return;

  // Write the tail to a temp file, then atomically replace the log so a crash
  // mid-rewrite cannot leave it truncated.
  const std::wstring tmpPath = logPath + L".tmp";
  _wfopen_s(&f, tmpPath.c_str(), L"wb");
  if (!f)
    return;
  bool ok = true;
  for (const auto &line : lines) {
    if (fwrite(line.data(), 1, line.size(), f) != line.size()) {
      ok = false;
      break;
    }
  }
  if (fclose(f) != 0)
    ok = false;
  if (!ok || !::MoveFileExW(tmpPath.c_str(), logPath.c_str(),
                            MOVEFILE_REPLACE_EXISTING)) {
    ::DeleteFileW(tmpPath.c_str());
  }
}

// ============================================================================
// Case-insensitive extension check, e.g. HasExtension(p, L".jxr")
// ============================================================================
inline bool HasExtension(const std::filesystem::path &p, const wchar_t *ext) {
  return ::_wcsicmp(p.extension().c_str(), ext) == 0;
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
