/**
 * Xtc.cpp
 *
 * Main XTC ebook class implementation
 * XTC ebook support for CrossPoint Reader
 */

#include "Xtc.h"

#include <Bitmap.h>
#include <ErrorReport.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include "Xtc/ThumbScale.h"

bool Xtc::load() {
  LOG_DBG("XTC", "Loading XTC: %s", filepath.c_str());

  // Initialize parser
  parser = makeUniqueNoThrow<xtc::XtcParser>();
  if (!parser) {
    LOG_ERR_OOM("XTC", "XtcParser", sizeof(xtc::XtcParser));
    return false;
  }

  // Open XTC file
  xtc::XtcError err = parser->open(filepath.c_str());
  if (err != xtc::XtcError::OK) {
    LOG_ERR("XTC", "Failed to load: %s", xtc::errorToString(err));
    parser.reset();
    return false;
  }

  loaded = true;
  LOG_DBG("XTC", "Loaded XTC: %s (%lu pages)", filepath.c_str(), parser->getPageCount());
  return true;
}

bool Xtc::clearCache() const {
  if (!Storage.exists(cachePath.c_str())) {
    LOG_DBG("XTC", "Cache does not exist, no action needed");
    return true;
  }

  if (!Storage.removeDir(cachePath.c_str())) {
    LOG_ERR("XTC", "Failed to clear cache");
    return false;
  }

  LOG_DBG("XTC", "Cache cleared successfully");
  return true;
}

void Xtc::setupCacheDir() const {
  if (Storage.exists(cachePath.c_str())) {
    return;
  }

  // Create directories recursively
  for (size_t i = 1; i < cachePath.length(); i++) {
    if (cachePath[i] == '/') {
      Storage.mkdir(cachePath.substr(0, i).c_str());
    }
  }
  Storage.mkdir(cachePath.c_str());
}

std::string Xtc::getTitle() const {
  if (!loaded || !parser) {
    return "";
  }

  // Try to get title from XTC metadata first
  std::string title = parser->getTitle();
  if (!title.empty()) {
    return title;
  }

  // Fallback: extract filename from path as title
  size_t lastSlash = filepath.find_last_of('/');
  size_t lastDot = filepath.find_last_of('.');

  if (lastSlash == std::string::npos) {
    lastSlash = 0;
  } else {
    lastSlash++;
  }

  if (lastDot == std::string::npos || lastDot <= lastSlash) {
    return filepath.substr(lastSlash);
  }

  return filepath.substr(lastSlash, lastDot - lastSlash);
}

std::string Xtc::getAuthor() const {
  if (!loaded || !parser) {
    return "";
  }

  // Try to get author from XTC metadata
  return parser->getAuthor();
}

bool Xtc::hasChapters() const {
  if (!loaded || !parser) {
    return false;
  }
  return parser->hasChapters();
}

const std::vector<xtc::ChapterInfo>& Xtc::getChapters() {
  static const std::vector<xtc::ChapterInfo> kEmpty;
  if (!loaded || !parser) {
    return kEmpty;
  }
  return parser->getChapters();
}

std::string Xtc::getCoverBmpPath() const {
  // XTH (2-bit) covers switched from a 1-bit hard-threshold to a 2-bit grayscale
  // BMP. generateCoverBmp() early-returns when the file already exists, so a stale
  // 1-bit cover.bmp from before the format change would never regenerate. Version
  // the 2-bit path to cover_v2.bmp so the old file is bypassed and the grayscale
  // one is produced; XTG (1-bit) covers keep cover.bmp (format unchanged).
  if (getBitDepth() == 2) {
    return cachePath + "/cover_v2.bmp";
  }
  return cachePath + "/cover.bmp";
}

