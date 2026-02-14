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

  return (state == QUNS_BUSY || state == QUNS_RUNNING_D3D_FULL_SCREEN ||
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
  FILETIME idleA, kernelA, userA;
  if (!::GetSystemTimes(&idleA, &kernelA, &userA))
    return 0.0;

  ::Sleep(1000); // 1-second sample window

  FILETIME idleB, kernelB, userB;
  if (!::GetSystemTimes(&idleB, &kernelB, &userB))
    return 0.0;

  ULONGLONG idle = FileTimeToU64(idleB) - FileTimeToU64(idleA);
  ULONGLONG kernel = FileTimeToU64(kernelB) - FileTimeToU64(kernelA);
  ULONGLONG user = FileTimeToU64(userB) - FileTimeToU64(userA);
  ULONGLONG total = kernel + user;

  if (total == 0)
    return 0.0;
  return (1.0 - static_cast<double>(idle) / static_cast<double>(total)) * 100.0;
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
