# Study: HDR Support in JPEG and the JXR Format

This document provides a comprehensive overview of how the traditional JPEG format has been extended to support High Dynamic Range (HDR) and an in-depth look at the JXR (JPEG XR) format.

---

## 1. How JPEG Supports HDR

Standard JPEG (JFIF) is natively an 8-bit format designed for Low Dynamic Range (SDR) images. However, several extensions and techniques have been developed to bring HDR capabilities to JPEG while maintaining backward compatibility.

### JPEG XT (ISO/IEC 18477)

JPEG XT is the official ISO extension of the JPEG standard specifically designed to support higher bit depths and HDR.

- **Mechanism:** It uses a **two-layer approach**.
  - **Base Layer:** A standard 8-bit JPEG image (LDR/SDR). Any legacy viewer will see this image.
  - **Extension Layer:** Contains residual data or "refinement" information that, when combined with the base layer by an aware decoder, reconstructs the full HDR image.
- **Versions:**
  - **Part 2:** Based on the Dolby JPEG-HDR format, focusing on integers and high-bit depth metadata.
  - **Part 7:** Supports floating-point coding (similar to OpenEXR) for extreme precision.
- **Bit Depth:** Supports 9 to 16 bits and even 32-bit floating point.

### Ultra HDR & Gain Maps

This is currently the most popular implementation of HDR in JPEG, spearheaded by Google (Android 14+) and supported by Apple and major browsers.

- **The Gain Map Technique:** Instead of an "extension layer" in the traditional sense, Ultra HDR embeds a secondary, low-resolution grayscale image called a **Gain Map** into the JPEG metadata (usually XMP).
- **Reconstruction:**
  1.  The viewer renders the standard 8-bit SDR JPEG.
  2.  If the display supports HDR, the decoder applies the Gain Map to the SDR pixels—effectively "multiplying" the brightness of specific areas based on the map.
- **Standardization:** The industry is moving towards **ISO 21496-1**, which standardizes the gain map format, allowing Android (Ultra HDR) and iOS (Adaptive HDR) photos to be cross-compatible.

---

## 2. What is JXR (JPEG XR)?

**JXR** stands for **JPEG XR** (the "XR" originally standing for "Extended Range"). It is an image compression standard (ISO/IEC 29199-2) that originated from Microsoft's **HD Photo** (formerly Windows Media Photo).

### Key Features of JXR

- **High Bit Depth:** Supports up to 48-bit RGB (16 bits per channel), allowing for significantly more color detail than the 256 levels in standard JPEG.
- **Compression Efficiency:** Offers better quality than standard JPEG at similar file sizes and supports both **lossy** and **lossless** compression using the same algorithm.
- **Advanced Color Spaces:** Supports RGB, CMYK, Monochrome, and even N-channel images. It can use fixed-point or shared-exponent floating-point formats.
- **Symmetric Complexity:** Designed to be computationally efficient for both encoding and decoding, making it suitable for hardware implementations like digital cameras.
- **Tile-Based Access:** Allows parts of the image to be decoded without reading the entire file, which is beneficial for high-resolution images.

### HDR Support in JXR

JXR was built from the ground up with HDR in mind.

- It supports **floating-point values** (e.g., 16-bit or 32-bit floats), which are necessary for professional HDR workflows.
- It uses a "shared exponent" format (similar to Radiance HDR's `.hdr` files) to store a wide range of luminance values efficiently.
- **Current Use Cases:** It is the default format for Windows HDR screenshots (Xbox Game Bar) and is used for HDR wallpapers in Windows 11.

---

## 3. Comparison at a Glance

| Feature                    | Standard JPEG (with Extensions) | JXR (JPEG XR)                       |
| :------------------------- | :------------------------------ | :---------------------------------- |
| **Native Bit Depth**       | 8-bit (Base)                    | Up to 16-bit/channel / Floating Pt  |
| **Backward Compatibility** | High (Shows SDR on old viewers) | Low (Requires specific JXR support) |
| **HDR Method**             | Multi-layer or Gain Maps        | Native bit depth / Floating point   |
| **Origins**                | JPEG Group (ISO)                | Microsoft (HD Photo)                |
| **Typical Use**            | Web, Mobile (Android/iOS)       | Windows HDR, Game screenshots       |

## Conclusion

While **JPEG (via Gain Maps)** is winning the battle for consumer HDR photography due to its perfect backward compatibility, **JXR** remains a powerful, specialized format for high-bit depth imagery and professional Windows-based HDR workflows.