bool Xtc::generateCoverBmp() const {
  // Already generated
  if (Storage.exists(getCoverBmpPath().c_str())) {
    return true;
  }

  if (!loaded || !parser) {
    LOG_ERR("XTC", "Cannot generate cover BMP, file not loaded");
    return false;
  }

  if (parser->getPageCount() == 0) {
    LOG_ERR("XTC", "No pages in XTC file");
    return false;
  }

  // Setup cache directory
  setupCacheDir();

  // Get first page info for cover
  xtc::PageInfo pageInfo;
  if (!parser->getPageInfo(0, pageInfo)) {
    LOG_DBG("XTC", "Failed to get first page info");
    return false;
  }

  // Get bit depth
  const uint8_t bitDepth = parser->getBitDepth();

  // -------------------------------------------------------------------------
  // XTH (2-bit) cover: bounded-RAM, native-palette 2-bit GRAYSCALE.
  //
  // The full XTH page is column-major and would be ((w*h+7)/8)*2 ~= 96KB for
  // 480x800 - too large for the low-heap sleep screen, which is why the old
  // path's malloc failed and no cover.bmp was ever written. Instead we read the
  // page as contiguous vertical strips (generateCoverBmp2BitGray) and downscale
  // to a holdable grayscale target. The sleep screen upscales the result; a
  // downscaled 4-level grayscale cover reads as far more refined than the old
  // full-res 1-bit hard threshold, and the small grayscale buffer is holdable.
  //
  // Target box: fit the page inside COVER_TARGET_LONG x COVER_TARGET_SHORT
  // (aspect-preserved, downscale only). The 2-bit column-major output buffer is
  // dstWidth * ((dstHeight*2+7)/8); the box is chosen so this stays < ~30KB in
  // any orientation (worst case 264*((440*2+7)/8) = 264*110 ~= 29KB), well under
  // the 96KB full page and the < 32KB budget. Plus a few KB of strip buffers.
  // -------------------------------------------------------------------------
  if (bitDepth == 2) {
    constexpr int COVER_TARGET_LONG = 440;
    constexpr int COVER_TARGET_SHORT = 264;
    const int boxW = (pageInfo.width >= pageInfo.height) ? COVER_TARGET_LONG : COVER_TARGET_SHORT;
    const int boxH = (pageInfo.width >= pageInfo.height) ? COVER_TARGET_SHORT : COVER_TARGET_LONG;

    // Fit-inside scale (downscale only). If the page already fits the box,
    // scale == 1 and we keep full resolution at 4-level grayscale.
    const float scaleX = static_cast<float>(boxW) / pageInfo.width;
    const float scaleY = static_cast<float>(boxH) / pageInfo.height;
    float scale = (scaleX < scaleY) ? scaleX : scaleY;
    if (scale > 1.0f) scale = 1.0f;

    uint16_t dstWidth = static_cast<uint16_t>(pageInfo.width * scale);
    uint16_t dstHeight = static_cast<uint16_t>(pageInfo.height * scale);
    if (dstWidth == 0) dstWidth = 1;
    if (dstHeight == 0) dstHeight = 1;

    // Per-axis inverse-scale (16.16) for the area-averaging downscaler. Using
    // the exact source/destination ratio per axis keeps the mapping accurate
    // even when integer truncation makes the two axes' effective scales differ.
    const uint32_t scaleInvX_fp =
        static_cast<uint32_t>((static_cast<uint64_t>(pageInfo.width) << 16) / dstWidth);
    const uint32_t scaleInvY_fp =
        static_cast<uint32_t>((static_cast<uint64_t>(pageInfo.height) << 16) / dstHeight);

    LOG_DBG("XTC", "Generating 2-bit gray cover BMP: %dx%d -> %dx%d (scale: %.3f)", pageInfo.width, pageInfo.height,
            dstWidth, dstHeight, scale);

    return generateCoverBmp2BitGray(pageInfo, dstWidth, dstHeight, scaleInvX_fp, scaleInvY_fp, getCoverBmpPath());
  }

  // -------------------------------------------------------------------------
  // XTG (1-bit) cover: unchanged. Row-major page is small (~48KB for 800x480)
  // and writes directly into a 1-bit BMP with no conversion.
  // -------------------------------------------------------------------------
  const size_t bitmapSize = ((pageInfo.width + 7) / 8) * pageInfo.height;
  auto pageBuffer = makeUniqueNoThrow<uint8_t[]>(bitmapSize);
  if (!pageBuffer) {
    LOG_ERR("XTC", "Failed to allocate page buffer (%lu bytes)", static_cast<unsigned long>(bitmapSize));
    return false;
  }

  // Load first page (cover)
  if (const_cast<xtc::XtcParser*>(parser.get())->loadPage(0, pageBuffer.get(), bitmapSize) == 0) {
    LOG_ERR("XTC", "Failed to load cover page");
    return false;
  }

  // Create BMP file
  HalFile coverBmp;
  if (!Storage.openFileForWrite("XTC", getCoverBmpPath(), coverBmp)) {
    LOG_DBG("XTC", "Failed to create cover BMP file");
    return false;
  }

  // Write 1-bit BMP header (top-down row order)
  BmpHeader bmpHeader;
  createBmpHeader(&bmpHeader, pageInfo.width, pageInfo.height, BmpRowOrder::TopDown);
  coverBmp.write(reinterpret_cast<const uint8_t*>(&bmpHeader), sizeof(bmpHeader));

  const uint32_t rowSize = ((pageInfo.width + 31) / 32) * 4;
  const size_t srcRowSize = (pageInfo.width + 7) / 8;

  for (uint16_t y = 0; y < pageInfo.height; y++) {
    // Write source row
    coverBmp.write(pageBuffer.get() + y * srcRowSize, srcRowSize);

    // Pad to 4-byte boundary
    uint8_t padding[4] = {0, 0, 0, 0};
    size_t paddingSize = rowSize - srcRowSize;
    if (paddingSize > 0) {
      coverBmp.write(padding, paddingSize);
    }
  }

  // pageBuffer freed automatically (unique_ptr).

  LOG_DBG("XTC", "Generated cover BMP: %s", getCoverBmpPath().c_str());
  return true;
}

