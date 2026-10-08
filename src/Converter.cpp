#include "Converter.h"
#include "Utils.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <wincodec.h>
#include <wrl/client.h>

// libultrahdr C API
#include <ultrahdr_api.h>

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

// ============================================================================
// IEEE 754 half-float ↔ float conversion helpers
// ============================================================================
static inline float HalfToFloat(uint16_t h) {
  uint32_t sign = (h & 0x8000u) << 16;
  uint32_t exponent = (h >> 10) & 0x1F;
  uint32_t mantissa = h & 0x03FF;

  if (exponent == 0) {
    if (mantissa == 0) {
      // ±0
      uint32_t bits = sign;
      float f;
      std::memcpy(&f, &bits, 4);
      return f;
    }
    // Subnormal: convert to normalized float
    while (!(mantissa & 0x0400)) {
      mantissa <<= 1;
      exponent--;
    }
    exponent++;
    mantissa &= ~0x0400u;
    exponent += (127 - 15);
    uint32_t bits = sign | (exponent << 23) | (mantissa << 13);
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
  } else if (exponent == 31) {
    // Inf / NaN
    uint32_t bits = sign | 0x7F800000u | (mantissa << 13);
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
  }

  exponent += (127 - 15);
  uint32_t bits = sign | (exponent << 23) | (mantissa << 13);
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

// Round-to-nearest-even float -> half (after F. Giesen's float_to_half_fast3).
// The old truncating version biased values downward and turned NaN into Inf.
static inline uint16_t FloatToHalf(float value) {
  constexpr uint32_t kF32Infinity = 255u << 23;
  constexpr uint32_t kF16Overflow = (127u + 16u) << 23; // first value >= 65520
  constexpr uint32_t kDenormMagic = ((127u - 15u) + (23u - 10u) + 1u) << 23;

  uint32_t bits;
  std::memcpy(&bits, &value, 4);
  const uint32_t sign = bits & 0x80000000u;
  bits ^= sign;

  uint16_t out;
  if (bits >= kF16Overflow) {
    out = (bits > kF32Infinity) ? 0x7E00 : 0x7C00; // NaN : Inf
  } else if (bits < (113u << 23)) {
    // Result is a half subnormal (or zero): let the FPU do the rounding.
    float f, magic;
    std::memcpy(&f, &bits, 4);
    std::memcpy(&magic, &kDenormMagic, 4);
    f += magic;
    uint32_t r;
    std::memcpy(&r, &f, 4);
    out = static_cast<uint16_t>(r - kDenormMagic);
  } else {
    const uint32_t mantissaOdd = (bits >> 13) & 1u;
    bits += ((15u - 127u) << 23) + 0xFFFu; // rebias exponent, round half up
    bits += mantissaOdd;                    // ...but ties go to even
    out = static_cast<uint16_t>(bits >> 13);
  }
  return static_cast<uint16_t>(out | (sign >> 16));
}

namespace jxr {

// ============================================================================
// Helper: Check if a WIC pixel format is HDR (high bit depth / float)
// ============================================================================
static bool IsHdrPixelFormat(const WICPixelFormatGUID &fmt) {
  return IsEqualGUID(fmt, GUID_WICPixelFormat64bppRGBAHalf) ||
         IsEqualGUID(fmt, GUID_WICPixelFormat128bppRGBAFloat) ||
         IsEqualGUID(fmt, GUID_WICPixelFormat128bppPRGBAFloat) ||
         IsEqualGUID(fmt, GUID_WICPixelFormat128bppRGBFloat) ||
         IsEqualGUID(fmt, GUID_WICPixelFormat48bppRGBHalf) ||
         IsEqualGUID(fmt, GUID_WICPixelFormat64bppRGBHalf) ||
         // JPEG XR fixed-point / shared-exponent variants are scRGB too
         IsEqualGUID(fmt, GUID_WICPixelFormat48bppRGBFixedPoint) ||
         IsEqualGUID(fmt, GUID_WICPixelFormat64bppRGBFixedPoint) ||
         IsEqualGUID(fmt, GUID_WICPixelFormat64bppRGBAFixedPoint) ||
         IsEqualGUID(fmt, GUID_WICPixelFormat128bppRGBFixedPoint) ||
         IsEqualGUID(fmt, GUID_WICPixelFormat128bppRGBAFixedPoint) ||
         IsEqualGUID(fmt, GUID_WICPixelFormat32bppRGBE);
}

// ============================================================================
// Helper: move the finished temp output into place, then delete the source.
// Renaming first means a failure at any step leaves the original JXR intact.
// ============================================================================
static bool FinalizeOutput(const fs::path &tempPath, const fs::path &finalPath,
                           const fs::path &inputPath) {
  std::error_code ec;
  fs::rename(tempPath, finalPath, ec);
  if (ec) {
    LogMsg(L"Failed to rename temp file to final: %hs", ec.message().c_str());
    fs::remove(tempPath, ec);
    return false;
  }

  fs::remove(inputPath, ec);
  if (ec) {
    LogMsg(L"Could not delete original JXR (locked?): %hs — keeping both "
           L"files",
           ec.message().c_str());
  }

  const uintmax_t size = fs::file_size(finalPath, ec);
  LogMsg(L"Conversion complete: %s (%.1f KB)", finalPath.wstring().c_str(),
         ec ? 0.0 : static_cast<double>(size) / 1024.0);
  return true;
}

// ============================================================================
// Helper: Simple SDR-only JPEG transcode via WIC (no libultrahdr needed)
// ============================================================================
static bool TranscodeSdrJxrToJpeg(ComPtr<IWICImagingFactory> &factory,
                                  ComPtr<IWICBitmapFrameDecode> &frame,
                                  const std::wstring &outputPath, int quality) {
  // Convert to 24bpp BGR for JPEG
  ComPtr<IWICFormatConverter> converter;
  HRESULT hr = factory->CreateFormatConverter(&converter);
  if (FAILED(hr)) {
    LogMsg(L"Failed to create format converter: 0x%08X", hr);
    return false;
  }

  hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat24bppBGR,
                             WICBitmapDitherTypeNone, nullptr, 0.0,
                             WICBitmapPaletteTypeCustom);
  if (FAILED(hr)) {
    LogMsg(L"Format conversion failed: 0x%08X", hr);
    return false;
  }

  // Create output stream
  ComPtr<IWICStream> stream;
  hr = factory->CreateStream(&stream);
  if (FAILED(hr))
    return false;

  hr = stream->InitializeFromFilename(outputPath.c_str(), GENERIC_WRITE);
  if (FAILED(hr)) {
    LogMsg(L"Failed to create output stream: 0x%08X", hr);
    return false;
  }

  // Create JPEG encoder
  ComPtr<IWICBitmapEncoder> encoder;
  hr = factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder);
  if (FAILED(hr))
    return false;

  hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
  if (FAILED(hr))
    return false;

  ComPtr<IWICBitmapFrameEncode> encFrame;
  ComPtr<IPropertyBag2> props;
  hr = encoder->CreateNewFrame(&encFrame, &props);
  if (FAILED(hr))
    return false;

  // Set JPEG quality
  PROPBAG2 option = {};
  option.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
  VARIANT varQuality;
  VariantInit(&varQuality);
  varQuality.vt = VT_R4;
  varQuality.fltVal = static_cast<float>(quality) / 100.0f;
  props->Write(1, &option, &varQuality);

  hr = encFrame->Initialize(props.Get());
  if (FAILED(hr))
    return false;

  UINT w, h;
  converter->GetSize(&w, &h);
  encFrame->SetSize(w, h);

  WICPixelFormatGUID outFmt = GUID_WICPixelFormat24bppBGR;
  encFrame->SetPixelFormat(&outFmt);

  hr = encFrame->WriteSource(converter.Get(), nullptr);
  if (FAILED(hr)) {
    LogMsg(L"WriteSource failed: 0x%08X", hr);
    return false;
  }

  hr = encFrame->Commit();
  if (FAILED(hr))
    return false;

  hr = encoder->Commit();
  if (FAILED(hr))
    return false;

  return true;
}

