#include "SystemCheck.h"
#include "Utils.h"
#include <shellapi.h>
#include <shobjidl.h>
#include <windows.h>

namespace jxr {

// ============================================================================
// Gaming / Fullscreen Detection
// ============================================================================
bool IsGaming() {
  QUERY_USER_NOTIFICATION_STATE state = QUNS_ACCEPTS_NOTIFICATIONS;
  HRESULT hr = ::SHQueryUserNotificationState(&state);
  if (FAILED(hr))
    return false;

  return (state == QUNS_RUNNING_D3D_FULL_SCREEN ||
          state == QUNS_PRESENTATION_MODE);
}

// ============================================================================
// CPU Usage Sampling
// ============================================================================
static ULONGLONG FileTimeToU64(const FILETIME &ft) {
  ULARGE_INTEGER li;
  li.LowPart = ft.dwLowDateTime;
  li.HighPart = ft.dwHighDateTime;
  return li.QuadPart;
}

double GetCpuUsagePercent() {
  static FILETIME s_prevIdle = {0}, s_prevKernel = {0}, s_prevUser = {0};
  static ULONGLONG s_prevTimeMs = 0;
  static double s_lastCpu = 0.0;

  ULONGLONG currentMs = ::GetTickCount64();
  if (currentMs - s_prevTimeMs < 1000 && s_prevTimeMs != 0) {
    return s_lastCpu;
  }

  FILETIME idle, kernel, user;
  if (!::GetSystemTimes(&idle, &kernel, &user))
    return 0.0;

  if (s_prevTimeMs == 0) {
    s_prevIdle = idle;
    s_prevKernel = kernel;
    s_prevUser = user;
    s_prevTimeMs = currentMs;
    return 0.0;
  }

  ULONGLONG idleDiff = FileTimeToU64(idle) - FileTimeToU64(s_prevIdle);
  ULONGLONG kernelDiff = FileTimeToU64(kernel) - FileTimeToU64(s_prevKernel);
  ULONGLONG userDiff = FileTimeToU64(user) - FileTimeToU64(s_prevUser);
  ULONGLONG totalDiff = kernelDiff + userDiff;

  s_prevIdle = idle;
  s_prevKernel = kernel;
  s_prevUser = user;
  s_prevTimeMs = currentMs;

  if (totalDiff == 0) {
    s_lastCpu = 0.0;
  } else {
    s_lastCpu = (1.0 - static_cast<double>(idleDiff) / static_cast<double>(totalDiff)) * 100.0;
  }

  return s_lastCpu;
}

// ============================================================================
// Combined Check
// ============================================================================
bool IsSystemBusy(double cpuThreshold) {
  if (IsGaming()) {
    return true;
  }
  double cpu = GetCpuUsagePercent();
  return cpu > cpuThreshold;
}

} // namespace jxr