bool Xtc::generateCoverBmp2BitGray(const xtc::PageInfo& pageInfo, uint16_t dstWidth, uint16_t dstHeight,
                                   uint32_t scaleInvX_fp, uint32_t scaleInvY_fp, const std::string& outPath) const {
  // Bounded-RAM XTH (2-bit) cover. Shares generateThumbBmp2Bit's strip-read
  // machinery (contiguous vertical strips, area-averaging downscale) but emits
  // 4-level GRAYSCALE instead of a 1-bit dither, and writes it as a 2-bit
  // native-palette BMP so the sleep screen renders it at the display's 4 native
  // gray levels with NO dithering pass.
  //
  // RAM: the 2-bit output is held column-major (dstWidth * dstColBytes, where
  // dstColBytes = (dstHeight*2+7)/8) so it can be transposed into a top-down BMP
  // at the end; plus one strip of both planes (a few KB). No FS error columns are
  // needed (direct gray quantization), and the full 96KB page is never allocated.
  const size_t colBytes = (pageInfo.height + 7) / 8;  // bytes per source column
  const size_t planeSize = (static_cast<size_t>(pageInfo.width) * pageInfo.height + 7) / 8;

  // Output, column-major 2-bit: column dstX occupies dstColBytes bytes; within a
  // column, pixel dstY lives at byte (dstY/4), bits 6-2*(dstY&3) (MSB = topmost),
  // matching the BMP 2bpp packing (4 pixels/byte, MSB first).
  const size_t dstColBytes = (static_cast<size_t>(dstHeight) * 2 + 7) / 8;
  auto outCols = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(dstWidth) * dstColBytes);
  if (!outCols) {
    LOG_ERR("XTC", "OOM: cover column buffer (%lu bytes)",
            static_cast<unsigned long>(static_cast<size_t>(dstWidth) * dstColBytes));
    return false;
  }
  memset(outCols.get(), 0x00, static_cast<size_t>(dstWidth) * dstColBytes);  // all black (level 0), set lighter below

  // Strip read buffers (both planes), sized for STRIP_DST_COLS output columns.
  // Identical contiguous-strip scheme as generateThumbBmp2Bit: a contiguous range
  // of source columns is a contiguous file run per plane.
  constexpr size_t STRIP_DST_COLS = 64;
  size_t stripSrcColsCap = 0;
  std::unique_ptr<uint8_t[]> strip1, strip2;
  size_t stripColLo = 0;     // lowest source-column index resident (srcX space)
  size_t stripColCount = 0;  // source columns resident
  auto* mutableParser = const_cast<xtc::XtcParser*>(parser.get());

  // Read source columns [srcXLo, srcXHi) of both planes as one contiguous run each.
  // colIndex = width-1-srcX decreases as srcX increases, so srcX in [srcXLo, srcXHi)
  // maps to colIndex in [width-srcXHi, width-srcXLo): one contiguous file run/plane.
  auto loadStrip = [&](size_t srcXLo, size_t srcXHi) -> bool {
    const size_t n = srcXHi - srcXLo;
    if (n > stripSrcColsCap) {
      const size_t cap = n * colBytes;
      auto s1 = makeUniqueNoThrow<uint8_t[]>(cap);
      auto s2 = makeUniqueNoThrow<uint8_t[]>(cap);
      if (!s1 || !s2) {
        LOG_ERR("XTC", "OOM: strip buffers (2x %lu bytes)", static_cast<unsigned long>(cap));
        return false;
      }
      strip1 = std::move(s1);
      strip2 = std::move(s2);
      stripSrcColsCap = n;
    }
    const size_t colIdxLo = static_cast<size_t>(pageInfo.width) - srcXHi;  // lowest colIndex
    const size_t srcOff = colIdxLo * colBytes;
    const size_t runBytes = n * colBytes;
    if (mutableParser->loadPageRegion(0, srcOff, strip1.get(), runBytes) == 0) return false;
    if (mutableParser->loadPageRegion(0, planeSize + srcOff, strip2.get(), runBytes) == 0) return false;
    stripColLo = srcXLo;
    stripColCount = n;
    return true;
  };

  // Decode a source pixel (srcX, srcY) from the resident strip -> grayscale 0..255.
  // Same slot math as generateThumbBmp2Bit. XTC polarity MATCHES the device reader
  // (XtcReaderActivity getPixelValue): 0=white, 1=DARK grey, 2=LIGHT grey, 3=black ->
  // luminance 255/85/170/0. The mid greys are NOT a linear ramp, so use an explicit LUT
  // (the old (3-pixelValue)*85 swapped the two mid greys vs the reader).
  auto stripGray = [&](size_t srcX, size_t srcY) -> uint8_t {
    const size_t slot = (stripColCount - 1) - (srcX - stripColLo);  // colIndex order
    const size_t off = slot * colBytes + srcY / 8;
    const size_t bitInByte = 7 - (srcY % 8);
    const uint8_t bit1 = (strip1[off] >> bitInByte) & 1;
    const uint8_t bit2 = (strip2[off] >> bitInByte) & 1;
    const uint8_t pixelValue = (bit1 << 1) | bit2;
    static constexpr uint8_t kXthLum[4] = {255, 85, 170, 0};
    return kXthLum[pixelValue];
  };

  for (uint16_t dstX = 0; dstX < dstWidth; dstX++) {
    uint32_t srcXStart, srcXEnd;
    xtc::computeSrcSpan(dstX, scaleInvX_fp, pageInfo.width, srcXStart, srcXEnd);

    // Ensure this column's source-column span is resident, advancing strips left
    // to right (dstX is monotonic).
    if (srcXStart < stripColLo || srcXEnd > stripColLo + stripColCount) {
      size_t lo = srcXStart;
      size_t hi = lo + (stripSrcColsCap ? stripSrcColsCap : STRIP_DST_COLS);
      if (hi < srcXEnd) hi = srcXEnd;  // span wider than the strip cap
      if (hi > pageInfo.width) hi = pageInfo.width;
      if (!loadStrip(lo, hi)) {
        LOG_ERR("XTC", "Strip read failed at srcX=%lu", static_cast<unsigned long>(srcXStart));
        return false;
      }
    }

    uint8_t* outCol = outCols.get() + static_cast<size_t>(dstX) * dstColBytes;
    for (uint16_t dstY = 0; dstY < dstHeight; dstY++) {
      uint32_t srcYStart, srcYEnd;
      xtc::computeSrcSpan(dstY, scaleInvY_fp, pageInfo.height, srcYStart, srcYEnd);

      // Area-average grayscale over the source block.
      uint32_t graySum = 0, totalCount = 0;
      for (uint32_t srcY = srcYStart; srcY < srcYEnd && srcY < pageInfo.height; srcY++) {
        for (uint32_t srcX = srcXStart; srcX < srcXEnd && srcX < pageInfo.width; srcX++) {
          graySum += stripGray(srcX, srcY);
          totalCount++;
        }
      }
      const uint8_t avgGray = (totalCount > 0) ? static_cast<uint8_t>(graySum / totalCount) : 255;

      // Quantize 0..255 to a native 2-bit level 0..3 (lossless >> 6 reconstruction
      // by the BMP reader). 0=black .. 3=white, matching the palette written below.
      const uint8_t level = avgGray >> 6;  // 0,1,2,3
      // Pack 2 bits into the column-major output (MSB = topmost pixel in the byte).
      const size_t byteInCol = dstY / 4;
      const uint8_t bitShift = static_cast<uint8_t>(6 - 2 * (dstY & 3));
      outCol[byteInCol] |= static_cast<uint8_t>(level << bitShift);
    }
  }

  // -------------------------------------------------------------------------
  // Write a 2-bit native-palette grayscale BMP (top-down). createBmpHeader only
  // produces a 1-bit/2-color header, so build the 2-bit header inline here:
  // 4-entry palette {0,85,170,255}, bfOffBits past the palette. The BMP reader
  // treats bpp<=2 as native palette and maps index -> paletteLum -> >>6 level,
  // so indices 0..3 render at exactly the 4 native gray levels with no dithering.
  // -------------------------------------------------------------------------
  HalFile coverBmp;
  if (!Storage.openFileForWrite("XTC", outPath, coverBmp)) {
    LOG_DBG("XTC", "Failed to create cover BMP file");
    return false;
  }

  // Headers + 4-entry palette, written as raw little-endian bytes (host is LE; we
  // emit bytes explicitly to stay endian-correct and avoid struct-padding issues).
  const uint32_t rowSize = (static_cast<uint32_t>(dstWidth) * 2 + 31) / 32 * 4;
  const uint32_t paletteBytes = 4u * 4u;                                      // 4 entries x BGRA
  const uint32_t offBits = 14u + 40u + paletteBytes;                         // file hdr + DIB + palette
  const uint32_t imageSize = rowSize * dstHeight;
  const uint32_t fileSize = offBits + imageSize;

  auto put16 = [](uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
  };
  auto put32 = [](uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
  };

  uint8_t hdr[14 + 40];
  memset(hdr, 0, sizeof(hdr));
  // BITMAPFILEHEADER (14 bytes)
  hdr[0] = 'B';
  hdr[1] = 'M';
  put32(hdr + 2, fileSize);   // bfSize
  put32(hdr + 10, offBits);   // bfOffBits
  // BITMAPINFOHEADER (40 bytes) at offset 14
  put32(hdr + 14, 40);                                          // biSize
  put32(hdr + 18, static_cast<uint32_t>(dstWidth));            // biWidth
  put32(hdr + 22, static_cast<uint32_t>(-static_cast<int32_t>(dstHeight)));  // biHeight (top-down)
  put16(hdr + 26, 1);                                          // biPlanes
  put16(hdr + 28, 2);                                          // biBitCount = 2
  put32(hdr + 30, 0);                                          // biCompression = BI_RGB
  put32(hdr + 34, imageSize);                                  // biSizeImage
  put32(hdr + 38, 2835);                                       // biXPelsPerMeter
  put32(hdr + 42, 2835);                                       // biYPelsPerMeter
  put32(hdr + 46, 4);                                          // biClrUsed
  put32(hdr + 50, 4);                                          // biClrImportant
  coverBmp.write(hdr, sizeof(hdr));

  // Palette: index 0..3 -> luminance 0,85,170,255 (BGRA, reserved 0).
  uint8_t palette[paletteBytes];
  for (int i = 0; i < 4; i++) {
    const uint8_t lum = static_cast<uint8_t>(i * 85);
    palette[i * 4 + 0] = lum;  // Blue
    palette[i * 4 + 1] = lum;  // Green
    palette[i * 4 + 2] = lum;  // Red
    palette[i * 4 + 3] = 0;    // Reserved
  }
  coverBmp.write(palette, sizeof(palette));

  // Transpose the column-major 2-bit buffer into row-major top-down rows.
  auto rowBuffer = makeUniqueNoThrow<uint8_t[]>(rowSize);
  if (!rowBuffer) {
    LOG_ERR("XTC", "OOM: cover row buffer (%lu bytes)", static_cast<unsigned long>(rowSize));
    return false;
  }
  for (uint16_t dstY = 0; dstY < dstHeight; dstY++) {
    memset(rowBuffer.get(), 0x00, rowSize);
    const size_t srcByte = dstY / 4;
    const uint8_t srcShift = static_cast<uint8_t>(6 - 2 * (dstY & 3));
    for (uint16_t dstX = 0; dstX < dstWidth; dstX++) {
      const uint8_t level = (outCols[static_cast<size_t>(dstX) * dstColBytes + srcByte] >> srcShift) & 0x03;
      // Pack into row-major 2bpp (MSB first, 4 pixels/byte).
      rowBuffer[dstX / 4] |= static_cast<uint8_t>(level << (6 - 2 * (dstX & 3)));
    }
    coverBmp.write(rowBuffer.get(), rowSize);  // padded to 4-byte boundary by rowSize
  }

  LOG_DBG("XTC", "Generated 2-bit gray cover BMP (%dx%d): %s", dstWidth, dstHeight, outPath.c_str());
  return true;
}

