/**
 * Xtc.h
 *
 * Main XTC ebook class for CrossPoint Reader
 * Provides EPUB-like interface for XTC file handling
 */

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "Xtc/XtcParser.h"
#include "Xtc/XtcTypes.h"

/**
 * XTC Ebook Handler
 *
 * Handles XTC file loading, page access, and cover image generation.
 * Interface is designed to be similar to Epub class for easy integration.
 */
class Xtc {
  std::string filepath;
  std::string cachePath;
  std::unique_ptr<xtc::XtcParser> parser;
  bool loaded;

 public:
  explicit Xtc(std::string filepath, const std::string& cacheDir) : filepath(std::move(filepath)), loaded(false) {
    // Create cache key based on filepath (same as Epub)
    cachePath = cacheDir + "/xtc_" + std::to_string(std::hash<std::string>{}(this->filepath));
  }
  ~Xtc() = default;

  /**
   * Load XTC file
   * @return true on success
   */
  bool load();

  /**
   * Clear cached data
   * @return true on success
   */
  bool clearCache() const;

  /**
   * Setup cache directory
   */
  void setupCacheDir() const;

  // Path accessors
  const std::string& getCachePath() const { return cachePath; }
  const std::string& getPath() const { return filepath; }

  // Metadata
  std::string getTitle() const;
  std::string getAuthor() const;
  bool hasChapters() const;
  const std::vector<xtc::ChapterInfo>& getChapters();

  // Cover image support (for sleep screen)
  std::string getCoverBmpPath() const;
  bool generateCoverBmp() const;
  // Thumbnail support (for Continue Reading card)
  std::string getThumbBmpPath() const;
  std::string getThumbBmpPath(int height) const;
  bool generateThumbBmp(int height) const;

  // Page access
  uint32_t getPageCount() const;
  uint16_t getPageWidth() const;
  uint16_t getPageHeight() const;
  uint8_t getBitDepth() const;  // 1 = XTC (1-bit), 2 = XTCH (2-bit)

  /**
   * Load page bitmap data
   * @param pageIndex Page index (0-based)
   * @param buffer Output buffer
   * @param bufferSize Buffer size
   * @return Number of bytes read
   */
  size_t loadPage(uint32_t pageIndex, uint8_t* buffer, size_t bufferSize) const;

  /**
   * Load a contiguous region of a page's bitmap data (after the page header).
   *
   * Used for band rendering: lets the reader pull only the columns a horizontal
   * band needs instead of the whole page. Only valid for uncompressed pages.
   * @param pageIndex Page index (0-based)
   * @param bitmapOffset Byte offset into the bitmap data
   * @param buffer Output buffer
   * @param length Number of bytes to read
   * @return Number of bytes read, 0 on failure
   */
  size_t loadPageRegion(uint32_t pageIndex, size_t bitmapOffset, uint8_t* buffer, size_t length) const;

  /**
   * Load page with streaming callback
   * @param pageIndex Page index
   * @param callback Callback for each chunk
   * @param chunkSize Chunk size
   * @return Error code
   */
  xtc::XtcError loadPageStreaming(uint32_t pageIndex,
                                  std::function<void(const uint8_t* data, size_t size, size_t offset)> callback,
                                  size_t chunkSize = 1024) const;

  // Progress calculation
  uint8_t calculateProgress(uint32_t currentPage) const;

  // Check if file is loaded
  bool isLoaded() const { return loaded; }

  // Error information
  xtc::XtcError getLastError() const;

 private:
  // XTH (2-bit) cover-thumbnail generator. Reads the column-major page as a few
  // contiguous vertical strips (one loadPageRegion per plane per strip) and
  // dithers column-wise with 1-bit Floyd-Steinberg, writing a 1-bit top-down BMP
  // to outPath. scaleInv_fp is the 16.16 inverse-scale factor. Returns false on
  // any alloc/read failure (no abort).
  bool generateThumbBmp2Bit(const xtc::PageInfo& pageInfo, uint16_t thumbWidth, uint16_t thumbHeight,
                            uint32_t scaleInv_fp, const std::string& outPath) const;

  // XTH (2-bit) sleep-screen cover generator. Like generateThumbBmp2Bit, reads the
  // column-major page as contiguous vertical strips (bounded RAM, never the 96KB
  // full page). Unlike the thumbnail, it preserves the 4 XTH gray levels: it
  // downscales to a holdable grayscale target (the sleep screen upscales it) and
  // writes a NATIVE-PALETTE 2-bit grayscale BMP (palette 0..3 -> 0/85/170/255) that
  // renders at the display's 4 native gray levels with no dithering. dstWidth/
  // dstHeight are the downscaled target; scaleInvX_fp/scaleInvY_fp are the 16.16
  // per-axis inverse-scale factors. Returns false on any alloc/read failure (no abort).
  bool generateCoverBmp2BitGray(const xtc::PageInfo& pageInfo, uint16_t dstWidth, uint16_t dstHeight,
                                uint32_t scaleInvX_fp, uint32_t scaleInvY_fp, const std::string& outPath) const;
};
