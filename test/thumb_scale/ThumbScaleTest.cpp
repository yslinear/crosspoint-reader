// Host-side tests for the pure XTH thumbnail helpers in lib/Xtc/Xtc/ThumbScale.h.
//
// Covers two IO-free pieces extracted from Xtc::generateThumbBmp's 2-bit path:
//   1. computeSrcSpan()        - destination->source downscale index mapping
//   2. ColumnFloydSteinberg    - column-wise 1-bit error diffusion on a gray ramp
//
// No SD/XTC file IO, no device deps.

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "Xtc/ThumbScale.h"

using xtc::ColumnFloydSteinberg;
using xtc::computeSrcSpan;
using xtc::makeScaleInvFp;

namespace {

// --- computeSrcSpan ---------------------------------------------------------

TEST(ComputeSrcSpan, SpansAreNonEmptyAndInBounds) {
  const uint32_t srcExtent = 800;
  const uint32_t dstExtent = 240;  // 0.3 downscale
  const float scale = static_cast<float>(dstExtent) / srcExtent;
  const uint32_t fp = makeScaleInvFp(scale);

  for (uint32_t dst = 0; dst < dstExtent; dst++) {
    uint32_t start, end;
    computeSrcSpan(dst, fp, srcExtent, start, end);
    EXPECT_LT(start, end) << "dst=" << dst;            // non-empty
    EXPECT_LE(end, srcExtent) << "dst=" << dst;        // in bounds
    EXPECT_LT(start, srcExtent) << "dst=" << dst;
  }
}

TEST(ComputeSrcSpan, MonotonicNonDecreasingStarts) {
  // The firmware relies on monotonic source spans to advance read strips left to
  // right without re-reading. Verify start indices never go backwards.
  const uint32_t srcExtent = 480;
  const uint32_t dstExtent = 144;
  const float scale = static_cast<float>(dstExtent) / srcExtent;
  const uint32_t fp = makeScaleInvFp(scale);

  uint32_t prevStart = 0, prevEnd = 0;
  for (uint32_t dst = 0; dst < dstExtent; dst++) {
    uint32_t start, end;
    computeSrcSpan(dst, fp, srcExtent, start, end);
    EXPECT_GE(start, prevStart) << "dst=" << dst;  // starts never decrease
    EXPECT_GE(end, prevEnd) << "dst=" << dst;      // ends never decrease
    prevStart = start;
    prevEnd = end;
  }
}

TEST(ComputeSrcSpan, CoversWholeSourceRoughly) {
  // The union of all spans should reach close to the end of the source extent
  // (the last span must include the final source index region).
  const uint32_t srcExtent = 1000;
  const uint32_t dstExtent = 250;
  const float scale = static_cast<float>(dstExtent) / srcExtent;
  const uint32_t fp = makeScaleInvFp(scale);

  uint32_t start, end;
  computeSrcSpan(dstExtent - 1, fp, srcExtent, start, end);
  EXPECT_EQ(end, srcExtent);  // last span clamps to the source end
}

// --- ColumnFloydSteinberg ---------------------------------------------------

// Run the column-wise dither over a HxW image and return the produced bits.
// gray[y*W + x] is 0..255; output[y*W + x] is 0/1.
std::vector<uint8_t> ditherImage(const std::vector<uint8_t>& gray, int W, int H) {
  std::vector<int16_t> cur(H, 0), nxt(H, 0);
  ColumnFloydSteinberg fs(cur.data(), nxt.data(), H);
  std::vector<uint8_t> out(static_cast<size_t>(W) * H, 0);
  for (int x = 0; x < W; x++) {
    for (int y = 0; y < H; y++) {
      out[static_cast<size_t>(y) * W + x] = fs.processPixel(gray[static_cast<size_t>(y) * W + x], y);
    }
    fs.nextColumn();
  }
  return out;
}

TEST(ColumnFloydSteinberg, PureBlackAllZero) {
  const int W = 16, H = 16;
  std::vector<uint8_t> gray(static_cast<size_t>(W) * H, 0);  // all black
  auto out = ditherImage(gray, W, H);
  for (auto b : out) EXPECT_EQ(b, 0u);
}

TEST(ColumnFloydSteinberg, PureWhiteAllOne) {
  const int W = 16, H = 16;
  std::vector<uint8_t> gray(static_cast<size_t>(W) * H, 255);  // all white
  auto out = ditherImage(gray, W, H);
  for (auto b : out) EXPECT_EQ(b, 1u);
}

TEST(ColumnFloydSteinberg, FlatMidGrayConservesInk) {
  // A flat 50% gray field must dither to ~50% white pixels. Error diffusion
  // conserves total ink: the fraction of white bits should track the input
  // brightness closely. 128/255 ~= 0.502.
  const int W = 64, H = 64;
  std::vector<uint8_t> gray(static_cast<size_t>(W) * H, 128);
  auto out = ditherImage(gray, W, H);

  size_t white = 0;
  for (auto b : out) white += b;
  const double frac = static_cast<double>(white) / out.size();
  EXPECT_NEAR(frac, 128.0 / 255.0, 0.04);  // within 4% of the target brightness
}

TEST(ColumnFloydSteinberg, VerticalRampConservesInk) {
  // A smooth top(black)->bottom(white) vertical ramp. Error diffusion should
  // conserve total ink: mean output brightness ~= mean input brightness.
  const int W = 64, H = 256;
  std::vector<uint8_t> gray(static_cast<size_t>(W) * H);
  uint64_t inputSum = 0;
  for (int y = 0; y < H; y++) {
    const uint8_t g = static_cast<uint8_t>((y * 255) / (H - 1));
    for (int x = 0; x < W; x++) {
      gray[static_cast<size_t>(y) * W + x] = g;
      inputSum += g;
    }
  }
  auto out = ditherImage(gray, W, H);

  uint64_t white = 0;
  for (auto b : out) white += b;
  const double outFrac = static_cast<double>(white) / out.size();
  const double inFrac = static_cast<double>(inputSum) / (255.0 * out.size());
  EXPECT_NEAR(outFrac, inFrac, 0.02);  // total ink conserved within 2%
}

TEST(ColumnFloydSteinberg, RampIsNotBlocky) {
  // The whole point of error diffusion over a hard threshold: a gradient must
  // contain BOTH black and white in the mid-tone region, not a single flat
  // threshold edge. Check the middle band of a vertical ramp has a mix.
  const int W = 32, H = 256;
  std::vector<uint8_t> gray(static_cast<size_t>(W) * H);
  for (int y = 0; y < H; y++) {
    const uint8_t g = static_cast<uint8_t>((y * 255) / (H - 1));
    for (int x = 0; x < W; x++) gray[static_cast<size_t>(y) * W + x] = g;
  }
  auto out = ditherImage(gray, W, H);

  // Inspect rows 96..160 (mid gray ~95..160), expect both blacks and whites.
  size_t black = 0, white = 0;
  for (int y = 96; y < 160; y++) {
    for (int x = 0; x < W; x++) {
      if (out[static_cast<size_t>(y) * W + x])
        white++;
      else
        black++;
    }
  }
  EXPECT_GT(black, 0u);  // a plain threshold would give an all-or-nothing band
  EXPECT_GT(white, 0u);
}

}  // namespace