std::string Xtc::getThumbBmpPath() const { return cachePath + "/thumb_[HEIGHT].bmp"; }
std::string Xtc::getThumbBmpPath(int height) const { return cachePath + "/thumb_" + std::to_string(height) + ".bmp"; }

bool Xtc::generateThumbBmp(int height) const {
  // Already generated
  if (Storage.exists(getThumbBmpPath(height).c_str())) {
    return true;
  }

  if (!loaded || !parser) {
    LOG_ERR("XTC", "Cannot generate thumb BMP, file not loaded");
    return false;
  }

  if (parser->getPageCount() == 0) {
    LOG_ERR("XTC", "No pages in XTC file");
    return false;
  }

  // Setup cache directory
  setupCacheDir();

  // Get first page info for cover
  xtc::PageInfo pageInfo;
  if (!parser->getPageInfo(0, pageInfo)) {
    LOG_DBG("XTC", "Failed to get first page info");
    return false;
  }

  // Get bit depth
  const uint8_t bitDepth = parser->getBitDepth();

  // Calculate target dimensions for thumbnail (fit within 240x400 Continue Reading card)
  int THUMB_TARGET_WIDTH = height * 0.6;
  int THUMB_TARGET_HEIGHT = height;

  // Calculate scale factor
  float scaleX = static_cast<float>(THUMB_TARGET_WIDTH) / pageInfo.width;
  float scaleY = static_cast<float>(THUMB_TARGET_HEIGHT) / pageInfo.height;
  float scale = (scaleX > scaleY) ? scaleX : scaleY;  // for cropping

  // Only scale down, never up
  if (scale >= 1.0f) {
    // Page is already small enough, just use cover.bmp
    // Copy cover.bmp to thumb.bmp
    if (generateCoverBmp()) {
      HalFile src, dst;
      if (Storage.openFileForRead("XTC", getCoverBmpPath(), src)) {
        if (Storage.openFileForWrite("XTC", getThumbBmpPath(height), dst)) {
          uint8_t buffer[512];
          while (src.available()) {
            size_t bytesRead = src.read(buffer, sizeof(buffer));
            dst.write(buffer, bytesRead);
          }
        }
      }
      LOG_DBG("XTC", "Copied cover to thumb (no scaling needed)");
      return Storage.exists(getThumbBmpPath(height).c_str());
    }
    return false;
  }

  uint16_t thumbWidth = static_cast<uint16_t>(pageInfo.width * scale);
  uint16_t thumbHeight = static_cast<uint16_t>(pageInfo.height * scale);

  LOG_DBG("XTC", "Generating thumb BMP: %dx%d -> %dx%d (scale: %.3f)", pageInfo.width, pageInfo.height, thumbWidth,
          thumbHeight, scale);

  // Fixed-point downscale factor (16.16), shared by both axes and both paths.
  const uint32_t scaleInv_fp = xtc::makeScaleInvFp(scale);

  // 2-bit (XTH) takes a dedicated column-major, strip-read path (fast + dithered);
  // 1-bit (XTG) keeps the original whole-page, row-major path below.
  if (bitDepth == 2) {
    return generateThumbBmp2Bit(pageInfo, thumbWidth, thumbHeight, scaleInv_fp, getThumbBmpPath(height));
  }

  // -------------------------------------------------------------------------
  // 1-bit (XTG) path: row-major. The full page is small and row-major, so keep
  // the original whole-page load. ((width+7)/8)*height ~= 48KB for 800x480.
  // -------------------------------------------------------------------------
  const size_t srcRowBytes = (pageInfo.width + 7) / 8;
  const size_t bitmapSize = srcRowBytes * pageInfo.height;

  auto pageBuffer = makeUniqueNoThrow<uint8_t[]>(bitmapSize);
  if (!pageBuffer) {
    LOG_ERR("XTC", "Failed to allocate page buffer (%lu bytes)", static_cast<unsigned long>(bitmapSize));
    return false;
  }
  if (const_cast<xtc::XtcParser*>(parser.get())->loadPage(0, pageBuffer.get(), bitmapSize) == 0) {
    LOG_ERR("XTC", "Failed to load cover page for thumb");
    return false;
  }

  // Create thumbnail BMP file - use 1-bit format for fast home screen rendering (no gray passes)
  HalFile thumbBmp;
  if (!Storage.openFileForWrite("XTC", getThumbBmpPath(height), thumbBmp)) {
    LOG_DBG("XTC", "Failed to create thumb BMP file");
    return false;
  }

  // Write 1-bit BMP header (top-down row order)
  BmpHeader bmpHeader;
  createBmpHeader(&bmpHeader, thumbWidth, thumbHeight, BmpRowOrder::TopDown);
  thumbBmp.write(reinterpret_cast<const uint8_t*>(&bmpHeader), sizeof(bmpHeader));

  const uint32_t rowSize = (thumbWidth + 31) / 32 * 4;

  // Allocate row buffer for 1-bit output
  auto rowBuffer = makeUniqueNoThrow<uint8_t[]>(rowSize);
  if (!rowBuffer) {
    LOG_ERR("XTC", "OOM: thumb row buffer (%lu bytes)", static_cast<unsigned long>(rowSize));
    return false;
  }

  for (uint16_t dstY = 0; dstY < thumbHeight; dstY++) {
    memset(rowBuffer.get(), 0xFF, rowSize);  // Start with all white (bit 1)

    uint32_t srcYStart, srcYEnd;
    xtc::computeSrcSpan(dstY, scaleInv_fp, pageInfo.height, srcYStart, srcYEnd);

    for (uint16_t dstX = 0; dstX < thumbWidth; dstX++) {
      uint32_t srcXStart, srcXEnd;
      xtc::computeSrcSpan(dstX, scaleInv_fp, pageInfo.width, srcXStart, srcXEnd);

      // Area averaging: sum grayscale values (0-255 range)
      uint32_t graySum = 0;
      uint32_t totalCount = 0;
      for (uint32_t srcY = srcYStart; srcY < srcYEnd && srcY < pageInfo.height; srcY++) {
        for (uint32_t srcX = srcXStart; srcX < srcXEnd && srcX < pageInfo.width; srcX++) {
          uint8_t grayValue = 255;  // Default: white
          const size_t byteIdx = srcY * srcRowBytes + srcX / 8;
          const size_t bitIdx = 7 - (srcX % 8);
          if (byteIdx < bitmapSize) {
            const uint8_t pixelBit = (pageBuffer[byteIdx] >> bitIdx) & 1;
            // XTC 1-bit polarity: 0=black, 1=white (same as BMP palette)
            grayValue = pixelBit ? 255 : 0;
          }
          graySum += grayValue;
          totalCount++;
        }
      }

      // Calculate average grayscale and quantize to 1-bit with noise dithering
      uint8_t avgGray = (totalCount > 0) ? static_cast<uint8_t>(graySum / totalCount) : 255;

      // Hash-based noise dithering for 1-bit output
      uint32_t hash = static_cast<uint32_t>(dstX) * 374761393u + static_cast<uint32_t>(dstY) * 668265263u;
      hash = (hash ^ (hash >> 13)) * 1274126177u;
      const int threshold = static_cast<int>(hash >> 24);           // 0-255
      const int adjustedThreshold = 128 + ((threshold - 128) / 2);  // Range: 64-192

      // Quantize to 1-bit: 0=black, 1=white
      uint8_t oneBit = (avgGray >= adjustedThreshold) ? 1 : 0;

      // Pack 1-bit value into row buffer (MSB first, 8 pixels per byte)
      const size_t byteIndex = dstX / 8;
      const size_t bitOffset = 7 - (dstX % 8);
      if (byteIndex < rowSize) {
        if (oneBit) {
          rowBuffer[byteIndex] |= (1 << bitOffset);  // Set bit for white
        } else {
          rowBuffer[byteIndex] &= ~(1 << bitOffset);  // Clear bit for black
        }
      }
    }

    // Write row (already padded to 4-byte boundary by rowSize)
    thumbBmp.write(rowBuffer.get(), rowSize);
  }

  // pageBuffer / rowBuffer freed automatically (unique_ptr).

  LOG_DBG("XTC", "Generated thumb BMP (%dx%d): %s", thumbWidth, thumbHeight, getThumbBmpPath(height).c_str());
  return true;
}

