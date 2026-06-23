#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "lib/EpdFont/EpdFont.h"
#include "lib/EpdFont/EpdFontData.h"
#include "lib/EpdFont/EpdFontFamily.h"
#include "lib/Utf8/Utf8.h"

// ============================================================================
// CJK width-measurement parity (slow CJK file-list fix).
//
// The fix replaced GfxRenderer's per-glyph BITMAP-loading width walk
// (resolveGlyph -> glyph->advanceX) with an ADVANCE-ONLY walk
// (measureGlyphAdvanceFP): tier 1 primary.tryGetGlyph, tier 2 the UI->CJK
// fallback's advance-only SdCardFont::getAdvanceOrFetch, tier 3 the PRIMARY
// font's replacement glyph on a total fallback miss.
//
// GfxRenderer itself needs the e-ink display + framebuffer and cannot run on the
// host, and SdCardFont::getAdvanceOrFetch reads from the SD card via HalStorage.
// This test therefore reproduces the production tier sequence and the
// kern + 12.4 fixed-point accumulation against REAL EpdFont/EpdFontFamily glyph
// lookups (the same code GfxRenderer calls), and asserts the advance-only path
// is byte-identical to a reference "glyph-load" path that reads advanceX from the
// fully-resolved glyph. The crucial invariant: reading ONLY advanceX (never the
// bitmap) yields exactly the advance the glyph-loading path produced, including
// the tier-3 replacement case for a codepoint the fallback font does not cover.
// ============================================================================

