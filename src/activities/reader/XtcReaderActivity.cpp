/**
 * XtcReaderActivity.cpp
 *
 * XTC ebook reader activity implementation
 * Displays pre-rendered XTC pages on e-ink display
 */

#include "XtcReaderActivity.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "ProgressFile.h"
#include "ReaderUtils.h"
#include "RecentBooksStore.h"
#include "XtcReaderChapterSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

void XtcReaderActivity::onEnter() {
  Activity::onEnter();

  if (!xtc) {
    return;
  }

  xtc->setupCacheDir();

  // Load saved progress
  loadProgress();

  // Save current XTC as last opened book and add to recent books
  APP_STATE.openEpubPath = xtc->getPath();
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(xtc->getPath(), xtc->getTitle(), xtc->getAuthor(), xtc->getThumbBmpPath());

  // Trigger first update
  requestUpdate();
}

void XtcReaderActivity::onExit() {
  Activity::onExit();

  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();
  xtc.reset();
}

void XtcReaderActivity::loop() {
  // Enter chapter selection activity
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (xtc && xtc->hasChapters() && !xtc->getChapters().empty()) {
      startActivityForResult(
          std::make_unique<XtcReaderChapterSelectionActivity>(renderer, mappedInput, xtc, currentPage),
          [this](const ActivityResult& result) {
            if (!result.isCancelled) {
              currentPage = std::get<PageResult>(result.data).page;
            }
          });
    }
  }

  // Long press BACK (1s+) goes to file selection
  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= ReaderUtils::GO_HOME_MS) {
    activityManager.goToFileBrowser(xtc ? xtc->getPath() : "");
    return;
  }

  // Short press BACK goes directly to home
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() < ReaderUtils::GO_HOME_MS) {
    onGoHome();
    return;
  }

  const auto [prevTriggered, nextTriggered, fromTilt] = ReaderUtils::detectPageTurn(mappedInput);
  if (!prevTriggered && !nextTriggered) {
    return;
  }

  // At end of the book, forward button goes home and back button returns to last page
  if (currentPage >= xtc->getPageCount()) {
    if (nextTriggered) {
      onGoHome();
    } else {
      currentPage = xtc->getPageCount() - 1;
      requestUpdate();
    }
    return;
  }

  const bool skipPages = !fromTilt && SETTINGS.longPressButtonBehavior == SETTINGS.CHAPTER_SKIP &&
                         mappedInput.getHeldTime() > ReaderUtils::SKIP_HOLD_MS;
  const int skipAmount = skipPages ? 10 : 1;

  if (prevTriggered) {
    if (currentPage >= static_cast<uint32_t>(skipAmount)) {
      currentPage -= skipAmount;
    } else {
      currentPage = 0;
    }
    requestUpdate();
  } else if (nextTriggered) {
    currentPage += skipAmount;
    if (currentPage >= xtc->getPageCount()) {
      currentPage = xtc->getPageCount();  // Allow showing "End of book"
    }
    requestUpdate();
  }
}

void XtcReaderActivity::render(RenderLock&&) {
  if (!xtc) {
    return;
  }

  // Bounds check
  if (currentPage >= xtc->getPageCount()) {
    // Show end of book screen
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_END_OF_BOOK), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  renderPage();
  saveProgress();
}

XtcReaderActivity::StatusBarInfo XtcReaderActivity::getStatusBarInfo() const {
  const int bookPageCount = static_cast<int>(xtc->getPageCount());
  const int bookPage = static_cast<int>(currentPage) + 1;
  std::string title =
      SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::BOOK_TITLE ? xtc->getTitle() : "";

  if (!xtc->hasChapters()) {
    return StatusBarInfo{bookPage, bookPageCount, std::move(title)};
  }

  const auto& chapters = xtc->getChapters();
  const auto chapterIt = std::find_if(chapters.begin(), chapters.end(), [this](const xtc::ChapterInfo& chapter) {
    return currentPage >= chapter.startPage && currentPage <= chapter.endPage;
  });

  if (chapterIt == chapters.end() || chapterIt->endPage < chapterIt->startPage) {
    return StatusBarInfo{bookPage, bookPageCount, std::move(title)};
  }

  if (SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::CHAPTER_TITLE) {
    title = chapterIt->name.empty() ? tr(STR_UNNAMED) : chapterIt->name;
  }

  return StatusBarInfo{static_cast<int>(currentPage - chapterIt->startPage) + 1,
                       static_cast<int>(chapterIt->endPage - chapterIt->startPage) + 1, std::move(title)};
}

