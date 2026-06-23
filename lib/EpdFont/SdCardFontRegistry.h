#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct SdCardFontFileInfo {
  std::string path;   // v4 on-disk naming: "/<root>/<Family>/<Family>_<size>.cpfont"
                      // where <root> is "/.fonts" (preferred, hidden) or "/fonts" (visible).
                      // e.g. "/.fonts/NotoSansCJK/NotoSansCJK_14.cpfont"
  uint8_t pointSize;  // parsed from filename: 14
  uint8_t style;      // always 0 in v4 (all 4 styles bundled in one file);
                      // kept for potential future formats
};

struct SdCardFontFamilyInfo {
  std::string name;  // directory name, e.g. "NotoSansCJK"
  std::vector<SdCardFontFileInfo> files;

  const SdCardFontFileInfo* findFile(uint8_t size, uint8_t style = 0) const;
  const SdCardFontFileInfo* findClosestReaderSize(uint8_t fontSizeEnum, uint8_t style = 0) const;
  bool hasSize(uint8_t size) const;
  std::vector<uint8_t> availableSizes() const;
};

// Pretty-print a raw SD font family (directory) name for DISPLAY, inserting a
// space at each lowercase->uppercase boundary so "NotoSansTC" shows as
// "Noto Sans TC" (matching the built-in font labels). Display only — the raw
// name is still used for selection/matching and the SD-card folder/path.
inline std::string prettifyFontName(const std::string& raw) {
  std::string out;
  out.reserve(raw.size() + 4);
  for (size_t i = 0; i < raw.size(); i++) {
    const char c = raw[i];
    const char prev = i > 0 ? raw[i - 1] : '\0';
    if (c >= 'A' && c <= 'Z' && prev >= 'a' && prev <= 'z') out += ' ';
    out += c;
  }
  return out;
}

class SdCardFontRegistry {
 public:
  static constexpr int MAX_SD_FAMILIES = 128;
  // Two top-level roots are scanned at discovery time. Hidden is preferred
  // when creating new installs; both are read from if present.
  static constexpr const char* FONTS_DIR_HIDDEN = "/.fonts";
  static constexpr const char* FONTS_DIR_VISIBLE = "/fonts";

  // Returns the existing root for `familyName` (the one that contains
  // /<root>/<familyName>/), or nullptr if the family is not installed in
  // either root. Used by writers to keep re-installs in their existing dir.
  static const char* findFamilyRoot(const char* familyName);

  // Returns the root path that should be used when creating a brand-new
  // family on disk (no prior install): the existing root if exactly one of
  // the two roots exists, otherwise the hidden root.
  static const char* defaultWriteRoot();

  // Scan SD card, populate families_. Returns true if any families found.
  bool discover();

  const std::vector<SdCardFontFamilyInfo>& getFamilies() const { return families_; }
  const SdCardFontFamilyInfo* findFamily(const std::string& name) const;
  int getFamilyIndex(const std::string& name) const;
  int getFamilyCount() const { return static_cast<int>(families_.size()); }

 private:
  std::vector<SdCardFontFamilyInfo> families_;  // sorted alphabetically

  static bool parseFilename(const char* filename, uint8_t& size, uint8_t& style);
  static void scanDirectory(const char* dirPath, SdCardFontFamilyInfo& family);
  // Scan one root (e.g. "/.fonts"), append families to `out`, dedup by name.
  static void scanRoot(const char* rootPath, std::vector<SdCardFontFamilyInfo>& out);
};
