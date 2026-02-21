# JxrAutoCleaner v1.1.2

**Patch release — completely fixes system busy false positives and makes manual Force Runs instantaneous**

## 🐛 Bug Fixes

### Force Scan Now is truly instant ⚡

We've removed the 30-second blocking delay from the worker thread. Clicking "Force Scan Now" will now immediately wake up the background process and start converting your screenshots right away, without waiting for the internal timer to pop.

### "System Busy" False Positives eliminated 🛑

We fixed a massive bug where the application would forever think you were gaming or in full-screen mode if you had Windows "Focus Assist" or "Do Not Disturb" turned on. The service now correctly distinguishes between actual 3D full-screen gaming and regular DND mode.

### CPU Timing rewritten ⏱️

The background CPU check previously blocked the thread completely for 1 second for every single image it processed. This has been completely refactored with high-performance tick counters to ensure smooth and fast conversions when multiple images are queued.

## 📦 Upgrade Instructions

**Existing users**: Simply run `JxrAutoCleaner-v1.1.2.msi` — it will automatically upgrade in-place.

---

**Full Changelog**: [v1.1.1...v1.1.2](https://github.com/ThatHunky/jxr-auto-cleaner/compare/v1.1.1...v1.1.2)
