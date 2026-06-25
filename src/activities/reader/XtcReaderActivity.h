/**
 * XtcReaderActivity.h
 *
 * XTC ebook reader activity for CrossPoint Reader
 * Displays pre-rendered XTC pages on e-ink display
 */

#pragma once

#include <Xtc.h>

#include <string>
#include <utility>

#include "activities/Activity.h"

class XtcReaderActivity final : public Activity {
  std::shared_ptr<Xtc> xtc;

  uint32_t currentPage = 0;
  int pagesUntilFullRefresh = 0;

  enum class StatusBarOverlayPosition { Bottom, Top };
  struct StatusBarInfo {
    int currentPage;
    int pageCount;
    std::string title;
  };

  // Logical-Y span (oriented coordinates) occupied by the status-bar overlay,
  // so the 2-bit grayscale passes can leave those framebuffer pixels untouched
  // (the BW bar drawn in Pass 1 survives displayGrayBuffer). [y0, y1) is the
  // exclusion band; empty (y0 == y1) means no overlay was drawn.
  struct StatusBarBand {
    int y0 = 0;
    int y1 = 0;
    bool contains(int y) const { return y >= y0 && y < y1; }
    bool empty() const { return y1 <= y0; }
  };

  void renderPage();
  // Band-renders a 2-bit (XTH) page without a full-page buffer, streaming one
  // horizontal band at a time. Returns false (caller falls back to the
  // whole-page path) if banding does not apply or its small buffers fail to
  // allocate. Precondition: bitDepth == 2.
  bool renderPage2BitBanded(uint16_t pageWidth, uint16_t pageHeight);
  // Draws the status-bar overlay into the framebuffer (no display) and returns
  // the logical-Y band it occupies, so callers can exclude it from grayscale
  // passes. Returns an empty band when nothing is drawn for this position.
  StatusBarBand renderStatusBarOverlay(StatusBarOverlayPosition position) const;
  // Single funnel that every 2-bit render path calls. Maps the configured XTC
  // status-bar mode (via xtcsb::overlayFor) to renderStatusBarOverlay, drawing
  // the bar into the BW framebuffer BEFORE the page's single display so no
  // separate FAST refresh is needed. Returns the band the bar occupies (empty
  // when the mode is HIDE) for the grayscale passes to skip.
  StatusBarBand drawConfiguredStatusBarOverlay() const;
  StatusBarInfo getStatusBarInfo() const;
  void saveProgress() const;
  void loadProgress();

 public:
  explicit XtcReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Xtc> xtc)
      : Activity("XtcReader", renderer, mappedInput), xtc(std::move(xtc)) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }
  ScreenshotInfo getScreenshotInfo() const override;
};
