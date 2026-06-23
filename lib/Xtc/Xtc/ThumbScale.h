/**
 * ThumbScale.h
 *
 * Pure, IO-free helpers for XTH (2-bit) cover-thumbnail generation.
 *
 * Two concerns are extracted here so they can be unit-tested on the host
 * without any SD/XTC file IO (see test/thumb_scale):
 *
 *  1. computeSrcSpan() - the destination-index -> source-index downscale
 *     mapping used by the area-averaging downscaler. Fixed-point (16.16),
 *     identical math for both the X (column) and Y (row) axes.
 *
 *  2. ColumnFloydSteinberg - 1-bit Floyd-Steinberg error diffusion run in
 *     COLUMN-MAJOR traversal order. The thumbnail path produces output one
 *     column at a time (because the XTH source is column-major and is read as
 *     contiguous vertical strips), so a column-wise diffusion is the natural
 *     fit: error propagates down within a column (toward the next row) and
 *     rightward to the next column. This is a 90-degree-rotated Floyd-Steinberg
 *     and preserves the same total-ink / error-conservation invariants the
 *     row-major variant has, so gradients dither smoothly instead of blocking.
 *
 * Header-only and dependency-free (only <cstdint>/<cstring>) so the firmware
 * and the host test share one definition.
 */

#pragma once

#include <cstdint>
#include <cstring>

namespace xtc {

// Inverse-scale fixed-point shift (16.16). scaleInv_fp = round(65536 / scale).
inline uint32_t makeScaleInvFp(float scale) { return static_cast<uint32_t>(65536.0f / scale); }

// Map destination index `dst` to its covered source-index half-open span
// [start, end) for area-averaging downscale. `srcExtent` is the source axis
// length (width or height). Guarantees start < end and end <= srcExtent for any
// 0 <= dst < dstExtent when scaleInv_fp corresponds to a >1 downscale factor.
inline void computeSrcSpan(uint32_t dst, uint32_t scaleInv_fp, uint32_t srcExtent, uint32_t& start, uint32_t& end) {
  start = (dst * scaleInv_fp) >> 16;
  end = ((dst + 1) * scaleInv_fp) >> 16;
  if (start >= srcExtent) start = srcExtent - 1;
  if (end > srcExtent) end = srcExtent;
  if (end <= start) end = start + 1;
  if (end > srcExtent) end = srcExtent;
}

// Column-major 1-bit Floyd-Steinberg error diffusion.
//
// The image is produced column by column, top to bottom within each column.
// For pixel (x, y) the rotated FS kernel distributes the quantization error as:
//
//        down (y+1)         : 7/16   (in-column, like FS "right")
//        next col (x+1):
//            up   (y-1)      : 3/16
//            same (y)        : 5/16
//            down (y+1)      : 1/16
//
// which is the standard FS kernel rotated 90 degrees to match column traversal.
// `height` int16 of error is carried for the current column (residual into the
// next pixel down) and for the next column. Two such buffers, swapped per
// column. Memory: 2 * height * 2 bytes (~1.6KB for a 400-tall thumb).
//
// Quantizes to 1-bit: returns 1 for white, 0 for black. Threshold at 128.
class ColumnFloydSteinberg {
 public:
  // Bind to caller-provided scratch. curCol and nextCol must each hold `height`
  // int16_t and be zero-initialised. No allocation here (the caller owns the
  // buffers via makeUniqueNoThrow), so this stays IO/alloc-free and testable.
  ColumnFloydSteinberg(int16_t* curCol, int16_t* nextCol, int height)
      : curCol(curCol), nextCol(nextCol), height(height) {
    std::memset(curCol, 0, static_cast<size_t>(height) * sizeof(int16_t));
    std::memset(nextCol, 0, static_cast<size_t>(height) * sizeof(int16_t));
  }

  // Process one pixel at row y of the current column. `gray` is 0..255.
  // Returns the 1-bit value (1 = white, 0 = black).
  uint8_t processPixel(int gray, int y) {
    int adjusted = gray + curCol[y];
    if (adjusted < 0) adjusted = 0;
    if (adjusted > 255) adjusted = 255;

    uint8_t bit;
    int quantizedValue;
    if (adjusted < 128) {
      bit = 0;
      quantizedValue = 0;
    } else {
      bit = 1;
      quantizedValue = 255;
    }

    const int error = adjusted - quantizedValue;

    // In-column: down (y+1) gets 7/16.
    if (y + 1 < height) curCol[y + 1] += static_cast<int16_t>((error * 7) >> 4);
    // Next column: up 3/16, same 5/16, down 1/16.
    if (y - 1 >= 0) nextCol[y - 1] += static_cast<int16_t>((error * 3) >> 4);
    nextCol[y] += static_cast<int16_t>((error * 5) >> 4);
    if (y + 1 < height) nextCol[y + 1] += static_cast<int16_t>((error) >> 4);

    return bit;
  }

  // Advance to the next column: nextCol becomes curCol, clear the new nextCol.
  void nextColumn() {
    int16_t* tmp = curCol;
    curCol = nextCol;
    nextCol = tmp;
    std::memset(nextCol, 0, static_cast<size_t>(height) * sizeof(int16_t));
  }

 private:
  int16_t* curCol;
  int16_t* nextCol;
  int height;
};

}  // namespace xtc
