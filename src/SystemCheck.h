#pragma once

namespace jxr {

/// Returns true if a fullscreen game, D3D exclusive app, or presentation is
/// active.
bool IsGaming();

/// Samples CPU usage over ~1 second. Returns percentage [0..100].
double GetCpuUsagePercent();

/// Returns true if the system is considered busy (gaming + CPU > threshold).
bool IsSystemBusy(double cpuThreshold = 25.0);

} // namespace jxr