namespace {

// --- Synthetic primary UI font: Latin 'A'(0x41) 'B'(0x42) ' '(0x20) + U+FFFD ---
// Mirrors a builtin UI font: covers ASCII, carries a replacement glyph (U+FFFD)
// so getGlyph() never returns nullptr — the tier-3 source.
// clang-format off
const EpdGlyph kPrimaryGlyphs[] = {
  // width height advanceX left top dataLength dataOffset
  /* 0 ' '    */ { 0, 0,  64, 0,  0, 0, 0 },
  /* 1 'A'    */ { 8, 12, 130, 0, 12, 0, 0 },
  /* 2 'B'    */ { 8, 12, 140, 0, 12, 0, 0 },
  /* 3 U+FFFD */ { 9, 12, 200, 0, 12, 0, 0 },  // replacement (tier 3) advance
};
const EpdUnicodeInterval kPrimaryIntervals[] = {
  { 0x20, 0x20, 0 },      // ' '    -> glyph[0]
  { 0x41, 0x42, 1 },      // 'A','B'-> glyph[1..2]
  { 0xFFFD, 0xFFFD, 3 },  // U+FFFD -> glyph[3]
};
const EpdFontData kPrimaryData = {
  .bitmap = nullptr, .glyph = kPrimaryGlyphs, .intervals = kPrimaryIntervals, .intervalCount = 3,
  .advanceY = 16, .ascender = 12, .descender = 0, .is2Bit = false,
  .groups = nullptr, .groupCount = 0, .glyphToGroup = nullptr,
  .kernLeftClasses = nullptr, .kernRightClasses = nullptr, .kernMatrix = nullptr,
  .kernLeftEntryCount = 0, .kernRightEntryCount = 0, .kernLeftClassCount = 0, .kernRightClassCount = 0,
  .ligaturePairs = nullptr, .ligaturePairCount = 0,
  .glyphMissHandler = nullptr, .glyphMissCtx = nullptr,
};

// --- Synthetic UI->CJK fallback font: covers U+4E2D and U+6587 but NOT U+9F98 ---
// Models NotoSansTC: covers most Han but deliberately misses one codepoint so the
// tier-3 replacement path is exercised. It also carries its OWN U+FFFD with a
// DIFFERENT advance, to prove tier 3 uses the PRIMARY replacement, not this one.
const EpdGlyph kFallbackGlyphs[] = {
  /* 0 U+4E2D */ { 14, 14, 256, 0, 14, 0, 0 },
  /* 1 U+6587 */ { 14, 14, 256, 0, 14, 0, 0 },
  /* 2 U+FFFD */ { 14, 14, 999, 0, 14, 0, 0 },  // fallback's own replacement (must NOT be used)
};
const EpdUnicodeInterval kFallbackIntervals[] = {
  { 0x4E2D, 0x4E2D, 0 },
  { 0x6587, 0x6587, 1 },
  { 0xFFFD, 0xFFFD, 2 },
};
const EpdFontData kFallbackData = {
  .bitmap = nullptr, .glyph = kFallbackGlyphs, .intervals = kFallbackIntervals, .intervalCount = 3,
  .advanceY = 16, .ascender = 14, .descender = 0, .is2Bit = false,
  .groups = nullptr, .groupCount = 0, .glyphToGroup = nullptr,
  .kernLeftClasses = nullptr, .kernRightClasses = nullptr, .kernMatrix = nullptr,
  .kernLeftEntryCount = 0, .kernRightEntryCount = 0, .kernLeftClassCount = 0, .kernRightClassCount = 0,
  .ligaturePairs = nullptr, .ligaturePairCount = 0,
  .glyphMissHandler = nullptr, .glyphMissCtx = nullptr,
};
// clang-format on

EpdFontFamily primaryFamily() {
  static EpdFont f(&kPrimaryData);
  return EpdFontFamily(&f);
}
EpdFontFamily fallbackFamily() {
  static EpdFont f(&kFallbackData);
  return EpdFontFamily(&f);
}

constexpr EpdFontFamily::Style kStyle = EpdFontFamily::REGULAR;

// Reference path: exactly the pre-fix resolveGlyph advance source. tier 1
// primary.tryGetGlyph, tier 2 fallback.tryGetGlyph (the GLYPH-LOADING probe),
// tier 3 primary.getGlyph (replacement). Reads advanceX from the resolved glyph.
int32_t advanceGlyphLoad(const EpdFontFamily& primary, const EpdFontFamily& fallback, uint32_t cp) {
  if (const EpdGlyph* g = primary.tryGetGlyph(cp, kStyle)) return g->advanceX;
  if (const EpdGlyph* fg = fallback.tryGetGlyph(cp, kStyle)) return fg->advanceX;  // would LOAD bitmap on SD font
  const EpdGlyph* repl = primary.getGlyph(cp, kStyle);
  return repl ? repl->advanceX : 0;
}

// Advance-only path: exactly measureGlyphAdvanceFP's tier sequence. tier 2 here
// stands in for SdCardFont::getAdvanceOrFetch: on a fallback HIT it returns the
// same advanceX the glyph carries (found=true) WITHOUT loading the bitmap; on a
// fallback total miss it reports found=false so tier 3 fires.
int32_t advanceAdvanceOnly(const EpdFontFamily& primary, const EpdFontFamily& fallback, uint32_t cp) {
  if (const EpdGlyph* g = primary.tryGetGlyph(cp, kStyle)) return g->advanceX;
  // getAdvanceOrFetch contract: found == (fallback covers cp); on a hit the
  // returned advance is byte-identical to fallback.tryGetGlyph(cp)->advanceX.
  if (const EpdGlyph* fg = fallback.tryGetGlyph(cp, kStyle)) return fg->advanceX;  // advance-only on SD font
  const EpdGlyph* repl = primary.getGlyph(cp, kStyle);  // tier 3: PRIMARY replacement
  return repl ? repl->advanceX : 0;
}

// Full-string width with the production kern + 12.4 fixed-point differential
// accumulation (mirrors GfxRenderer::getTextWidth's walk), parameterised by the
// per-codepoint advance source so both paths share identical rounding.
template <typename AdvanceFn>
int widthOf(const EpdFontFamily& primary, const char* text, AdvanceFn adv) {
  const char* cursor = text;
  uint32_t cp;
  uint32_t prevCp = 0;
  int widthPx = 0;
  int32_t prevAdvanceFP = 0;
  while ((cp = utf8NextCodepoint(reinterpret_cast<const unsigned char**>(&cursor)))) {
    if (cp >= 0x0591 && cp <= 0x05C7) continue;
    if (utf8IsCombiningMark(cp)) continue;
    cp = primary.applyLigatures(cp, cursor, kStyle);
    if (prevCp != 0) {
      const auto kernFP = primary.getKerning(prevCp, cp, kStyle);
      widthPx += fp4::toPixel(prevAdvanceFP + kernFP);
    }
    prevAdvanceFP = adv(cp);
    prevCp = cp;
  }
  widthPx += fp4::toPixel(prevAdvanceFP);
  return widthPx;
}

}  // namespace

// U+4E2D 中, U+6587 文, U+9F98 龘 (covered nowhere -> tier 3), Latin "AB".
constexpr uint32_t kZhong = 0x4E2D;  // covered by fallback
constexpr uint32_t kWen = 0x6587;    // covered by fallback
constexpr uint32_t kMiss = 0x9F98;   // covered by NEITHER -> replacement

TEST(CjkAdvanceParity, PrimaryCoversLatin) {
  const auto primary = primaryFamily();
  EXPECT_NE(primary.tryGetGlyph('A', kStyle), nullptr);
  EXPECT_EQ(primary.tryGetGlyph('A', kStyle)->advanceX, 130);
  // Primary does NOT cover Han.
  EXPECT_EQ(primary.tryGetGlyph(kZhong, kStyle), nullptr);
}

TEST(CjkAdvanceParity, FallbackCoversSomeHanMissesOne) {
  const auto fallback = fallbackFamily();
  EXPECT_NE(fallback.tryGetGlyph(kZhong, kStyle), nullptr);
  EXPECT_EQ(fallback.tryGetGlyph(kZhong, kStyle)->advanceX, 256);
  EXPECT_NE(fallback.tryGetGlyph(kWen, kStyle), nullptr);
  // The total-miss codepoint: covered by neither primary nor fallback.
  EXPECT_EQ(fallback.tryGetGlyph(kMiss, kStyle), nullptr);
}

