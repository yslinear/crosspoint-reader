#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "EpdFont.h"
#include "EpdFontData.h"

// On-disk binary format version for .cpfont files. Defined as a preprocessor
// macro (rather than a constexpr) so it can be stringified into the SD-fonts
// release URL — see FONT_MANIFEST_URL in FontDownloadActivity.h. No integer
// suffix because stringification would include it (e.g. `4U` → `"4U"`).
//
// The canonical version for the build tooling lives in
// lib/EpdFont/scripts/cpfont_version.py. This firmware-side copy must be
// bumped manually when the firmware is updated to support a new format.
// Reader enforcement: SdCardFont::load().
#define CPFONT_VERSION 4

class SdCardFont {
 public:
  static constexpr uint16_t MAX_PAGE_GLYPHS = 512;
  static constexpr uint8_t MAX_STYLES = 4;

  // overflowCapacity: number of on-demand glyph slots in the per-instance
  // overflow ring. Default 8 keeps existing reader-body callers byte-identical.
  // The UI->CJK fallback instance passes a larger value (~64) so a long Han
  // title doesn't evict-and-refetch from SD on every e-ink redraw.
  explicit SdCardFont(uint32_t overflowCapacity = DEFAULT_OVERFLOW_CAPACITY);
  ~SdCardFont();
  // Owns raw buffers freed in dtor — no shallow-copy semantics. Make any
  // accidental pass-by-value or move a compile-time error.
  SdCardFont(const SdCardFont&) = delete;
  SdCardFont& operator=(const SdCardFont&) = delete;
  SdCardFont(SdCardFont&&) = delete;
  SdCardFont& operator=(SdCardFont&&) = delete;

  // Load .cpfont file: reads header + intervals into RAM, records file layout offsets.
  // Supports v4 (multi-style) format.
  // Returns true on success.
  bool load(const char* path);

  // Pre-read glyphs needed for the given UTF-8 text from SD card.
  // styleMask: bitmask of styles to prewarm (bit 0=regular, 1=bold, 2=italic, 3=bolditalic).
  // Default 0x0F = all present styles.
  // When metadataOnly=true, only glyph metrics are loaded (no bitmap data).
  // maxBitmapBytes: HARD byte budget for the TOTAL resident prewarm footprint
  //   across all prewarmed styles — the mini bitmap store PLUS the per-glyph
  //   metadata (miniGlyphs/miniIntervals) PLUS the mini-kern tables, all of which
  //   stay resident until the next prewarm/clearCache. Glyphs are loaded in
  //   advance-table read order, each charged its full resident cost (dataLength +
  //   sizeof(EpdGlyph) + sizeof(EpdUnicodeInterval)), until the running total
  //   would exceed this budget; the rest are skipped (their metrics still load via
  //   the miss path, so layout stays correct). Default SIZE_MAX = no cap
  //   (foreground behaviour unchanged). The background worker passes its remaining
  //   held-cache budget so its resident footprint can never exceed the cap.
  // outBitmapBytes: if non-null, receives the ACTUAL resident bytes allocated
  //   (bitmaps + per-glyph metadata + mini-kern, summed across styles). The worker
  //   charges this against its held-cache accounting so the cap binds on the real
  //   resident footprint, not a flat per-page estimate.
  // Returns number of glyphs that couldn't be loaded (0 on full success).
  int prewarm(const char* utf8Text, uint8_t styleMask = 0x0F, bool metadataOnly = false,
              size_t maxBitmapBytes = SIZE_MAX, size_t* outBitmapBytes = nullptr);

  // Build a compact advance-only table for layout measurement.
  // Extracts ALL unique codepoints from words (no MAX_PAGE_GLYPHS cap),
  // batch-reads advanceX from SD, stores in a sorted per-style table.
  // Returns number of codepoints not found in font coverage.
  int buildAdvanceTable(const char* utf8Text, uint8_t styleMask = 0x0F);
  int buildAdvanceTable(const std::vector<std::string>& words, bool includeHyphen, uint8_t styleMask = 0x0F);