void XtcReaderActivity::renderStatusBarOverlay(const StatusBarOverlayPosition position) const {
  const bool drawBottom = SETTINGS.xtcStatusBarMode == CrossPointSettings::XTC_STATUS_BAR_MODE::XTC_STATUS_BAR_BOTTOM &&
                          position == StatusBarOverlayPosition::Bottom;
  const bool drawTop = SETTINGS.xtcStatusBarMode == CrossPointSettings::XTC_STATUS_BAR_MODE::XTC_STATUS_BAR_TOP &&
                       position == StatusBarOverlayPosition::Top;
  if (!drawBottom && !drawTop) {
    return;
  }

  const int statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  if (statusBarHeight <= 0) {
    return;
  }

  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);

  int clearY;
  int paddingBottom = 0;
  if (position == StatusBarOverlayPosition::Bottom) {
    clearY = renderer.getScreenHeight() - orientedMarginBottom - statusBarHeight - 4;
    if (clearY < 0) {
      clearY = 0;
    }
  } else {
    clearY = orientedMarginTop;
    paddingBottom = renderer.getScreenHeight() - statusBarHeight - orientedMarginBottom - orientedMarginTop - 4;
  }
  const int clearHeight = position == StatusBarOverlayPosition::Bottom
                              ? renderer.getScreenHeight() - orientedMarginBottom - clearY
                              : statusBarHeight + 4;
  if (clearHeight > 0) {
    renderer.fillRect(0, clearY, renderer.getScreenWidth(), clearHeight, false);
  }

  const int pageCount = static_cast<int>(xtc->getPageCount());
  const int displayPage = static_cast<int>(currentPage) + 1;
  const float progress = pageCount > 0 ? (static_cast<float>(displayPage) * 100.0f) / pageCount : 0.0f;
  const auto pageInfo = getStatusBarInfo();
  GUI.drawStatusBar(renderer, progress, pageInfo.currentPage, pageInfo.pageCount, pageInfo.title, paddingBottom);
}

