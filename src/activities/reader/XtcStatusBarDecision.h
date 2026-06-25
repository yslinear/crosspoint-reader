/**
 * XtcStatusBarDecision.h
 *
 * Single source of truth for "given the configured XTC status-bar mode, what
 * status-bar overlay should ANY render path draw after putting the page on the
 * panel?"
 *
 * This is the decision seam that every XtcReaderActivity 2-bit render path is
 * funnelled through (see drawConfiguredStatusBarOverlay), so that no new render
 * path can silently forget to draw the overlay. It is intentionally pure and
 * IO-free (takes a plain int, no device includes) so it can be unit-tested on
 * the host. The int values mirror CrossPointSettings::XTC_STATUS_BAR_MODE
 * (HIDE=0, BOTTOM=1, TOP=2); the call site static_asserts the enum against
 * these literals.
 */

#pragma once

namespace xtcsb {

enum class Overlay { None, Top, Bottom };

// Maps the configured XTC status-bar mode to the overlay that must be drawn.
// Values mirror CrossPointSettings::XTC_STATUS_BAR_MODE:
//   0 = XTC_STATUS_BAR_HIDE   -> None
//   1 = XTC_STATUS_BAR_BOTTOM -> Bottom
//   2 = XTC_STATUS_BAR_TOP    -> Top
// Any unknown value defends to None (no overlay) rather than guessing.
constexpr Overlay overlayFor(int xtcStatusBarMode) {
  switch (xtcStatusBarMode) {
    case 2:  // XTC_STATUS_BAR_TOP
      return Overlay::Top;
    case 1:  // XTC_STATUS_BAR_BOTTOM
      return Overlay::Bottom;
    default:  // XTC_STATUS_BAR_HIDE / unknown
      return Overlay::None;
  }
}

// The exact mode -> draw-or-not decision encoded by
// XtcReaderActivity::drawConfiguredStatusBarOverlay()'s switch: a configured
// mode causes a status-bar draw iff it maps to a non-None overlay. Isolated here
// so the helper's draw decision is host-testable without pulling the render
// stack. A future edit that makes a visible mode map to None (the regression
// shape) breaks both overlayFor and overlayDraws.
constexpr bool overlayDraws(int xtcStatusBarMode) { return overlayFor(xtcStatusBarMode) != Overlay::None; }

}  // namespace xtcsb
