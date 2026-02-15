#include "Converter.h"
#include "Utils.h"

#include <cmath>
#include <cstdint>
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

static inline uint16_t FloatToHalf(float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, 4);

  uint32_t sign = (bits >> 16) & 0x8000;
  int32_t exponent = ((bits >> 23) & 0xFF) - 127 + 15;
  uint32_t mantissa = bits & 0x007FFFFFu;

  if (exponent <= 0) {
    if (exponent < -10)
      return static_cast<uint16_t>(sign); // Too small, flush to ±0
    // Subnormal
    mantissa |= 0x00800000u;
    uint32_t shift = static_cast<uint32_t>(1 - exponent);
    mantissa >>= shift;
    return static_cast<uint16_t>(sign | (mantissa >> 13));
  } else if (exponent >= 31) {
    // Overflow → Inf, or NaN passthrough
    if (exponent == 31 && mantissa != 0)
      return static_cast<uint16_t>(sign | 0x7C00 | (mantissa >> 13)); // NaN
    return static_cast<uint16_t>(sign | 0x7C00);                      // ±Inf
  }

  return static_cast<uint16_t>(sign | (exponent << 10) | (mantissa >> 13));
}

namespace jxr {

// ============================================================================
// Helper: Check if a WIC pixel format is HDR (high bit depth / float)
// ============================================================================
static bool IsHdrPixelFormat(const WICPixelFormatGUID &fmt) {
  return IsEqualGUID(fmt, GUID_WICPixelFormat64bppRGBAHalf) ||
         IsEqualGUID(fmt, GUID_WICPixelFormat128bppRGBAFloat) ||
         IsEqualGUID(fmt, GUID_WICPixelFormat128bppRGBFloat) ||
         IsEqualGUID(fmt, GUID_WICPixelFormat48bppRGBHalf) ||
         IsEqualGUID(fmt, GUID_WICPixelFormat64bppRGBHalf);
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
  WICPixelFormatGUID pixFmt;
  frame->GetPixelFormat(&pixFmt);

  // If SDR (8-bit), do a simple transcode without libultrahdr
  if (!IsHdrPixelFormat(pixFmt)) {
    LogMsg(L"SDR pixel format detected, performing simple JPEG transcode");
    bool ok =
        TranscodeSdrJxrToJpeg(factory, frame, tempPath.wstring(), jpegQuality);
    // Release all WIC COM objects to unlock the source file
    frame.Reset();
    decoder.Reset();
    factory.Reset();
    if (ok) {
      std::error_code ec;
      fs::remove(inputPath, ec);
      if (ec) {
        LogMsg(L"Could not delete original JXR (locked?): %hs — keeping both "
               L"files",
               ec.message().c_str());
      }
      fs::rename(tempPath, finalPath, ec);
      if (ec) {
        LogMsg(L"File replace failed: %hs", ec.message().c_str());
        return false;
      }
      LogMsg(L"SDR conversion complete: %s", finalPath.wstring().c_str());
    }
    return ok;
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
    LogMsg(L"HDR format conversion failed: 0x%08X", hr);
    return false;
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
  {
    constexpr float kScRGBToUhdr = 80.0f / 203.0f;
    auto *pixels = reinterpret_cast<uint16_t *>(hdrPixels.data());
    const size_t totalComponents = static_cast<size_t>(width) * height * 4;
    for (size_t i = 0; i < totalComponents; ++i) {
      float val = HalfToFloat(pixels[i]);
      val *= kScRGBToUhdr;
      if (val < 0.0f)
        val = 0.0f; // Clamp negatives (out-of-gamut; invalid for Ultra HDR)
      pixels[i] = FloatToHalf(val);
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
                  output->data_sz);
    outFile.close();
  }

  uhdr_release_encoder(enc);

  // Release all WIC COM objects to unlock the source file
  converter.Reset();
  frame.Reset();
  decoder.Reset();
  factory.Reset();

  // --- Atomic replace ---
  std::error_code ec;
  fs::remove(inputPath, ec);
  if (ec) {
    LogMsg(L"Could not delete original JXR (locked?): %hs — keeping both files",
           ec.message().c_str());
    // Still rename the temp to final so the conversion output is usable
    fs::rename(tempPath, finalPath, ec);
    if (ec) {
      LogMsg(L"Failed to rename temp file to final: %hs", ec.message().c_str());
      return false;
    }
    LogMsg(L"HDR conversion complete (original kept): %s (%.1f KB)",
           finalPath.wstring().c_str(),
           static_cast<double>(fs::file_size(finalPath)) / 1024.0);
    return true;
  }

  fs::rename(tempPath, finalPath, ec);
  if (ec) {
    LogMsg(L"Failed to rename temp file to final: %hs", ec.message().c_str());
    return false;
  }

  LogMsg(L"HDR conversion complete: %s (%.1f KB)", finalPath.wstring().c_str(),
         static_cast<double>(fs::file_size(finalPath)) / 1024.0);
  return true;
}

} // namespace jxr