  // Look up advanceX for a codepoint from the advance table.
  // Returns the 12.4 fixed-point advance, or 0 if not found.
  uint16_t getAdvance(uint32_t codepoint, uint8_t style) const;

  // Advance-ONLY resolution for measurement (UI->CJK fallback). Returns the
  // 12.4 fixed-point advanceX for `codepoint`, fetching it from SD if it is not
  // already in the advance cache — but NEVER loading the glyph bitmap (unlike
  // tryGetGlyph(), which loads the full glyph into the overflow ring). This is
  // what makes CJK file-list width measurement fast: each codepoint hits SD at
  // most once (the result is written back into the advance cache), and only the
  // 8-byte advanceX is read, not the bitmap.
  //
  // Parity contract with resolveGlyph()'s tier-2 (fallback) branch: on a coverage
  // hit, the returned advance is byte-identical to fallback.tryGetGlyph(cp)->
  // advanceX, because both read advanceX from the same EpdGlyph at the same file
  // offset. On a TOTAL miss (the fallback font does not cover the codepoint at
  // all — findGlobalGlyphIndex < 0) `found` is set false and the return value is
  // meaningless; the caller must then fall back to the PRIMARY font's replacement
  // glyph advance (resolveGlyph tier-3), NOT this font's own replacement. On a
  // covered codepoint `found` is true.
  //
  // Cache-full behaviour: even when the advance cache is full (so the fetched
  // value cannot be written back), the one-shot advance-only SD read is still
  // performed and the real advanceX is returned — never 0.
  //
  // fetchedFromSd: optional write-back deferral. When null (default), a freshly
  // read advance is written back into the cache immediately via a single-entry
  // merge — convenient for one-off measurement (getTextWidth). When non-null, the
  // immediate write-back is SKIPPED and *fetchedFromSd is set true iff the value
  // came from a fresh SD read (false on a cache hit / total miss / read failure),
  // so a hot caller (truncatedText) can stage all misses for ONE batched
  // cacheAdvances() merge instead of paying a per-codepoint table realloc.
  uint16_t getAdvanceOrFetch(uint32_t codepoint, uint8_t style, bool& found, bool* fetchedFromSd = nullptr);

  // Public mirror of the private AdvanceEntry so callers can stage a sorted
  // batch for cacheAdvances() without reaching into private state.
  struct AdvanceEntryPublic {
    uint32_t codepoint;
    uint16_t advanceX;  // 12.4 fixed-point
  };

  // Persist just-fetched advances into the per-style table.
  // Used by GfxRenderer::getTextAdvanceX to write back advances that were read
  // from SD via the glyph-miss path (instead of discarding them), so the same
  // CJK glyph is read from SD at most once per font lifetime. Merges N
  // pre-sorted (by codepoint) entries in one allocation to avoid per-codepoint
  // realloc churn in the layout hot loop.
  void cacheAdvances(uint8_t style, const AdvanceEntryPublic* sortedNew, uint32_t newCount);

  // Returns true if advance table is populated for at least one style.
  bool hasAdvanceTable() const;

  // Free mini data for all styles and restore stub EpdFontData. Also wipes the
  // on-demand overflow ring. Preserves the persistent advance cache so repeated
  // layout passes can reuse previously fetched metrics.
  void clearCache();

  // Free per-page prewarm mini-data ONLY, preserving the on-demand overflow ring
  // (shared UI/file-browser CJK glyph cache) and the advance cache. Use on a
  // reader exit where the prewarm must be released but the UI cache must survive.
  void clearPrewarm();

  // Drop the persistent advance cache. Call when unloading the SD font or
  // when font/size/family/glyph-table state changes.
  void clearPersistentCache();