void XtcReaderActivity::renderPage() {
  const uint16_t pageWidth = xtc->getPageWidth();
  const uint16_t pageHeight = xtc->getPageHeight();
  const uint8_t bitDepth = xtc->getBitDepth();

  // 2-bit (XTH) pages need two 48KB bit planes resident (96KB) in the whole-page
  // path, which a fresh-booted device can usually allocate but a heap fragmented
  // by reading often cannot -> STR_MEMORY_ERROR. Band-render instead: stream one
  // horizontal band's source columns at a time so peak heap is ~24KB (16KB
  // transposed band sub-buffer + 8KB strip scratch) rather than 96KB. The page's
  // column-major layout maps each physical band to a contiguous run of source
  // columns in the two portrait orientations, so a band's bytes are a single
  // contiguous read per plane. Falls back to the whole-page path below when
  // banding does not apply (landscape, no strip-grayscale support, compressed
  // page) or its small buffers fail to allocate.
  const auto orient = renderer.getOrientation();
  const bool portraitOrient = orient == GfxRenderer::Portrait || orient == GfxRenderer::PortraitInverted;
  // The band <-> physical-row mapping is only exact when the page fills the panel
  // in portrait: source columns (pageWidth) tile the physical band axis
  // (panel height) and source rows (pageHeight) span the physical width.
  const bool pageFillsPanel = pageWidth == renderer.getDisplayHeight() && pageHeight == renderer.getDisplayWidth();
  if (bitDepth == 2 && portraitOrient && pageFillsPanel && renderer.supportsStripGrayscale()) {
    renderer.clearScreen();
    if (renderPage2BitBanded(pageWidth, pageHeight)) {
      return;
    }
    // Banding declined (e.g. compressed page or OOM on the small buffers);
    // fall through to the whole-page path, which logs STR_MEMORY_ERROR if the
    // 96KB allocation also fails.
  }

  // Calculate buffer size for one page
  // XTG (1-bit): Row-major, ((width+7)/8) * height bytes
  // XTH (2-bit): Two bit planes, column-major, ((width * height + 7) / 8) * 2 bytes
  size_t pageBufferSize;
  if (bitDepth == 2) {
    pageBufferSize = ((static_cast<size_t>(pageWidth) * pageHeight + 7) / 8) * 2;
  } else {
    pageBufferSize = ((pageWidth + 7) / 8) * pageHeight;
  }

  // Allocate page buffer
  uint8_t* pageBuffer = static_cast<uint8_t*>(malloc(pageBufferSize));
  if (!pageBuffer) {
    LOG_ERR("XTR", "Failed to allocate page buffer (%lu bytes)", pageBufferSize);
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_MEMORY_ERROR), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  // Load page data
  size_t bytesRead = xtc->loadPage(currentPage, pageBuffer, pageBufferSize);
  if (bytesRead == 0) {
    LOG_ERR("XTR", "Failed to load page %lu: bufferSize=%lu bitDepth=%u error=%s", currentPage, pageBufferSize,
            bitDepth, xtc::errorToString(xtc->getLastError()));
    free(pageBuffer);
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_PAGE_LOAD_ERROR), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  // Clear screen first
  renderer.clearScreen();

  // Copy page bitmap using GfxRenderer's drawPixel
  // XTC/XTCH pages are pre-rendered with status bar included, so render full page
  const uint16_t maxSrcY = pageHeight;

  if (bitDepth == 2) {
    // XTH 2-bit mode: Two bit planes, column-major order
    // - Columns scanned right to left (x = width-1 down to 0)
    // - 8 vertical pixels per byte (MSB = topmost pixel in group)
    // - First plane: Bit1, Second plane: Bit2
    // - Pixel value = (bit1 << 1) | bit2
    // - Grayscale: 0=White, 1=Dark Grey, 2=Light Grey, 3=Black

    const size_t planeSize = (static_cast<size_t>(pageWidth) * pageHeight + 7) / 8;
    const uint8_t* plane1 = pageBuffer;              // Bit1 plane
    const uint8_t* plane2 = pageBuffer + planeSize;  // Bit2 plane
    const size_t colBytes = (pageHeight + 7) / 8;    // Bytes per column (100 for 800 height)

    // Lambda to get pixel value at (x, y)
    auto getPixelValue = [&](uint16_t x, uint16_t y) -> uint8_t {
      const size_t colIndex = pageWidth - 1 - x;
      const size_t byteInCol = y / 8;
      const size_t bitInByte = 7 - (y % 8);
      const size_t byteOffset = colIndex * colBytes + byteInCol;
      const uint8_t bit1 = (plane1[byteOffset] >> bitInByte) & 1;
      const uint8_t bit2 = (plane2[byteOffset] >> bitInByte) & 1;
      return (bit1 << 1) | bit2;
    };

    // Optimized grayscale rendering without storeBwBuffer (saves 48KB peak memory)
    // Flow: BW display → LSB/MSB passes → grayscale display → re-render BW for next frame

    // Count pixel distribution for debugging
    uint32_t pixelCounts[4] = {0, 0, 0, 0};
    for (uint16_t y = 0; y < pageHeight; y++) {
      for (uint16_t x = 0; x < pageWidth; x++) {
        pixelCounts[getPixelValue(x, y)]++;
      }
    }
    LOG_DBG("XTR", "Pixel distribution: White=%lu, DarkGrey=%lu, LightGrey=%lu, Black=%lu", pixelCounts[0],
            pixelCounts[1], pixelCounts[2], pixelCounts[3]);

    // Pass 1: BW buffer - draw all non-white pixels as black
    for (uint16_t y = 0; y < pageHeight; y++) {
      for (uint16_t x = 0; x < pageWidth; x++) {
        if (getPixelValue(x, y) >= 1) {
          renderer.drawPixel(x, y, true);
        }
      }
    }

    ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);

    // Pass 2: LSB buffer - mark DARK gray only (XTH value 1)
    // In LUT: 0 bit = apply gray effect, 1 bit = untouched
    renderer.clearScreen(0x00);
    for (uint16_t y = 0; y < pageHeight; y++) {
      for (uint16_t x = 0; x < pageWidth; x++) {
        if (getPixelValue(x, y) == 1) {  // Dark grey only
          renderer.drawPixel(x, y, false);
        }
      }
    }
    renderer.copyGrayscaleLsbBuffers();

    // Pass 3: MSB buffer - mark LIGHT AND DARK gray (XTH value 1 or 2)
    // In LUT: 0 bit = apply gray effect, 1 bit = untouched
    renderer.clearScreen(0x00);
    for (uint16_t y = 0; y < pageHeight; y++) {
      for (uint16_t x = 0; x < pageWidth; x++) {
        const uint8_t pv = getPixelValue(x, y);
        if (pv == 1 || pv == 2) {  // Dark grey or Light grey
          renderer.drawPixel(x, y, false);
        }
      }
    }
    renderer.copyGrayscaleMsbBuffers();

    // Display grayscale overlay
    renderer.displayGrayBuffer();

    // Pass 4: Re-render BW to framebuffer (restore for next frame, instead of restoreBwBuffer)
    renderer.clearScreen();
    for (uint16_t y = 0; y < pageHeight; y++) {
      for (uint16_t x = 0; x < pageWidth; x++) {
        if (getPixelValue(x, y) >= 1) {
          renderer.drawPixel(x, y, true);
        }
      }
    }

    // Cleanup grayscale buffers with current frame buffer
    renderer.cleanupGrayscaleWithFrameBuffer();

    free(pageBuffer);

    LOG_DBG("XTR", "Rendered page %lu/%lu (2-bit grayscale)", currentPage + 1, xtc->getPageCount());
    return;
  } else {
    // 1-bit mode: 8 pixels per byte, MSB first
    const size_t srcRowBytes = (pageWidth + 7) / 8;  // 60 bytes for 480 width

    for (uint16_t srcY = 0; srcY < maxSrcY; srcY++) {
      const size_t srcRowStart = srcY * srcRowBytes;

      for (uint16_t srcX = 0; srcX < pageWidth; srcX++) {
        // Read source pixel (MSB first, bit 7 = leftmost pixel)
        const size_t srcByte = srcRowStart + srcX / 8;
        const size_t srcBit = 7 - (srcX % 8);
        const bool isBlack = !((pageBuffer[srcByte] >> srcBit) & 1);  // XTC: 0 = black, 1 = white

        if (isBlack) {
          renderer.drawPixel(srcX, srcY, true);
        }
      }
    }
  }
  // White pixels are already cleared by clearScreen()

  free(pageBuffer);

  if (SETTINGS.xtcStatusBarMode == CrossPointSettings::XTC_STATUS_BAR_MODE::XTC_STATUS_BAR_TOP) {
    renderStatusBarOverlay(StatusBarOverlayPosition::Top);
  } else {
    renderStatusBarOverlay(StatusBarOverlayPosition::Bottom);
  }

  ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);

  LOG_DBG("XTR", "Rendered page %lu/%lu (%u-bit)", currentPage + 1, xtc->getPageCount(), bitDepth);
}

