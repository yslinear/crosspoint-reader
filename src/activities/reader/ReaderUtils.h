#pragma once

#include <CrossPointSettings.h>
#include <GfxRenderer.h>
#include <HalTiltSensor.h>
#include <Logging.h>

#include "MappedInputManager.h"
#include "ReaderShortcutDecision.h"

namespace ReaderUtils {

constexpr unsigned long GO_HOME_MS = 1000;
constexpr unsigned long SKIP_HOLD_MS = 700;
constexpr unsigned long BOOKMARK_HOLD_MS = 400;
constexpr unsigned long BOOKMARK_MESSAGE_DURATION_MS = 2500;

// Pin the dependency-free thresholds in the pure decision core to the runtime
// constants so the two can never drift.
static_assert(rdr::HELD_GO_HOME_MS == GO_HOME_MS, "GO_HOME_MS out of sync with rdr core");
static_assert(rdr::HELD_SKIP_HOLD_MS == SKIP_HOLD_MS, "SKIP_HOLD_MS out of sync with rdr core");
static_assert(rdr::HELD_BOOKMARK_HOLD_MS == BOOKMARK_HOLD_MS, "BOOKMARK_HOLD_MS out of sync with rdr core");

// Pin the decoded settings literals in the pure core to the real enum values.
static_assert(rdr::settings::LP_BEHAVIOR_OFF == CrossPointSettings::OFF, "LP_BEHAVIOR_OFF out of sync");
static_assert(rdr::settings::LP_BEHAVIOR_CHAPTER_SKIP == CrossPointSettings::CHAPTER_SKIP,
              "LP_BEHAVIOR_CHAPTER_SKIP out of sync");
static_assert(rdr::settings::LP_BEHAVIOR_ORIENTATION_CHANGE == CrossPointSettings::ORIENTATION_CHANGE,
              "LP_BEHAVIOR_ORIENTATION_CHANGE out of sync");
static_assert(rdr::settings::LP_MENU_KOSYNC == CrossPointSettings::LP_MENU_KOSYNC, "LP_MENU_KOSYNC out of sync");
static_assert(rdr::settings::LP_MENU_DISABLED == CrossPointSettings::LP_MENU_DISABLED, "LP_MENU_DISABLED out of sync");
static_assert(rdr::settings::LP_MENU_BOOKMARK == CrossPointSettings::LP_MENU_BOOKMARK, "LP_MENU_BOOKMARK out of sync");

inline void applyOrientation(GfxRenderer& renderer, const uint8_t orientation) {
  switch (orientation) {
    case CrossPointSettings::ORIENTATION::PORTRAIT:
      renderer.setOrientation(GfxRenderer::Orientation::Portrait);
      break;
    case CrossPointSettings::ORIENTATION::LANDSCAPE_CW:
      renderer.setOrientation(GfxRenderer::Orientation::LandscapeClockwise);
      break;
    case CrossPointSettings::ORIENTATION::INVERTED:
      renderer.setOrientation(GfxRenderer::Orientation::PortraitInverted);
      break;
    case CrossPointSettings::ORIENTATION::LANDSCAPE_CCW:
      renderer.setOrientation(GfxRenderer::Orientation::LandscapeCounterClockwise);
      break;
    default:
      break;
  }
}

struct PageTurnResult {
  bool prev;
  bool next;
  bool fromTilt;
};

// Thin adapter: resolve all IO (SETTINGS, halTiltSensor, MappedInputManager
// edges) into the dependency-free rdr::PageTurnInputs, then delegate the actual
// prev/next/fromTilt logic to the pure, host-tested rdr::detectPageTurnCore.
// Behavior is identical to the previous inline implementation.
inline PageTurnResult detectPageTurn(const MappedInputManager& input) {
  using Button = MappedInputManager::Button;
  rdr::PageTurnInputs in;
  in.pageBackPressed = input.wasPressed(Button::PageBack);
  in.pageBackReleased = input.wasReleased(Button::PageBack);
  in.pageForwardPressed = input.wasPressed(Button::PageForward);
  in.pageForwardReleased = input.wasReleased(Button::PageForward);
  in.leftPressed = input.wasPressed(Button::Left);
  in.leftReleased = input.wasReleased(Button::Left);
  in.rightPressed = input.wasPressed(Button::Right);
  in.rightReleased = input.wasReleased(Button::Right);
  in.powerReleased = input.wasReleased(Button::Power);
  in.navSwapped = input.isNavDirectionSwapped();
  in.usePress = SETTINGS.longPressButtonBehavior == SETTINGS.OFF;
  in.tiltEnabled = SETTINGS.tiltPageTurn != CrossPointSettings::TILT_PAGE_TURN::TILT_OFF;
  // Preserve the original short-circuit: wasTiltedForward()/wasTiltedBack() are
  // CONSUMING edge reads (they clear the event flag). The previous inline code
  // was `SETTINGS.tiltPageTurn && halTiltSensor.wasTiltedForward()`, so the
  // sensor was NOT read (and the flag NOT consumed) when tilt is disabled. Only
  // read it when enabled to keep behavior byte-identical.
  if (in.tiltEnabled) {
    in.tiltForward = halTiltSensor.wasTiltedForward();
    in.tiltBack = halTiltSensor.wasTiltedBack();
  }
  in.powerIsPageTurn = SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::PAGE_TURN;

  const rdr::PageTurnDecision d = rdr::detectPageTurnCore(in);
  return {d.prev, d.next, d.fromTilt};
}

inline void displayWithRefreshCycle(const GfxRenderer& renderer, int& pagesUntilFullRefresh) {
  if (pagesUntilFullRefresh <= 1) {
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
  } else {
    renderer.displayBuffer();
    pagesUntilFullRefresh--;
  }
}

// Grayscale anti-aliasing pass. Renders content twice (LSB + MSB) to build
// the grayscale buffer. Only the content callback is re-rendered — status bars
// and other overlays should be drawn before calling this.
// Kept as a template to avoid std::function overhead; instantiated once per reader type.
template <typename RenderFn>
void renderAntiAliased(GfxRenderer& renderer, RenderFn&& renderFn) {
  if (!renderer.storeBwBuffer()) {
    LOG_ERR("READER", "Failed to store BW buffer for anti-aliasing");
    return;
  }

  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
  renderFn();
  renderer.copyGrayscaleLsbBuffers();

  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
  renderFn();
  renderer.copyGrayscaleMsbBuffers();

  renderer.displayGrayBuffer();
  renderer.setRenderMode(GfxRenderer::BW);

  renderer.restoreBwBuffer();
}

}  // namespace ReaderUtils
