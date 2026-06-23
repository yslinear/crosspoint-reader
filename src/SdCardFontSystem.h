#pragma once

#include <SdCardFontManager.h>
#include <SdCardFontRegistry.h>

#include <atomic>
#include <memory>

class GfxRenderer;
class SdCardFont;

/// Facade that owns the SD card font registry, manager, and resolver logic.
/// Hides implementation details behind a single begin() + ensureLoaded() API.
class SdCardFontSystem {
 public:
  // Defined out-of-line in the .cpp: the unique_ptr<SdCardFont> member needs the
  // complete SdCardFont type to instantiate its deleter, which is only available
  // where SdCardFont.h is included (the .cpp), not here (forward-declared).
  SdCardFontSystem();
  ~SdCardFontSystem();
  SdCardFontSystem(const SdCardFontSystem&) = delete;
  SdCardFontSystem& operator=(const SdCardFontSystem&) = delete;
  /// Discover SD card fonts and load user's saved selection. Call once during setup.
  void begin(GfxRenderer& renderer);

  /// Ensure the correct SD font family is loaded for the current settings.
  /// Call before entering the reader or after settings change.
  /// Also re-discovers if the registry has been marked dirty (e.g. by web upload).
  void ensureLoaded(GfxRenderer& renderer);

  /// Resolve an SD card font ID from family name + fontSize enum.
  /// Returns 0 if not found. Used by CrossPointSettings::getReaderFontId().
  int resolveFontId(const char* familyName, uint8_t fontSizeEnum) const;

  /// Font ID of the UI->CJK glyph fallback, or 0 if no CJK fallback font is
  /// installed. Valid after begin(). Passed to GfxRenderer::setUiFallbackFont().
  int getUiFallbackFontId() const { return uiFallbackFontId_; }

  /// Access the registry (e.g. for settings UI to enumerate available fonts).
  const SdCardFontRegistry& registry() const { return registry_; }

  /// Non-const access to the registry (for FontInstaller).
  SdCardFontRegistry& registry() { return registry_; }

  /// Mark the registry as needing re-discovery.
  /// Thread-safe: can be called from the web server task.
  void markRegistryDirty() { registryDirty_.store(true, std::memory_order_release); }

  /// If the registry is dirty, re-scan the SD card now and clear the flag.
  /// Used by the web UI so uploaded/deleted fonts appear in the list
  /// without waiting for the reader activity to run ensureLoaded().
  void refreshIfDirty() {
    if (registryDirty_.exchange(false, std::memory_order_acquire)) {
      registry_.discover();
    }
  }

 private:
  /// Load the CJK UI-fallback font (NotoSansTC at 12px) if present on the SD
  /// card, registering it with the renderer and recording its ID in
  /// uiFallbackFontId_. No-op (leaves the ID 0) when the font is absent, so
  /// non-CJK users pay zero extra RAM. Parallel to SdCardFontManager::loadFamily
  /// but uses a no-prewarm path and its own long-lived SdCardFont owner.
  void loadUiFallback(GfxRenderer& renderer);

  SdCardFontRegistry registry_;
  SdCardFontManager manager_;
  std::atomic<bool> registryDirty_{false};

  // Long-lived owner of the UI-fallback SdCardFont. The EpdFontFamily registered
  // into the renderer holds raw EpdFont* into this object's stubData, so it must
  // outlive every render — owned for the whole process lifetime here.
  std::unique_ptr<SdCardFont> uiFallback_;
  int uiFallbackFontId_ = 0;
};

// Global SD card font system instance (defined in main.cpp).
extern SdCardFontSystem sdFontSystem;
