# JxrAutoCleaner v1.1.1

**Patch release — fixes tray Exit button and adds log rotation**

## � Bug Fixes

### Tray "Exit" now properly kills the process

The worker thread had blocking `Sleep()` calls and a condition variable wait that didn't respond to the shutdown signal. Clicking "Exit" would leave the process hanging for up to 30 seconds (or indefinitely if stuck in a retry loop).

**Fixed by:**

- Adding a `shutdown()` method to `ThreadSafeQueue` that wakes all blocked `wait_and_pop()` calls immediately
- Replacing all `Sleep()` calls in the worker thread with `WaitForSingleObject(shutdownEvent)` so they respond to exit instantly

## ✨ Improvements

### Log rotation (500-line cap)

`log.txt` previously grew unbounded. Now on each startup, if the log exceeds 500 lines, it's trimmed to the most recent 500.

## 📦 Upgrade Instructions

**Existing users**: Simply run `JxrAutoCleaner-v1.1.1.msi` — it will automatically upgrade in-place.

---

**Full Changelog**: [v1.1...v1.1.1](https://github.com/ThatHunky/jxr-auto-cleaner/compare/v1.1...v1.1.1)