bool Xtc::generateThumbBmp2Bit(const xtc::PageInfo& pageInfo, uint16_t thumbWidth, uint16_t thumbHeight,
                               uint32_t scaleInv_fp, const std::string& outPath) const {
  // XTH (2-bit) cover thumbnail. Two design goals over the old per-column band path:
  //
  // SPEED. XTH is column-major on disk: plane1 at offset 0, plane2 at planeSize;
  // within a plane, source column c occupies colBytes contiguous bytes at c*colBytes.
  // colIndex = width-1-srcX, so a CONTIGUOUS range of source columns is a contiguous
  // file run. We traverse OUTPUT columns left to right; each output column maps to a
  // monotonically advancing source-column span, so we read the page as a few CONTIGUOUS
  // VERTICAL STRIPS (STRIP_DST_COLS output columns each) - one loadPageRegion per plane
  // per strip (~4-8 strips total for a 240-wide thumb), instead of one read per source
  // column (~9600 reads, ~30s on device).
  //
  // QUALITY. Output is produced column by column, so we dither column-wise with
  // 1-bit Floyd-Steinberg (xtc::ColumnFloydSteinberg) - a 90-degree-rotated FS that
  // diffuses error down within a column and rightward to the next column. The 4 XTH
  // gray levels error-diffuse to 1-bit so gradients stay smooth, not blocky.
  //
  // RAM. The 1-bit output is held column-major in RAM (thumbWidth * thumbColBytes,
  // ~12KB for 240x400), one strip of both planes (STRIP_DST_COLS worth of source
  // columns, a few KB), and two FS error columns (~1.6KB). Peak well under the old
  // 96KB full-page buffer, target < ~32KB. BMP is written once at the end.
  const size_t colBytes = (pageInfo.height + 7) / 8;  // bytes per source column
  const size_t planeSize = (static_cast<size_t>(pageInfo.width) * pageInfo.height + 7) / 8;

  // Output thumbnail, column-major 1-bit: column dstX occupies thumbColBytes bytes,
  // byte (dstY/8) holds 8 vertical pixels (MSB = topmost). Transposed to a row-major
  // top-down BMP at the end.
  const size_t thumbColBytes = (static_cast<size_t>(thumbHeight) + 7) / 8;
  auto outCols = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(thumbWidth) * thumbColBytes);
  if (!outCols) {
    LOG_ERR("XTC", "OOM: thumb column buffer (%lu bytes)",
            static_cast<unsigned long>(static_cast<size_t>(thumbWidth) * thumbColBytes));
    return false;
  }
  memset(outCols.get(), 0xFF, static_cast<size_t>(thumbWidth) * thumbColBytes);  // all white

  // Floyd-Steinberg column error scratch (two columns, swapped per output column).
  auto fsCur = makeUniqueNoThrow<int16_t[]>(thumbHeight);
  auto fsNext = makeUniqueNoThrow<int16_t[]>(thumbHeight);
  if (!fsCur || !fsNext) {
    LOG_ERR("XTC", "OOM: FS error columns (2x %lu bytes)",
            static_cast<unsigned long>(static_cast<size_t>(thumbHeight) * sizeof(int16_t)));
    return false;
  }
  xtc::ColumnFloydSteinberg fs(fsCur.get(), fsNext.get(), thumbHeight);

  // Strip read buffers (both planes), sized for STRIP_DST_COLS output columns.
  // stripSrcCols grows on demand if one strip's source-column span exceeds the cap
  // (only under extreme downscale). Each plane's strip is column-major: source
  // column k of the strip at strip[k*colBytes ...].
  constexpr size_t STRIP_DST_COLS = 64;
  size_t stripSrcColsCap = 0;
  std::unique_ptr<uint8_t[]> strip1, strip2;
  size_t stripColLo = 0;    // lowest source-column index resident (srcX space)
  size_t stripColCount = 0;  // source columns resident
  auto* mutableParser = const_cast<xtc::XtcParser*>(parser.get());

  // Read source columns [srcXLo, srcXHi) of both planes as one contiguous run each.
  // colIndex decreases as srcX increases, so srcX in [srcXLo, srcXHi) is colIndex in
  // [width-srcXHi, width-srcXLo): one contiguous file run per plane.
  auto loadStrip = [&](size_t srcXLo, size_t srcXHi) -> bool {
    const size_t n = srcXHi - srcXLo;
    if (n > stripSrcColsCap) {
      const size_t cap = n * colBytes;
      auto s1 = makeUniqueNoThrow<uint8_t[]>(cap);
      auto s2 = makeUniqueNoThrow<uint8_t[]>(cap);
      if (!s1 || !s2) {
        LOG_ERR("XTC", "OOM: strip buffers (2x %lu bytes)", static_cast<unsigned long>(cap));
        return false;
      }
      strip1 = std::move(s1);
      strip2 = std::move(s2);
      stripSrcColsCap = n;
    }
    const size_t colIdxLo = static_cast<size_t>(pageInfo.width) - srcXHi;  // lowest colIndex
    const size_t srcOff = colIdxLo * colBytes;
    const size_t runBytes = n * colBytes;
    if (mutableParser->loadPageRegion(0, srcOff, strip1.get(), runBytes) == 0) return false;
    if (mutableParser->loadPageRegion(0, planeSize + srcOff, strip2.get(), runBytes) == 0) return false;
    stripColLo = srcXLo;
    stripColCount = n;
    return true;
  };

  // Decode a source pixel (srcX, srcY) from the resident strip -> grayscale 0..255.
  // colIndex = width-1-srcX; within the strip, source column srcX lives at strip
  // slot (srcX - stripColLo) in colIndex order: colIndex - colIdxLo = (width-1-srcX) -
  // (width-srcXHi) = stripColCount-1 - (srcX - stripColLo). Decode byte srcY/8.
  auto stripGray = [&](size_t srcX, size_t srcY) -> uint8_t {
    const size_t slot = (stripColCount - 1) - (srcX - stripColLo);  // colIndex order
    const size_t off = slot * colBytes + srcY / 8;
    const size_t bitInByte = 7 - (srcY % 8);
    const uint8_t bit1 = (strip1[off] >> bitInByte) & 1;
    const uint8_t bit2 = (strip2[off] >> bitInByte) & 1;
    const uint8_t pixelValue = (bit1 << 1) | bit2;
    // XTC polarity MATCHES the device reader: 0=white, 1=DARK grey, 2=LIGHT grey,
    // 3=black -> luminance 255/85/170/0 (explicit LUT; mid greys are not a linear ramp).
    static constexpr uint8_t kXthLum[4] = {255, 85, 170, 0};
    return kXthLum[pixelValue];
  };

  for (uint16_t dstX = 0; dstX < thumbWidth; dstX++) {
    uint32_t srcXStart, srcXEnd;
    xtc::computeSrcSpan(dstX, scaleInv_fp, pageInfo.width, srcXStart, srcXEnd);

    // Ensure this column's source-column span is resident. dstX increases
    // monotonically, so we advance strips left to right, anchoring each new strip
    // at this column's first source column so its full span fits.
    if (srcXStart < stripColLo || srcXEnd > stripColLo + stripColCount) {
      size_t lo = srcXStart;
      size_t hi = lo + (stripSrcColsCap ? stripSrcColsCap : STRIP_DST_COLS);
      if (hi < srcXEnd) hi = srcXEnd;  // span wider than the strip cap
      if (hi > pageInfo.width) hi = pageInfo.width;
      if (!loadStrip(lo, hi)) {
        LOG_ERR("XTC", "Strip read failed at srcX=%lu", static_cast<unsigned long>(srcXStart));
        return false;
      }
    }

    uint8_t* outCol = outCols.get() + static_cast<size_t>(dstX) * thumbColBytes;
    for (uint16_t dstY = 0; dstY < thumbHeight; dstY++) {
      uint32_t srcYStart, srcYEnd;
      xtc::computeSrcSpan(dstY, scaleInv_fp, pageInfo.height, srcYStart, srcYEnd);

      // Area-average grayscale over the source block.
      uint32_t graySum = 0, totalCount = 0;
      for (uint32_t srcY = srcYStart; srcY < srcYEnd && srcY < pageInfo.height; srcY++) {
        for (uint32_t srcX = srcXStart; srcX < srcXEnd && srcX < pageInfo.width; srcX++) {
          graySum += stripGray(srcX, srcY);
          totalCount++;
        }
      }
      const uint8_t avgGray = (totalCount > 0) ? static_cast<uint8_t>(graySum / totalCount) : 255;

      // Column-wise 1-bit Floyd-Steinberg: 1=white, 0=black.
      if (fs.processPixel(avgGray, dstY) == 0) {
        outCol[dstY / 8] &= ~(1 << (7 - (dstY % 8)));  // black
      }
      // white bits already set by the initial 0xFF fill.
    }
    fs.nextColumn();
  }

  // Write the BMP: 1-bit, top-down, 4-byte row align. Transpose the column-major
  // output buffer into row-major rows.
  HalFile thumbBmp;
  if (!Storage.openFileForWrite("XTC", outPath, thumbBmp)) {
    LOG_DBG("XTC", "Failed to create thumb BMP file");
    return false;
  }
  BmpHeader bmpHeader;
  createBmpHeader(&bmpHeader, thumbWidth, thumbHeight, BmpRowOrder::TopDown);
  thumbBmp.write(reinterpret_cast<const uint8_t*>(&bmpHeader), sizeof(bmpHeader));

  const uint32_t rowSize = (thumbWidth + 31) / 32 * 4;
  auto rowBuffer = makeUniqueNoThrow<uint8_t[]>(rowSize);
  if (!rowBuffer) {
    LOG_ERR("XTC", "OOM: thumb row buffer (%lu bytes)", static_cast<unsigned long>(rowSize));
    return false;
  }
  for (uint16_t dstY = 0; dstY < thumbHeight; dstY++) {
    memset(rowBuffer.get(), 0xFF, rowSize);  // all white
    const size_t srcByte = dstY / 8;
    const size_t srcBit = 7 - (dstY % 8);
    for (uint16_t dstX = 0; dstX < thumbWidth; dstX++) {
      const uint8_t bit = (outCols[static_cast<size_t>(dstX) * thumbColBytes + srcByte] >> srcBit) & 1;
      if (!bit) {  // black
        rowBuffer[dstX / 8] &= ~(1 << (7 - (dstX % 8)));
      }
    }
    thumbBmp.write(rowBuffer.get(), rowSize);  // padded to 4-byte boundary by rowSize
  }

  LOG_DBG("XTC", "Generated thumb BMP (%dx%d): %s", thumbWidth, thumbHeight, outPath.c_str());
  return true;
}