  // Returns pointer to the managed EpdFont for a given style.
  // Returns nullptr if the style is not present.
  EpdFont* getEpdFont(uint8_t style = 0);

  // Returns true if the given style is present in this font file.
  bool hasStyle(uint8_t style) const;

  // Resolve requested style bits to the closest present style.
  uint8_t resolveStyle(uint8_t style) const;

  // Resolve a style the SAME way EpdFontFamily::getFont() does (bold/italic bits
  // only; bold&italic -> boldItalic|bold|italic, else regular), so an advance read
  // here lands on the same per-style EpdFont that the fallback family's
  // tryGetGlyph(cp, style) would have used. Used by getAdvanceOrFetch() and by the
  // measurement caller that batches its write-back via cacheAdvances(), so both
  // target the identical style index. Differs from resolveStyle(), which has a
  // richer (re-ordered) fallback chain not matched by the family.
  uint8_t resolveFamilyStyle(uint8_t style) const;

  // Resolve every requested style bit through fallback and return the actual
  // styles that need cache/advance preparation.
  uint8_t resolveStyleMask(uint8_t styleMask) const;

  // Number of styles present in this font file.
  uint8_t styleCount() const { return styleCount_; }

  // Returns true if the glyph pointer points into the overflow buffer.
  bool isOverflowGlyph(const EpdGlyph* glyph) const;

  // Returns the bitmap for an on-demand-loaded (overflow) glyph.
  const uint8_t* getOverflowBitmap(const EpdGlyph* glyph) const;

  // Extract SdCardFont* from an opaque glyphMissCtx pointer.
  // Used by GfxRenderer::getGlyphBitmap() to recover the SdCardFont from EpdFontData::glyphMissCtx.
  static SdCardFont* fromMissCtx(void* ctx);

  struct Stats {
    uint32_t prewarmTotalMs = 0;
    uint32_t sdReadTimeMs = 0;
    uint32_t seekCount = 0;
    uint32_t uniqueGlyphs = 0;
    uint32_t bitmapBytes = 0;
  };
  void logStats(const char* label = "SDCF");
  void resetStats();
  const Stats& getStats() const { return stats_; }

  // Content hash of the file header + style TOC entries (computed during load).
  // Used to generate deterministic font IDs for section cache invalidation.
  uint32_t contentHash() const { return contentHash_; }

 private:
  // Per-style metadata (parsed from file header/TOC)
  struct CpFontHeader {
    uint32_t intervalCount = 0;
    uint32_t glyphCount = 0;
    uint8_t advanceY = 0;
    int16_t ascender = 0;
    int16_t descender = 0;
    bool is2Bit = false;
    uint16_t kernLeftEntryCount = 0;
    uint16_t kernRightEntryCount = 0;
    uint8_t kernLeftClassCount = 0;
    uint8_t kernRightClassCount = 0;
    uint8_t ligaturePairCount = 0;
  };

  // All per-style data: file offsets, intervals, kern/lig, prewarm cache, EpdFont
  struct PerStyle {
    CpFontHeader header{};

    // File layout offsets for this style's data sections
    uint32_t intervalsFileOffset = 0;
    uint32_t glyphsFileOffset = 0;
    uint32_t kernLeftFileOffset = 0;
    uint32_t kernRightFileOffset = 0;
    uint32_t kernMatrixFileOffset = 0;
    uint32_t ligatureFileOffset = 0;
    uint32_t bitmapFileOffset = 0;

    // Full intervals loaded from file (kept in RAM for codepoint lookup)
    EpdUnicodeInterval* fullIntervals = nullptr;
    struct BmpInterval16 {
      uint16_t first;
      uint16_t last;
      uint16_t offset;
    } __attribute__((packed));
    static_assert(sizeof(BmpInterval16) == 6, "BmpInterval16 must remain compact");
    BmpInterval16* bmpIntervals = nullptr;
    bool intervalsAreBmp16 = false;

