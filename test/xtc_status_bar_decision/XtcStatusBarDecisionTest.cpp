// Host-side regression test for the pure XTC status-bar overlay decision in
// src/activities/reader/XtcStatusBarDecision.h.
//
// Background (the regression this guards against):
// XtcReaderActivity has three render paths. The two 2-bit (XTH grayscale) paths
// display the page themselves and return BEFORE the 1-bit path's shared
// status-bar overlay, so each 2-bit path must explicitly draw the overlay. A
// new banded 2-bit path was added that returned WITHOUT drawing the status bar
// -> the XTC status bar silently disappeared on XTH portrait pages.
//
// TWO-LAYER GUARD (read this to avoid the tautology trap):
//
//   Layer 1 - structural / routing guard (NOT this test):
//     Every 2-bit render path now funnels through a single call site,
//     XtcReaderActivity::drawConfiguredStatusBarOverlay() (whole-page 2-bit path
//     at XtcReaderActivity.cpp:403, banded 2-bit path at :606). This single
//     shared call site is what prevents any path from silently skipping the
//     overlay. It is enforced by code review of that one call site at compile
//     time, NOT by any host test -- a unit test cannot prove "every future
//     render path calls the helper" without compiling and driving the full
//     device render stack (GfxRenderer, HalDisplay, the XTC decoder, the strip
//     framebuffer machinery).
//
//   Layer 2 - decision guard (THIS test):
//     The helper's only branch is the pure mode -> draw decision. This test
//     pins that decision: a visible mode (BOTTOM/TOP) MUST produce a draw, HIDE
//     must not. So WHEN the single call site is reached, a visible mode always
//     draws. The test guards the decision; the refactor guards the routing.
//
// IMPORTANT: This test does NOT prove that renderPage2BitBanded calls the
// helper -- that is guaranteed structurally by the single call site (Layer 1).
// This test proves that WHEN the helper is called, it draws for every visible
// mode (Layer 2). overlayDraws() isolates the helper's exact draw-or-not switch
// so a regression that makes a visible mode stop drawing breaks here.
//
// No device deps: depends only on the pure header (a plain int -> enum switch).

#include <gtest/gtest.h>

#include "XtcStatusBarDecision.h"

using xtcsb::Overlay;
using xtcsb::overlayDraws;
using xtcsb::overlayFor;

namespace {

// Mirror CrossPointSettings::XTC_STATUS_BAR_MODE so the test reads in terms of
// the real configuration values, not bare literals. Kept local to avoid pulling
// in the device settings header (and its HalStorage transitive deps).
enum XtcStatusBarMode {
  XTC_STATUS_BAR_HIDE = 0,
  XTC_STATUS_BAR_BOTTOM = 1,
  XTC_STATUS_BAR_TOP = 2,
};

// --- The core regression guard ---------------------------------------------
// A visible status-bar mode MUST produce an overlay. If either of these returns
// None, the 2-bit render paths draw nothing and the status bar disappears.

TEST(XtcStatusBarOverlay, BottomModeDrawsBottomOverlay) {
  EXPECT_EQ(overlayFor(XTC_STATUS_BAR_BOTTOM), Overlay::Bottom);
  // Strongest form of the regression assertion: a visible mode is never None.
  EXPECT_NE(overlayFor(XTC_STATUS_BAR_BOTTOM), Overlay::None);
}

TEST(XtcStatusBarOverlay, TopModeDrawsTopOverlay) {
  EXPECT_EQ(overlayFor(XTC_STATUS_BAR_TOP), Overlay::Top);
  EXPECT_NE(overlayFor(XTC_STATUS_BAR_TOP), Overlay::None);
}

// --- Hide mode draws nothing ------------------------------------------------

TEST(XtcStatusBarOverlay, HideModeDrawsNoOverlay) {
  EXPECT_EQ(overlayFor(XTC_STATUS_BAR_HIDE), Overlay::None);
}

// --- Defensive: unknown / out-of-range modes never invent an overlay --------

TEST(XtcStatusBarOverlay, UnknownModeDrawsNoOverlay) {
  EXPECT_EQ(overlayFor(3), Overlay::None);    // XTC_STATUS_BAR_MODE_COUNT
  EXPECT_EQ(overlayFor(-1), Overlay::None);   // negative junk
  EXPECT_EQ(overlayFor(999), Overlay::None);  // far out of range
}

// --- The helper's draw-or-not contract (Layer 2) ----------------------------
// overlayDraws(mode) is the exact decision the helper switch encodes: does a
// configured mode cause renderStatusBarOverlay + displayBuffer, or nothing? This
// is the "WHEN called, does it draw?" half of the two-layer guard. A regression
// that makes a visible mode stop drawing (the bug's shape) flips one of these.

TEST(XtcStatusBarOverlay, VisibleModesDraw_HideDoesNot) {
  EXPECT_TRUE(overlayDraws(XTC_STATUS_BAR_BOTTOM));
  EXPECT_TRUE(overlayDraws(XTC_STATUS_BAR_TOP));
  EXPECT_FALSE(overlayDraws(XTC_STATUS_BAR_HIDE));
}

TEST(XtcStatusBarOverlay, UnknownModesDoNotDraw) {
  // Defensive: out-of-range modes draw nothing (never invent a draw).
  EXPECT_FALSE(overlayDraws(3));
  EXPECT_FALSE(overlayDraws(-1));
  EXPECT_FALSE(overlayDraws(999));
}

// --- Compile-time guarantee: the decision is usable in constexpr context ----
// Keeps the function pure/constexpr so it can be inlined in the render path
// with zero runtime cost.
static_assert(overlayFor(XTC_STATUS_BAR_BOTTOM) == Overlay::Bottom, "BOTTOM must map to Bottom");
static_assert(overlayFor(XTC_STATUS_BAR_TOP) == Overlay::Top, "TOP must map to Top");
static_assert(overlayFor(XTC_STATUS_BAR_HIDE) == Overlay::None, "HIDE must map to None");

// The helper's draw decision (Layer 2) is also a compile-time guarantee.
static_assert(overlayDraws(XTC_STATUS_BAR_BOTTOM), "BOTTOM must draw");
static_assert(overlayDraws(XTC_STATUS_BAR_TOP), "TOP must draw");
static_assert(!overlayDraws(XTC_STATUS_BAR_HIDE), "HIDE must not draw");

}  // namespace
