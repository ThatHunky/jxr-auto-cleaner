# JxrAutoCleaner v1.1.2

**Patch release: fixes a data-loss bug, plus shutdown, tray and logging fixes**

## 🐛 Bug Fixes

### Screenshots can no longer be lost when a conversion fails halfway

The converter used to delete the original `.jxr` **before** renaming the finished `.tmp.jpg` to `.jpg`. If the rename failed (for example, a viewer had the `.jpg` open), the only copy left was `X.tmp.jpg`, and the orphan cleanup deleted it on the next startup.

**Fixed by:**

- Renaming the output into place first, and deleting the original only after that succeeds
- Checking the JPEG write for errors (such as a full disk), so a truncated file is never promoted over the original
- Changing the startup cleanup to recover a lone `X.tmp.jpg` (left by older versions) as `X.jpg` instead of deleting it. It is deleted only when the `.jxr` still exists.
- Running the startup cleanup before the worker starts, so it can't delete a temp file that is still being written

### Exit and shutdown

- Closing the window (`WM_CLOSE`) now really ends the process; before, the threads stopped but the process stayed alive
- Logoff and shutdown (`WM_ENDSESSION`) now actually reach the app; the message-only window never received them. A cancelled logoff leaves it running.
- The watcher waits for its cancelled read before freeing the buffer, which fixes possible heap corruption on exit

### Tray icon survives Explorer restarts

The app used a message-only window, which never receives broadcasts, so the icon disappeared for good after an Explorer crash or restart and the app could only be stopped from Task Manager. It now uses a hidden top-level window and re-adds the icon on `TaskbarCreated`.

### Other fixes

- Several filesystem calls that could throw on a worker thread (which crashes the whole app) now report errors instead
- Scans skip folders they can't read instead of stopping at the first one
- Watcher buffer overflows reported as `ERROR_NOTIFY_ENUM_DIR` now trigger a rescan instead of losing the events
- HDR float-to-half conversion now rounds to nearest instead of truncating, and NaN pixels are clamped to 0 instead of becoming infinity
- The alpha channel is no longer scaled along with luminance
- The single-instance lock is now per user session, so two signed-in users can each run the app

## ✨ Improvements

- The log is written as UTF-8 and is thread-safe, so non-ASCII paths (such as a Cyrillic user name) are logged correctly
- "System busy" is logged once per busy period instead of every 30 seconds
- More JPEG XR HDR pixel formats (fixed-point, RGBE) are recognised. If WIC can't convert one, the app falls back to an SDR transcode.
- `--convert` output now appears in the terminal that launched it
- The startup registry entry is quoted, so paths with spaces are handled safely

## 📦 Upgrade Instructions

**Existing users**: run `JxrAutoCleaner-v1.1.2.msi`. It upgrades the existing install in place.

---

**Full Changelog**: [v1.1.1...v1.1.2](https://github.com/ThatHunky/jxr-auto-cleaner/compare/v1.1.1...v1.1.2)
