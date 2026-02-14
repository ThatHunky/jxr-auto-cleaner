#pragma once
#include "ThreadSafeQueue.h"
#include <string>
#include <windows.h>

namespace jxr {

/// Watches a directory recursively for new .jxr files.
/// Runs in its own thread; call Run() from the thread entry point.
class FileWatcher {
public:
  /// watchDir: directory to watch recursively
  /// queue: thread-safe queue to push discovered .jxr paths into
  /// shutdownEvent: when signaled, the watcher exits its loop
  void Run(const std::wstring &watchDir, ThreadSafeQueue<std::wstring> &queue,
           HANDLE shutdownEvent);
};

} // namespace jxr