bool XtcReaderActivity::renderPage2BitBanded(uint16_t pageWidth, uint16_t pageHeight) {
  // Caller guarantees: bitDepth == 2, Portrait/PortraitInverted, strip grayscale
  // supported, framebuffer already cleared.
  //
  // XTH layout: two bit planes, column-major (colIndex = pageWidth-1-x), each
  // column colBytes contiguous bytes, 8 vertical pixels/byte (MSB = topmost).
  // pixelValue = (bit1 << 1) | bit2; 0=White 1=DarkGrey 2=LightGrey 3=Black.
  //
  // In Portrait, drawPixel maps phyY = panelHeight-1-x, so a physical band
  // [y0,y0+rows) is exactly source columns x in [panelH-y0-rows, panelH-y0).
  // In PortraitInverted, phyY = x, so the band is x in [y0, y0+rows). Either way
  // the band is a contiguous source-column range, hence a contiguous file run.
  const GfxRenderer::Orientation orient = renderer.getOrientation();
  const int panelH = renderer.getDisplayHeight();      // physical band axis (480)
  const int gwBytes = renderer.getDisplayWidthBytes();  // strip scratch row stride
  constexpr int STRIP_ROWS = 80;

  const size_t planeSize = (static_cast<size_t>(pageWidth) * pageHeight + 7) / 8;
  const size_t colBytes = (pageHeight + 7) / 8;  // bytes per source column

  // Band sub-buffer holds up to STRIP_ROWS columns x colBytes for both planes.
  // ~16KB (80*101*2) vs the 96KB full page. Strip scratch is gwBytes*STRIP_ROWS
  // (~8KB). Peak ~24KB. Both freed automatically on return.
  const size_t bandColsMax = STRIP_ROWS;
  const size_t subPlaneBytes = bandColsMax * colBytes;
  auto sub = makeUniqueNoThrow<uint8_t[]>(subPlaneBytes * 2);
  if (!sub) {
    LOG_ERR("XTR", "OOM: band sub-buffer (%u bytes)", static_cast<unsigned>(subPlaneBytes * 2));
    return false;
  }
  auto scratch = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(gwBytes) * STRIP_ROWS);
  if (!scratch) {
    LOG_ERR("XTR", "OOM: strip scratch (%d bytes)", gwBytes * STRIP_ROWS);
    return false;
  }

  // Map a physical band origin/extent to its source-column range [colLo, colLo+n)
  // (colIndex space) and read both planes' contiguous runs into the sub-buffer.
  // Returns false on read error (caller falls back to the whole-page path).
  // colLo is the lowest colIndex in the band; column k of the band lives at
  // sub[(k)*colBytes ...] for plane1 and sub[subPlaneBytes + k*colBytes ...].
  size_t bandColLo = 0;  // lowest colIndex resident in sub
  size_t bandColCount = 0;
  auto fillBand = [&](int y0, int rows) -> bool {
    // Source-column (x) range covered by this physical band.
    int xLo;
    if (orient == GfxRenderer::Portrait) {
      xLo = panelH - y0 - rows;  // phyY = panelH-1-x
    } else {
      xLo = y0;  // PortraitInverted: phyY = x
    }
    // colIndex = pageWidth-1-x; x in [xLo, xLo+rows) -> colIndex in
    // [pageWidth-xLo-rows, pageWidth-xLo). Lowest colIndex:
    bandColLo = static_cast<size_t>(pageWidth) - xLo - rows;
    bandColCount = static_cast<size_t>(rows);
    const size_t runBytes = bandColCount * colBytes;
    const size_t srcOff = bandColLo * colBytes;
    if (xtc->loadPageRegion(currentPage, srcOff, sub.get(), runBytes) == 0) {
      return false;  // plane1
    }
    if (xtc->loadPageRegion(currentPage, planeSize + srcOff, sub.get() + subPlaneBytes, runBytes) == 0) {
      return false;  // plane2
    }
    return true;
  };

  // Band-local pixel value lookup into the sub-buffer (assumes (x,y) is within
  // the currently resident band).
  auto bandPixel = [&](uint16_t x, uint16_t y) -> uint8_t {
    const size_t colIndex = static_cast<size_t>(pageWidth) - 1 - x;
    const size_t k = colIndex - bandColLo;  // 0..bandColCount-1
    const size_t byteInCol = y / 8;
    const size_t bitInByte = 7 - (y % 8);
    const size_t off = k * colBytes + byteInCol;
    const uint8_t bit1 = (sub[off] >> bitInByte) & 1;
    const uint8_t bit2 = (sub[subPlaneBytes + off] >> bitInByte) & 1;
    return (bit1 << 1) | bit2;
  };

  // Iterate the source-column range that this physical band covers and invoke f
  // for each (x,y). x runs over the band's columns, y over full page height.
  auto forEachInBand = [&](int y0, int rows, auto&& f) {
    int xLo;
    if (orient == GfxRenderer::Portrait) {
      xLo = panelH - y0 - rows;
    } else {
      xLo = y0;
    }
    const int xHi = xLo + rows;  // exclusive
    for (int x = xLo; x < xHi; x++) {
      if (x < 0 || x >= pageWidth) continue;
      for (uint16_t y = 0; y < pageHeight; y++) {
        f(static_cast<uint16_t>(x), y);
      }
    }
  };

  // Pass 1: build the full BW framebuffer band by band (non-white -> black).
  for (int y = 0; y < panelH; y += STRIP_ROWS) {
    const int rows = (panelH - y < STRIP_ROWS) ? (panelH - y) : STRIP_ROWS;
    if (!fillBand(y, rows)) {
      LOG_ERR("XTR", "Band read failed (BW) at y=%d", y);
      return false;
    }
    forEachInBand(y, rows, [&](uint16_t x, uint16_t yy) {
      if (bandPixel(x, yy) >= 1) {
        renderer.drawPixel(x, yy, true);
      }
    });
  }
  ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);

  // Pass 2: LSB plane (DarkGrey only, value 1) streamed straight to controller.
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
  for (int y = 0; y < panelH; y += STRIP_ROWS) {
    const int rows = (panelH - y < STRIP_ROWS) ? (panelH - y) : STRIP_ROWS;
    if (!fillBand(y, rows)) {
      LOG_ERR("XTR", "Band read failed (LSB) at y=%d", y);
      renderer.setRenderMode(GfxRenderer::BW);
      return false;
    }
    renderer.beginStripTarget(scratch.get(), y, rows);
    renderer.clearScreen(0x00);
    forEachInBand(y, rows, [&](uint16_t x, uint16_t yy) {
      if (bandPixel(x, yy) == 1) {
        renderer.drawPixel(x, yy, false);
      }
    });
    renderer.endStripTarget();
    renderer.writeGrayscalePlaneStrip(true, scratch.get(), y, rows);
  }

  // Pass 3: MSB plane (DarkGrey or LightGrey, value 1 or 2).
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
  for (int y = 0; y < panelH; y += STRIP_ROWS) {
    const int rows = (panelH - y < STRIP_ROWS) ? (panelH - y) : STRIP_ROWS;
    if (!fillBand(y, rows)) {
      LOG_ERR("XTR", "Band read failed (MSB) at y=%d", y);
      renderer.setRenderMode(GfxRenderer::BW);
      return false;
    }
    renderer.beginStripTarget(scratch.get(), y, rows);
    renderer.clearScreen(0x00);
    forEachInBand(y, rows, [&](uint16_t x, uint16_t yy) {
      const uint8_t pv = bandPixel(x, yy);
      if (pv == 1 || pv == 2) {
        renderer.drawPixel(x, yy, false);
      }
    });
    renderer.endStripTarget();
    renderer.writeGrayscalePlaneStrip(false, scratch.get(), y, rows);
  }

  renderer.setRenderMode(GfxRenderer::BW);
  renderer.displayGrayBuffer();
  // BW framebuffer from Pass 1 is intact (strip passes wrote only to scratch);
  // re-sync controller RAM for the next differential page turn directly from it.
  renderer.cleanupGrayscaleWithFrameBuffer();

  LOG_DBG("XTR", "Rendered page %lu/%lu (2-bit grayscale, banded)", currentPage + 1, xtc->getPageCount());
  return true;
}