    // Persistent kern-class + ligature tables (lazy-loaded on first prewarm).
    // The full kern MATRIX is NOT resident — on Literata-class fonts a single
    // style's matrix is ~36-42KB contiguous, and 4 styles' worth won't fit
    // alongside bitmaps + framebuffer on a 380KB device. Only kernLeftClasses
    // and kernRightClasses (small codepoint→classId tables, ~3KB each) stay
    // resident; the matrix is reconstructed per-page as miniKernMatrix.
    EpdKernClassEntry* kernLeftClasses = nullptr;
    EpdKernClassEntry* kernRightClasses = nullptr;
    EpdLigaturePair* ligaturePairs = nullptr;
    bool kernLigLoaded = false;

    // Stub EpdFontData returned when not prewarmed
    EpdFontData stubData{};

    // Mini EpdFontData built during prewarm
    EpdFontData miniData{};
    EpdUnicodeInterval* miniIntervals = nullptr;
    EpdGlyph* miniGlyphs = nullptr;
    uint8_t* miniBitmap = nullptr;
    uint32_t miniIntervalCount = 0;
    uint32_t miniGlyphCount = 0;

    // Per-page mini kern matrix (built by buildMiniKernMatrix on each full
    // prewarm). miniKernLeftClasses/miniKernRightClasses map ONLY the codepoints
    // used on the current page to renumbered class IDs (1..miniKern*ClassCount).
    // miniKernMatrix is a small miniKernLeftClassCount × miniKernRightClassCount
    // flat matrix. Typical Latin page: ~25×25 matrix = ~625 bytes per style vs
    // ~36KB for the full Literata matrix — ~50× reduction.
    EpdKernClassEntry* miniKernLeftClasses = nullptr;
    EpdKernClassEntry* miniKernRightClasses = nullptr;
    uint16_t miniKernLeftEntryCount = 0;
    uint16_t miniKernRightEntryCount = 0;
    uint8_t miniKernLeftClassCount = 0;
    uint8_t miniKernRightClassCount = 0;
    int8_t* miniKernMatrix = nullptr;

    // The EpdFont whose data pointer we manage
    EpdFont epdFont{&stubData};

    bool present = false;
  };

  PerStyle styles_[MAX_STYLES] = {};
  uint8_t styleCount_ = 0;

  char filePath_[128] = {};

  // Overflow context: glyphMissHandler needs to know which style it's serving
  struct OverflowContext {
    SdCardFont* self;
    uint8_t styleIdx;
  };
  OverflowContext overflowCtx_[MAX_STYLES] = {};

  // On-demand overflow buffer (ring buffer of glyphs loaded via glyphMissHandler).
  // Capacity is per-instance (overflowCapacity_, set in the constructor) so the
  // UI fallback can size a larger ring without inflating reader-body fonts.
  static constexpr uint32_t DEFAULT_OVERFLOW_CAPACITY = 8;
  struct OverflowEntry {
    EpdGlyph glyph;
    uint8_t* bitmap = nullptr;
    uint32_t codepoint = 0;
    uint8_t styleIdx = 0;
  };
  // Heap-allocated ring of overflowCapacity_ entries (allocated in the
  // constructor, freed in the destructor). Sized once at construction; a single
  // OverflowEntry is 24 bytes, so the default 8-slot ring is 192 bytes and a
  // 64-slot UI-fallback ring is ~1.5 KB — charged only when the fallback loads.
  OverflowEntry* overflow_ = nullptr;
  uint32_t overflowCapacity_ = DEFAULT_OVERFLOW_CAPACITY;
  uint32_t overflowCount_ = 0;
  uint32_t overflowNext_ = 0;