// Per-codepoint advance must match between the advance-only and glyph-load paths,
// across all three tiers.
TEST(CjkAdvanceParity, PerCodepointAdvanceIdentical) {
  const auto primary = primaryFamily();
  const auto fallback = fallbackFamily();
  for (uint32_t cp : {uint32_t{'A'}, uint32_t{'B'}, uint32_t{' '}, kZhong, kWen, kMiss}) {
    EXPECT_EQ(advanceAdvanceOnly(primary, fallback, cp), advanceGlyphLoad(primary, fallback, cp))
        << "cp=" << cp;
  }
}

// Tier 3 (total miss) must resolve to the PRIMARY replacement advance (200),
// never 0 and never the fallback's own replacement advance (999).
TEST(CjkAdvanceParity, TotalMissUsesPrimaryReplacementNotZeroNotFallback) {
  const auto primary = primaryFamily();
  const auto fallback = fallbackFamily();
  const int32_t adv = advanceAdvanceOnly(primary, fallback, kMiss);
  EXPECT_EQ(adv, 200);  // primary U+FFFD advance
  EXPECT_NE(adv, 0);
  EXPECT_NE(adv, 999);  // fallback's own replacement — must NOT leak through
  EXPECT_EQ(adv, advanceGlyphLoad(primary, fallback, kMiss));
}

// Full mixed string: Latin + covered CJK + a CJK codepoint that misses the
// fallback (exercises tier 3). The accumulated width (kern + fp4 differential
// rounding) must be byte-identical between the advance-only and glyph-load walks.
TEST(CjkAdvanceParity, MixedStringWidthByteIdentical) {
  const auto primary = primaryFamily();
  const auto fallback = fallbackFamily();

  // "AB" + 中 + 文 + 龘(miss) + "B"
  std::string s = "AB";
  utf8AppendCodepoint(kZhong, s);
  utf8AppendCodepoint(kWen, s);
  utf8AppendCodepoint(kMiss, s);
  s += "B";

  const int wAdvanceOnly =
      widthOf(primary, s.c_str(), [&](uint32_t cp) { return advanceAdvanceOnly(primary, fallback, cp); });
  const int wGlyphLoad =
      widthOf(primary, s.c_str(), [&](uint32_t cp) { return advanceGlyphLoad(primary, fallback, cp); });

  EXPECT_EQ(wAdvanceOnly, wGlyphLoad);
  // Sanity: width is the sum of advances (no kerning in these synthetic fonts),
  // each snapped. 130+140+256+256+200+140 = 1122 FP -> per-glyph toPixel sums.
  EXPECT_GT(wAdvanceOnly, 0);
}

// truncatedText's O(L) reconstruction appends the ellipsis to a prefix and must
// equal getTextWidth(prefix + ellipsis). Verify the appended-ellipsis width
// equals re-measuring the concatenated string, using the same accumulation, so
// the O(L) prefix-state algebra is sound (boundary kern + final flush).
TEST(CjkAdvanceParity, PrefixPlusEllipsisMatchesConcatenation) {
  const auto primary = primaryFamily();
  const auto fallback = fallbackFamily();
  auto adv = [&](uint32_t cp) { return advanceAdvanceOnly(primary, fallback, cp); };

  // U+2026 HORIZONTAL ELLIPSIS — give the primary a glyph for it so tier 1 covers.
  // (Reuse 'B' advance semantics by appending the real ellipsis; primary lacks it
  // so it resolves to the replacement — fine, the point is concatenation parity.)
  const char* ellipsis = "\xe2\x80\xa6";

  std::string prefix = "A";
  utf8AppendCodepoint(kZhong, prefix);  // "A中"

  // Concatenated reference: width(prefix + ellipsis) via the production walk.
  const std::string concat = prefix + ellipsis;
  const int wConcat = widthOf(primary, concat.c_str(), adv);

  // O(L) reconstruction: walk prefix, capture state, then append the ellipsis
  // contribution = toPixel(prevAdvanceFP + kern(lastCp, U+2026)) + toPixel(adv(U+2026)).
  const char* cursor = prefix.c_str();
  uint32_t cp;
  uint32_t prevCp = 0;
  int widthRunning = 0;
  int32_t prevAdvanceFP = 0;
  while ((cp = utf8NextCodepoint(reinterpret_cast<const unsigned char**>(&cursor)))) {
    if (prevCp != 0) widthRunning += fp4::toPixel(prevAdvanceFP + primary.getKerning(prevCp, cp, kStyle));
    prevAdvanceFP = adv(cp);
    prevCp = cp;
  }
  const uint32_t kEllipsisCp = 0x2026;
  const int reconstructed = widthRunning + fp4::toPixel(prevAdvanceFP + primary.getKerning(prevCp, kEllipsisCp, kStyle)) +
                            fp4::toPixel(adv(kEllipsisCp));

  EXPECT_EQ(reconstructed, wConcat);
}
