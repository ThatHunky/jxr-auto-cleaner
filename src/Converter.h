#pragma once
#include <string>

namespace jxr {

/// Convert a JXR file to an Ultra HDR JPEG (gain map JPEG).
/// The output file is written next to the input with .jpg extension.
/// Returns true on success, false on failure (error is logged).
/// If the JXR is SDR (8-bit), a simple JPEG transcode is performed.
bool ConvertJxrToUltraHdrJpeg(const std::wstring &jxrPath,
                              int jpegQuality = 95);

} // namespace jxr
