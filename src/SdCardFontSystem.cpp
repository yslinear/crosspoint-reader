#include "SdCardFontSystem.h"

#include <EpdFontFamily.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <Memory.h>
#include <SdCardFont.h>

#include "CrossPointSettings.h"

namespace {

static uint8_t fontSizeEnumFromSettings() {
  uint8_t e = SETTINGS.fontSize;
  if (e >= CrossPointSettings::FONT_SIZE_COUNT) e = 1;  // default to MEDIUM
  return e;
}

}  // namespace

// ctor/dtor defined here (not =default in the header) so the unique_ptr<SdCardFont>
// member can see the complete SdCardFont type for its deleter.
SdCardFontSystem::SdCardFontSystem() = default;
SdCardFontSystem::~SdCardFontSystem() = default;

void SdCardFontSystem::begin(GfxRenderer& renderer) {
  registry_.discover();

  // Register this system as the SD font ID resolver in settings.
  // Uses a static trampoline since CrossPointSettings stores a plain function pointer.
  SETTINGS.sdFontIdResolver = [](void* ctx, const char* familyName, uint8_t fontSizeEnum) -> int {
    return static_cast<SdCardFontSystem*>(ctx)->resolveFontId(familyName, fontSizeEnum);
  };
  SETTINGS.sdFontResolverCtx = this;

  // If user has a saved SD font selection, load it
  if (SETTINGS.sdFontFamilyName[0] != '\0') {
    const auto* family = registry_.findFamily(SETTINGS.sdFontFamilyName);
    if (family) {
      if (manager_.loadFamily(*family, renderer, fontSizeEnumFromSettings())) {
        LOG_DBG("SDFS", "Loaded SD card font family: %s", SETTINGS.sdFontFamilyName);
      } else {
        LOG_ERR("SDFS", "Failed to load SD font family: %s (clearing)", SETTINGS.sdFontFamilyName);
        SETTINGS.sdFontFamilyName[0] = '\0';
      }
    } else {
      LOG_DBG("SDFS", "SD font family not found on card: %s (clearing)", SETTINGS.sdFontFamilyName);
      SETTINGS.sdFontFamilyName[0] = '\0';
    }
  }

  // Load the CJK UI-fallback after the reader font so the renderer can resolve
  // Han glyphs the built-in UI fonts lack. Gated on file presence inside.
  loadUiFallback(renderer);

  LOG_DBG("SDFS", "SD font system ready (%d families discovered)", registry_.getFamilyCount());
}

void SdCardFontSystem::loadUiFallback(GfxRenderer& renderer) {
  // Exact 12px NotoSansTC only. A missing family/size leaves uiFallbackFontId_
  // at 0, so non-CJK users pay zero RAM (no SdCardFont, no overflow ring).
  static constexpr char kFallbackFamily[] = "NotoSansTC";
  static constexpr uint8_t kFallbackPointSize = 12;
  // ~64-slot overflow ring: a long Han title (book/chapter names) can touch
  // dozens of unique glyphs in one redraw. The default 8-slot ring would
  // evict-and-refetch from SD on every e-ink update; 64 covers a typical title
  // in one pass. ~24 bytes/slot => ~1.5 KB, charged only when CJK is installed.
  static constexpr uint32_t kFallbackOverflowCapacity = 64;

  const auto* family = registry_.findFamily(kFallbackFamily);
  if (!family) return;
  const auto* file = family->findFile(kFallbackPointSize);
  if (!file) {
    LOG_DBG("SDFS", "UI fallback %s present but no %upx file", kFallbackFamily, kFallbackPointSize);
    return;
  }

  auto font = makeUniqueNoThrow<SdCardFont>(kFallbackOverflowCapacity);
  if (!font) {
    LOG_ERR("SDFS", "OOM: UI fallback SdCardFont");
    return;
  }
  // No prewarm: stubData.intervalCount stays 0 so getGlyph() routes every
  // codepoint through glyphMissHandler into the per-instance overflow ring.
  if (!font->load(file->path.c_str())) {
    LOG_ERR("SDFS", "Failed to load UI fallback font: %s", file->path.c_str());
    return;
  }

  const int fontId = SdCardFontManager::computeFontId(font->contentHash(), kFallbackFamily, kFallbackPointSize);
  // Guard against collision with an already-registered font (e.g. the reader
  // body font being the same NotoSansTC at the same size).
  if (renderer.getFontMap().count(fontId) != 0) {
    LOG_DBG("SDFS", "UI fallback ID %d already registered, reusing", fontId);
    uiFallbackFontId_ = fontId;
    return;
  }

  // The registered EpdFontFamily holds raw EpdFont* into font's stubData, so the
  // SdCardFont must outlive registration — uiFallback_ owns it for process life.
  renderer.registerSdCardFont(fontId, font.get());
  renderer.insertFont(fontId, EpdFontFamily(font->getEpdFont(0), font->getEpdFont(1), font->getEpdFont(2),
                                            font->getEpdFont(3)));
  uiFallback_ = std::move(font);
  uiFallbackFontId_ = fontId;
  LOG_DBG("SDFS", "UI fallback loaded: %s id=%d (overflow=%u)", file->path.c_str(), fontId, kFallbackOverflowCapacity);
}