uint32_t Xtc::getPageCount() const {
  if (!loaded || !parser) {
    return 0;
  }
  return parser->getPageCount();
}

uint16_t Xtc::getPageWidth() const {
  if (!loaded || !parser) {
    return 0;
  }
  return parser->getWidth();
}

uint16_t Xtc::getPageHeight() const {
  if (!loaded || !parser) {
    return 0;
  }
  return parser->getHeight();
}

uint8_t Xtc::getBitDepth() const {
  if (!loaded || !parser) {
    return 1;  // Default to 1-bit
  }
  return parser->getBitDepth();
}

size_t Xtc::loadPage(uint32_t pageIndex, uint8_t* buffer, size_t bufferSize) const {
  if (!loaded || !parser) {
    return 0;
  }
  return const_cast<xtc::XtcParser*>(parser.get())->loadPage(pageIndex, buffer, bufferSize);
}

size_t Xtc::loadPageRegion(uint32_t pageIndex, size_t bitmapOffset, uint8_t* buffer, size_t length) const {
  if (!loaded || !parser) {
    return 0;
  }
  return const_cast<xtc::XtcParser*>(parser.get())->loadPageRegion(pageIndex, bitmapOffset, buffer, length);
}

xtc::XtcError Xtc::loadPageStreaming(uint32_t pageIndex,
                                     std::function<void(const uint8_t* data, size_t size, size_t offset)> callback,
                                     size_t chunkSize) const {
  if (!loaded || !parser) {
    return xtc::XtcError::FILE_NOT_FOUND;
  }
  return const_cast<xtc::XtcParser*>(parser.get())->loadPageStreaming(pageIndex, callback, chunkSize);
}

uint8_t Xtc::calculateProgress(uint32_t currentPage) const {
  if (!loaded || !parser || parser->getPageCount() == 0) {
    return 0;
  }
  return static_cast<uint8_t>((currentPage + 1) * 100 / parser->getPageCount());
}

xtc::XtcError Xtc::getLastError() const {
  if (!parser) {
    return xtc::XtcError::FILE_NOT_FOUND;
  }
  return parser->getLastError();
}
