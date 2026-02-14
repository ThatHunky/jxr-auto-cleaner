#include "FileWatcher.h"
#include "Utils.h"
#include <algorithm>
#include <cctype>
#include <filesystem>

namespace fs = std::filesystem;

namespace jxr {

// ============================================================================
// Case-insensitive extension check
// ============================================================================
static bool HasJxrExtension(const std::wstring &filename) {
  if (filename.size() < 4)
    return false;
  std::wstring ext = filename.substr(filename.size() - 4);
  // Convert to lowercase
  std::transform(ext.begin(), ext.end(), ext.begin(),
                 [](wchar_t c) { return static_cast<wchar_t>(::towlower(c)); });
  return ext == L".jxr";
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
                                           FILE_NOTIFY_CHANGE_FILE_NAME |
                                               FILE_NOTIFY_CHANGE_LAST_WRITE,
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
      ::CancelIoEx(hDir, &overlapped);
      LogMsg(L"FileWatcher: shutdown signaled, exiting");
      break;
    }

    if (waitResult == WAIT_OBJECT_0) {
      // Directory change occurred
      DWORD bytesReturned = 0;
      if (!::GetOverlappedResult(hDir, &overlapped, &bytesReturned, FALSE)) {
        LogMsg(L"FileWatcher: GetOverlappedResult failed, error %u",
               ::GetLastError());
        continue;
      }

      if (bytesReturned == 0) {
        // Buffer overflow — too many changes at once. Scan directory manually.
        LogMsg(
            L"FileWatcher: buffer overflow, scanning directory for .jxr files");
        try {
          for (const auto &entry : fs::recursive_directory_iterator(watchDir)) {
            if (entry.is_regular_file() &&
                HasJxrExtension(entry.path().wstring())) {
              // Skip if a .jpg already exists (already converted)
              fs::path jpgPath = entry.path();
              jpgPath.replace_extension(L".jpg");
              if (fs::exists(jpgPath))
                continue;
              queue.push(entry.path().wstring());
            }
          }
        } catch (const std::exception &e) {
          LogMsg(L"FileWatcher: scan error: %hs", e.what());
        }
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

          if (HasJxrExtension(filename)) {
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