void XtcReaderActivity::saveProgress() const {
  uint8_t data[4];
  data[0] = currentPage & 0xFF;
  data[1] = (currentPage >> 8) & 0xFF;
  data[2] = (currentPage >> 16) & 0xFF;
  data[3] = (currentPage >> 24) & 0xFF;
  if (!ProgressFile::writeAtomic(xtc->getCachePath(), data, sizeof(data))) {
    LOG_ERR("XTR", "Failed to save progress: page %lu", currentPage);
  }
}

void XtcReaderActivity::loadProgress() {
  HalFile f;
  if (Storage.openFileForRead("XTR", xtc->getCachePath() + "/progress.bin", f)) {
    uint8_t data[4];
    if (f.read(data, 4) == 4) {
      currentPage = data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24);
      LOG_DBG("XTR", "Loaded progress: page %lu", currentPage);

      // Validate page number
      if (currentPage >= xtc->getPageCount()) {
        currentPage = 0;
      }
    }
    f.close();
  }
}

ScreenshotInfo XtcReaderActivity::getScreenshotInfo() const {
  ScreenshotInfo info;
  info.readerType = ScreenshotInfo::ReaderType::Xtc;
  if (xtc) {
    const std::string t = xtc->getTitle();
    snprintf(info.title, sizeof(info.title), "%s", t.c_str());
    const uint32_t pageCount = xtc->getPageCount();
    info.totalPages = pageCount;
    // Clamp to last valid page to avoid sentinel value (currentPage == pageCount)
    uint32_t clampedPage = (pageCount > 0 && currentPage >= pageCount) ? pageCount - 1 : currentPage;
    info.progressPercent = pageCount > 0 ? xtc->calculateProgress(clampedPage) : 0;
    info.currentPage = static_cast<int>(clampedPage) + 1;
  } else {
    info.currentPage = currentPage + 1;
  }
  return info;
}
