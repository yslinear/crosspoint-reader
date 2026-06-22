#pragma once

#include <cstdint>
#include <string>
#include <vector>

class GfxRenderer;
class SdCardFont;
struct SdCardFontFamilyInfo;

class SdCardFontManager {
 public:
  SdCardFontManager() = default;
  ~SdCardFontManager();
  SdCardFontManager(const SdCardFontManager&) = delete;
  SdCardFontManager& operator=(const SdCardFontManager&) = delete;

  // Load the font file whose physical point size is closest to the reader
  // fontSizeEnum (SMALL=12, MEDIUM=14, LARGE=16, EXTRA_LARGE=18). Only one
  // .cpfont file is loaded; other sizes remain on disk. This keeps resident
  // interval + kern/ligature tables to one size's worth of memory.
  // Returns true on success.
  bool loadFamily(const SdCardFontFamilyInfo& family, GfxRenderer& renderer, uint8_t fontSizeEnum);

  // Unload everything, unregister from renderer.
  void unloadAll(GfxRenderer& renderer);

  // Look up the font ID for the loaded family. Returns 0 if nothing loaded
  // or familyName doesn't match.
  int getFontId(const std::string& familyName) const;

  // Get name of currently loaded family (empty if none).
  const std::string& currentFamilyName() const { return loadedFamilyName_; };

  // Point size that was actually loaded.
  // 0 if nothing loaded.
  uint8_t currentPointSize() const { return loadedPointSize_; };

  // Deterministic font ID from content hash + family name + point size. Stable
  // across load/unload cycles and reboots; changes when font content changes.
  // Exposed so parallel loaders (e.g. the UI-fallback path in SdCardFontSystem)
  // generate IDs identically without duplicating the hash.
  static int computeFontId(uint32_t contentHash, const char* familyName, uint8_t pointSize);

 private:
  struct LoadedFont {
    SdCardFont* font;  // heap-allocated, owned
    int fontId;
    uint8_t size;
  };

  std::string loadedFamilyName_;
  uint8_t loadedPointSize_ = 0;
  std::vector<LoadedFont> loaded_;
};