  // Compact advance-only table for layout measurement (per-style).
  // Built by buildAdvanceTable(), queried by getAdvance().
  struct AdvanceEntry {
    uint32_t codepoint;
    uint16_t advanceX;  // 12.4 fixed-point
  };
  // Per-style advance table. Sorted by codepoint for binary lookup.
  // Bounded to a per-style cap (advanceCacheLimit_); persists across layout
  // passes (across calls to clearCache()) so repeated indexing of the same font
  // amortizes SD reads. Cleared only on font unload or clearPersistentCache().
  //
  // Cap rationale (380KB RAM ceiling): AdvanceEntry is 8 bytes (unpacked), so a
  // table costs cap*8 bytes per style. The Latin default keeps the historic
  // ~6KB/style. CJK fonts have thousands of glyphs, so a 768-entry cap thrashes
  // SD on every layout pass; we grow ONLY the regular style (index 0) to the
  // CJK cap (~24KB) and leave bold/italic/bold-italic at the default. Worst case
  // for a 4-style CJK reader font: 24KB + 3*6KB = ~42KB, gated to CJK fonts
  // only (detected in load() via glyphCount). See load() for detection.
  static constexpr uint32_t kAdvanceCacheLimitDefault = 768;     // ~6KB/style
  static constexpr uint32_t kAdvanceCacheLimitCjk = 3072;        // ~24KB/style
  static constexpr uint32_t kCjkGlyphCountThreshold = 2000;      // > Latin, < CJK
  uint32_t advanceCacheLimit_[MAX_STYLES] = {kAdvanceCacheLimitDefault, kAdvanceCacheLimitDefault,
                                             kAdvanceCacheLimitDefault, kAdvanceCacheLimitDefault};
  AdvanceEntry* advanceTable_[MAX_STYLES] = {};
  uint32_t advanceTableSize_[MAX_STYLES] = {};
  bool advanceTableLookup(uint8_t styleIdx, uint32_t codepoint, uint16_t* outAdvance) const;
  // Merge sortedNew (sorted by codepoint, no overlap with existing) into the
  // advance table for styleIdx, preserving sort order; cap-truncates the tail.
  void mergeIntoAdvanceTable(uint8_t styleIdx, const AdvanceEntry* sortedNew, uint32_t newCount);

  Stats stats_;
  uint32_t contentHash_ = 0;
  bool loaded_ = false;

  // Per-style helpers
  void freeStyleMiniData(PerStyle& s);
  void freeStyleAll(PerStyle& s);
  void freeStyleKernLigatureData(PerStyle& s);
  void freeStyleMiniKern(PerStyle& s);
  bool loadStyleKernLigatureData(PerStyle& s);
  bool buildMiniKernMatrix(PerStyle& s, const uint32_t* codepoints, uint32_t cpCount);
  void applyKernLigaturePointers(PerStyle& s, EpdFontData& data) const;
  void applyGlyphMissCallback(uint8_t styleIdx);
  int32_t findGlobalGlyphIndex(const PerStyle& s, uint32_t codepoint) const;
  int fetchAdvancesForCodepoints(uint32_t* codepoints, uint32_t cpCount, uint8_t styleMask);
  template <typename Iter>
  int buildAdvanceTableRange(Iter begin, Iter end, bool includeSpace, bool includeHyphen, uint8_t styleMask);
  // remainingBitmapBytes (in/out): the bitmap byte budget still available for
  // this style. prewarmStyle loads glyph bitmaps until the running total would
  // exceed it, then truncates; on return it is decremented by the bytes this
  // style actually allocated. Glyph METRICS are always loaded in full (layout
  // correctness), only BITMAP loading is budget-gated.
  int prewarmStyle(uint8_t styleIdx, const uint32_t* codepoints, uint32_t cpCount, bool metadataOnly,
                   size_t& remainingBitmapBytes);

  // Global helpers
  void freeAll();
  void clearOverflow();
  static void computeStyleFileOffsets(PerStyle& s, uint32_t baseOffset);

  // Static callback for EpdFontData::glyphMissHandler (per-style via OverflowContext)
  static const EpdGlyph* onGlyphMiss(void* ctx, uint32_t codepoint);
};
