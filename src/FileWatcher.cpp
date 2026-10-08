#include "FileWatcher.h"
#include "Utils.h"
#include <cstdint>
#include <filesystem>
#include <vector>

namespace fs = std::filesystem;

namespace jxr {

// ============================================================================
// Full scan (used for buffer overflow and "Force Run Now")
// ============================================================================
size_t QueueUnconvertedJxrFiles(const std::wstring &dir,
                                ThreadSafeQueue<std::wstring> &queue) {
  size_t count = 0;
  std::error_code ec;
  fs::recursive_directory_iterator it(
      dir, fs::directory_options::skip_permission_denied, ec);
  for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
    const fs::directory_entry &entry = *it;
    std::error_code fileEc;
    if (!entry.is_regular_file(fileEc) || !HasExtension(entry.path(), L".jxr"))
      continue;
    // Skip if a .jpg already exists (already converted)
    fs::path jpgPath = entry.path();
    jpgPath.replace_extension(L".jpg");
    if (fs::exists(jpgPath, fileEc))
      continue;
    queue.push(entry.path().wstring());
    ++count;
  }
  if (ec)
    LogMsg(L"Scan of '%s' stopped early: %hs", dir.c_str(),
           ec.message().c_str());
  return count;
}

// ============================================================================
// Main watcher loop
// ============================================================================
void FileWatcher::Run(const std::wstring &watchDir,
                      ThreadSafeQueue<std::wstring> &queue,
                      HANDLE shutdownEvent) {
  LogMsg(L"FileWatcher: watching '%s'", watchDir.c_str());

  // Open directory handle for monitoring
  HANDLE hDir =
      ::CreateFileW(watchDir.c_str(), FILE_LIST_DIRECTORY,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);

  if (hDir == INVALID_HANDLE_VALUE) {
    LogMsg(L"FileWatcher: failed to open directory, error %u",
           ::GetLastError());
    return;
  }

  // RAII handle cleanup
  UniqueHandle dirHandle(hDir);

  // Overlapped event for async ReadDirectoryChangesW
  HANDLE hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!hEvent) {
    LogMsg(L"FileWatcher: failed to create event");
    return;
  }
  UniqueHandle eventHandle(hEvent);

  // Buffer for directory change notifications
  constexpr DWORD BUF_SIZE = 64 * 1024; // 64 KB
  std::vector<uint8_t> buffer(BUF_SIZE);

  while (true) {
    OVERLAPPED overlapped = {};
    overlapped.hEvent = hEvent;
    ::ResetEvent(hEvent);

    BOOL success = ::ReadDirectoryChangesW(hDir, buffer.data(), BUF_SIZE,
                                           TRUE, // Watch subtree
                                           FILE_NOTIFY_CHANGE_FILE_NAME,
                                           nullptr, &overlapped, nullptr);

    if (!success) {
      DWORD err = ::GetLastError();
      if (err != ERROR_IO_PENDING) {
        LogMsg(L"FileWatcher: ReadDirectoryChangesW failed, error %u", err);
        break;
      }
    }

    // Wait for either directory change or shutdown
    HANDLE waitHandles[2] = {hEvent, shutdownEvent};
    DWORD waitResult =
        ::WaitForMultipleObjects(2, waitHandles, FALSE, INFINITE);

    if (waitResult == WAIT_OBJECT_0 + 1) {
      // Shutdown signaled
      // Wait for the cancelled read to complete before `overlapped` and
      // `buffer` go out of scope, or the kernel may write into freed memory.
      if (::CancelIoEx(hDir, &overlapped) ||
          ::GetLastError() != ERROR_NOT_FOUND) {
        DWORD ignored = 0;
        ::GetOverlappedResult(hDir, &overlapped, &ignored, TRUE);
      }
      LogMsg(L"FileWatcher: shutdown signaled, exiting");
      break;
    }

    if (waitResult == WAIT_OBJECT_0) {
      // Directory change occurred
      DWORD bytesReturned = 0;
      bool overflow = false;
      if (!::GetOverlappedResult(hDir, &overlapped, &bytesReturned, FALSE)) {
        DWORD err = ::GetLastError();
        if (err != ERROR_NOTIFY_ENUM_DIR) {
          LogMsg(L"FileWatcher: GetOverlappedResult failed, error %u", err);
          continue;
        }
        overflow = true;
      } else if (bytesReturned == 0) {
        overflow = true;
      }

      if (overflow) {
        // Too many changes at once; the events were dropped. Rescan instead.
        LogMsg(
            L"FileWatcher: buffer overflow, scanning directory for .jxr files");
        size_t queued = QueueUnconvertedJxrFiles(watchDir, queue);
        LogMsg(L"FileWatcher: overflow scan queued %zu files", queued);
        continue;
      }

      // Parse the notification buffer
      const uint8_t *ptr = buffer.data();
      while (true) {
        const FILE_NOTIFY_INFORMATION *info =
            reinterpret_cast<const FILE_NOTIFY_INFORMATION *>(ptr);

        if (info->Action == FILE_ACTION_ADDED ||
            info->Action == FILE_ACTION_RENAMED_NEW_NAME) {
          std::wstring filename(info->FileName,
                                info->FileNameLength / sizeof(wchar_t));

          if (HasExtension(fs::path(filename), L".jxr")) {
            // Build full path
            std::wstring fullPath = watchDir + L"\\" + filename;
            LogMsg(L"FileWatcher: detected JXR: %s", fullPath.c_str());
            queue.push(std::move(fullPath));
          }
        }

        if (info->NextEntryOffset == 0)
          break;
        ptr += info->NextEntryOffset;
      }
    } else {
      // Unexpected wait result
      LogMsg(L"FileWatcher: unexpected wait result %u", waitResult);
      break;
    }
  }

  LogMsg(L"FileWatcher: exited");
}

} // namespace jxr