// ============================================================================
// Main conversion function
// ============================================================================
bool ConvertJxrToUltraHdrJpeg(const std::wstring &jxrPath, int jpegQuality) {
  LogMsg(L"Converting: %s", jxrPath.c_str());

  // Build output path: same directory, same name, .jpg extension
  fs::path inputPath(jxrPath);
  fs::path tempPath = inputPath;
  tempPath.replace_extension(L".tmp.jpg");
  fs::path finalPath = inputPath;
  finalPath.replace_extension(L".jpg");

  // --- WIC Decode ---
  ComPtr<IWICImagingFactory> factory;
  HRESULT hr = ::CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                  CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
  if (FAILED(hr)) {
    LogMsg(L"Failed to create WIC factory: 0x%08X", hr);
    return false;
  }

  ComPtr<IWICBitmapDecoder> decoder;
  hr = factory->CreateDecoderFromFilename(
      jxrPath.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
      &decoder);
  if (FAILED(hr)) {
    LogMsg(L"Failed to decode JXR file: 0x%08X", hr);
    return false;
  }

  ComPtr<IWICBitmapFrameDecode> frame;
  hr = decoder->GetFrame(0, &frame);
  if (FAILED(hr)) {
    LogMsg(L"Failed to get frame: 0x%08X", hr);
    return false;
  }

  // Check pixel format
  WICPixelFormatGUID pixFmt = GUID_WICPixelFormatUndefined;
  hr = frame->GetPixelFormat(&pixFmt);
  if (FAILED(hr)) {
    LogMsg(L"Failed to read pixel format: 0x%08X", hr);
    return false;
  }

  // Simple WIC transcode for SDR sources (and HDR formats WIC can't convert
  // to half float). Releases the WIC objects so the source can be deleted.
  auto transcodeSdr = [&]() {
    bool ok =
        TranscodeSdrJxrToJpeg(factory, frame, tempPath.wstring(), jpegQuality);
    frame.Reset();
    decoder.Reset();
    factory.Reset();
    if (!ok) {
      std::error_code ec;
      fs::remove(tempPath, ec);
      return false;
    }
    return FinalizeOutput(tempPath, finalPath, inputPath);
  };

  if (!IsHdrPixelFormat(pixFmt)) {
    LogMsg(L"SDR pixel format detected, performing simple JPEG transcode");
    return transcodeSdr();
  }

  // --- HDR path: convert to half-float RGBA ---
  LogMsg(L"HDR pixel format detected, using Ultra HDR JPEG encoding");

  // Convert to 64bpp RGBA Half Float
  ComPtr<IWICFormatConverter> converter;
  hr = factory->CreateFormatConverter(&converter);
  if (FAILED(hr)) {
    LogMsg(L"Failed to create format converter: 0x%08X", hr);
    return false;
  }

  hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat64bppRGBAHalf,
                             WICBitmapDitherTypeNone, nullptr, 0.0,
                             WICBitmapPaletteTypeCustom);
  if (FAILED(hr)) {
    LogMsg(L"HDR format conversion failed: 0x%08X, falling back to SDR "
           L"transcode",
           hr);
    converter.Reset();
    return transcodeSdr();
  }

  UINT width, height;
  converter->GetSize(&width, &height);

  // 64bpp = 8 bytes per pixel (4 channels × 16-bit half float)
  const UINT bytesPerPixel = 8;
  const UINT stride = width * bytesPerPixel;
  const size_t bufferSize = static_cast<size_t>(stride) * height;

  std::vector<uint8_t> hdrPixels(bufferSize);
  hr = converter->CopyPixels(nullptr, stride, static_cast<UINT>(bufferSize),
                             hdrPixels.data());
  if (FAILED(hr)) {
    LogMsg(L"CopyPixels failed: 0x%08X", hr);
    return false;
  }

  // --- Rescale scRGB to libultrahdr's expected luminance range ---
  // scRGB: SDR white = 1.0 (~80 nits, per sRGB/IEC 61966-2-1).
  // libultrahdr's 64bppRGBAHalfFloat expects 1.0 = 203 nits (BT.2408).
  // Scale factor: 80.0 / 203.0 maps scRGB 1.0 → 0.3941 (which the library
  // correctly interprets as 80 nits, since 0.3941 × 203 ≈ 80).
  // Only RGB is scaled; alpha is coverage, not luminance.
  {
    constexpr float kScRGBToUhdr = 80.0f / 203.0f;
    auto *pixels = reinterpret_cast<uint16_t *>(hdrPixels.data());
    const size_t totalPixels = static_cast<size_t>(width) * height;
    for (size_t p = 0; p < totalPixels; ++p) {
      uint16_t *rgb = pixels + p * 4;
      for (int c = 0; c < 3; ++c) {
        float val = HalfToFloat(rgb[c]) * kScRGBToUhdr;
        // Clamp negatives (out-of-gamut; invalid for Ultra HDR) and NaN.
        if (!(val > 0.0f))
          val = 0.0f;
        rgb[c] = FloatToHalf(val);
      }
    }
  }

  // --- libultrahdr encode (HDR-only mode) ---
  uhdr_codec_private_t *enc = uhdr_create_encoder();
  if (!enc) {
    LogMsg(L"Failed to create uhdr encoder");
    return false;
  }

  // Set up the raw HDR image descriptor
  uhdr_raw_image_t hdrImg = {};
  hdrImg.fmt = UHDR_IMG_FMT_64bppRGBAHalfFloat;
  hdrImg.cg = UHDR_CG_BT_709; // scRGB uses BT.709 primaries
  hdrImg.ct = UHDR_CT_LINEAR; // scRGB is linear
  hdrImg.range = UHDR_CR_FULL_RANGE;
  hdrImg.w = width;
  hdrImg.h = height;
  hdrImg.planes[0] = hdrPixels.data();
  hdrImg.stride[0] = width; // stride in pixels, not bytes
  hdrImg.planes[1] = nullptr;
  hdrImg.planes[2] = nullptr;
  hdrImg.stride[1] = 0;
  hdrImg.stride[2] = 0;

  // Register only the HDR image — libultrahdr will tone-map internally
  uhdr_error_info_t err = uhdr_enc_set_raw_image(enc, &hdrImg, UHDR_HDR_IMG);
  if (err.error_code != UHDR_CODEC_OK) {
    LogMsg(L"uhdr_enc_set_raw_image failed: %hs", err.detail);
    uhdr_release_encoder(enc);
    return false;
  }

  // --- Encoder tuning for high-quality HDR output ---

  // Target display peak brightness (nits). Default for CT_LINEAR is 10000,
  // which wastes gain map precision. 4000 nits covers current gaming displays
  // with generous headroom for highlights.
  err = uhdr_enc_set_target_display_peak_brightness(enc, 4000.0f);
  if (err.error_code != UHDR_CODEC_OK) {
    LogMsg(L"uhdr_enc_set_target_display_peak_brightness failed: %hs",
           err.detail);
    // Non-fatal: continue with default
  }

  // Multi-channel gain map preserves per-channel color accuracy in highlights
  err = uhdr_enc_set_using_multi_channel_gainmap(enc, 1);
  if (err.error_code != UHDR_CODEC_OK) {
    LogMsg(L"uhdr_enc_set_using_multi_channel_gainmap failed: %hs", err.detail);
  }

  // Best quality preset for encoder tuning
  err = uhdr_enc_set_preset(enc, UHDR_USAGE_BEST_QUALITY);
  if (err.error_code != UHDR_CODEC_OK) {
    LogMsg(L"uhdr_enc_set_preset failed: %hs", err.detail);
  }

  // Set quality for SDR base image
  err = uhdr_enc_set_quality(enc, jpegQuality, UHDR_BASE_IMG);
  if (err.error_code != UHDR_CODEC_OK) {
    LogMsg(L"uhdr_enc_set_quality failed: %hs", err.detail);
    uhdr_release_encoder(enc);
    return false;
  }

  // Set quality for gain map image (95 for better HDR reconstruction)
  err = uhdr_enc_set_quality(enc, 95, UHDR_GAIN_MAP_IMG);
  if (err.error_code != UHDR_CODEC_OK) {
    LogMsg(L"uhdr_enc_set_quality (gain map) failed: %hs", err.detail);
    uhdr_release_encoder(enc);
    return false;
  }

  // Encode
  err = uhdr_encode(enc);
  if (err.error_code != UHDR_CODEC_OK) {
    LogMsg(L"uhdr_encode failed: %hs", err.detail);
    uhdr_release_encoder(enc);
    return false;
  }

  // Get encoded stream
  uhdr_compressed_image_t *output = uhdr_get_encoded_stream(enc);
  if (!output || !output->data || output->data_sz == 0) {
    LogMsg(L"uhdr_get_encoded_stream returned null");
    uhdr_release_encoder(enc);
    return false;
  }

  // Write to temp file
  {
    std::ofstream outFile(tempPath, std::ios::binary);
    if (!outFile.is_open()) {
      LogMsg(L"Failed to write temp output file: %s",
             tempPath.wstring().c_str());
      uhdr_release_encoder(enc);
      return false;
    }
    outFile.write(reinterpret_cast<const char *>(output->data),
                  static_cast<std::streamsize>(output->data_sz));
    outFile.close();
    if (!outFile) {
      // e.g. disk full: never promote a truncated JPEG over the original
      LogMsg(L"Failed to write temp output file: %s",
             tempPath.wstring().c_str());
      uhdr_release_encoder(enc);
      std::error_code ec;
      fs::remove(tempPath, ec);
      return false;
    }
  }

  uhdr_release_encoder(enc);

  // Release all WIC COM objects to unlock the source file
  converter.Reset();
  frame.Reset();
  decoder.Reset();
  factory.Reset();

  return FinalizeOutput(tempPath, finalPath, inputPath);
}

} // namespace jxr
