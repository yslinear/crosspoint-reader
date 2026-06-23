#pragma once

#include <HalStorage.h>

class Print;
class ZipFile;

class JpegToBmpConverter {
  static bool jpegFileToBmpStreamInternal(HalFile& jpegFile, Print& bmpOut, int targetWidth, int targetHeight,
                                          bool oneBit, bool crop = true);

 public:
  // Create the decode serialization mutex once, at startup (single-threaded),
  // before any task can decode. Eager init closes the non-atomic check-then-create
  // window in DecodeLock (a prio-1 render-task decode could otherwise preempt the
  // prio-0 worker between the null-check and the store and create a second mutex).
  static void initDecodeMutex();

  static bool jpegFileToBmpStream(HalFile& jpegFile, Print& bmpOut, bool crop = true);
  // Convert with custom target size (for thumbnails)
  static bool jpegFileToBmpStreamWithSize(HalFile& jpegFile, Print& bmpOut, int targetMaxWidth, int targetMaxHeight);
  // Convert to 1-bit BMP (black and white only, no grays) for fast home screen rendering
  static bool jpegFileTo1BitBmpStreamWithSize(HalFile& jpegFile, Print& bmpOut, int targetMaxWidth,
                                              int targetMaxHeight);
};