void SdCardFontSystem::ensureLoaded(GfxRenderer& renderer) {
  // If the web server (or another task) installed/deleted fonts, re-discover.
  // Track whether we just re-discovered so we can force a reload below even
  // when the wanted family/size still maps to the same point size — the file
  // contents on disk may have changed (e.g. user re-uploaded a new build).
  const bool registryWasDirty = registryDirty_.exchange(false, std::memory_order_acquire);
  if (registryWasDirty) {
    LOG_DBG("SDFS", "Registry dirty — re-discovering fonts");
    registry_.discover();
  }

  const char* wantedFamily = SETTINGS.sdFontFamilyName;
  const std::string& currentFamily = manager_.currentFamilyName();
  const uint8_t sizeEnum = fontSizeEnumFromSettings();

  if (wantedFamily[0] == '\0') {
    if (!currentFamily.empty()) {
      manager_.unloadAll(renderer);
    }
    return;
  }

  // Reload if family changed OR if the user-selected size maps to a
  // different file than what's currently loaded OR if the registry was
  // just rediscovered (file may have been replaced on disk).
  bool familyMatches = (currentFamily == wantedFamily);
  if (familyMatches) {
    const auto* family = registry_.findFamily(wantedFamily);
    if (!family) {
      LOG_DBG("SDFS", "SD font family disappeared: %s (clearing)", wantedFamily);
      manager_.unloadAll(renderer);
      SETTINGS.sdFontFamilyName[0] = '\0';
      return;
    }
    const auto* selected = family->findClosestReaderSize(sizeEnum);
    const uint8_t wantedPt = selected ? selected->pointSize : 0;
    if (!registryWasDirty && wantedPt == manager_.currentPointSize()) return;
    LOG_DBG("SDFS", "Reloading %s: size %u -> %u (enum %u)%s", wantedFamily, manager_.currentPointSize(), wantedPt,
            sizeEnum, registryWasDirty ? " [registry dirty]" : "");
  }

  if (!currentFamily.empty()) {
    manager_.unloadAll(renderer);
  }

  const auto* family = registry_.findFamily(wantedFamily);
  if (family) {
    if (manager_.loadFamily(*family, renderer, sizeEnum)) {
      LOG_DBG("SDFS", "Loaded SD font family: %s", wantedFamily);
    } else {
      LOG_ERR("SDFS", "Failed to load SD font family: %s (clearing)", wantedFamily);
      SETTINGS.sdFontFamilyName[0] = '\0';
    }
  } else {
    LOG_DBG("SDFS", "SD font family not found: %s (clearing)", wantedFamily);
    SETTINGS.sdFontFamilyName[0] = '\0';
  }
}

int SdCardFontSystem::resolveFontId(const char* familyName, uint8_t /*fontSizeEnum*/) const {
  // The manager loads exactly one size (closest to SETTINGS.fontSize), so the
  // enum is implicit — always return the single loaded font ID for this family.
  // ensureLoaded() must have been called with the current settings before this.
  return manager_.getFontId(familyName);
}
