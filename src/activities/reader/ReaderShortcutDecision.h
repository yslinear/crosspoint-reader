/**
 * ReaderShortcutDecision.h
 *
 * Pure, host-testable decision cores carved out of the reader input path,
 * mirroring how xtcsb::overlayFor (XtcStatusBarDecision.h) was carved out of the
 * render path.
 *
 * Two seams live here, both IO-free and constexpr (plain bools/ints/enums, no
 * MappedInputManager / SETTINGS / halTiltSensor / device includes), so they can
 * be unit-tested on the host with zero runtime cost when inlined:
 *
 *   SEAM 1 - detectPageTurnCore: the prev/next/fromTilt logic that used to be
 *            inlined in ReaderUtils::detectPageTurn. The runtime function now
 *            resolves all IO (SETTINGS, halTiltSensor, MappedInputManager edges)
 *            into a PageTurnInputs and delegates here.
 *
 *   SEAM 2 - resolveHeldButtonAction: the held-time -> action ladder that used to
 *            be scattered as inline getHeldTime() comparisons across
 *            XtcReaderActivity::loop and EpubReaderActivity::loop. The loops now
 *            resolve their inputs and dispatch on the returned HeldAction.
 *
 * Thresholds mirror ReaderUtils.h:
 *   GO_HOME_MS = 1000  (>= inclusive)   BOOKMARK_HOLD_MS = 400 (>= inclusive)
 *   SKIP_HOLD_MS = 700 (>  strict)
 *
 * The strict-vs-inclusive asymmetry (SKIP uses '>', the rest use '>=') is the
 * regression magnet pinned by the boundary tests.
 *
 * Settings/menu enum values are passed as decoded ints, not the device enums, to
 * keep this header dependency-free. They mirror CrossPointSettings:
 *   longPressButtonBehavior: OFF=0, CHAPTER_SKIP=1, ORIENTATION_CHANGE=2
 *   longPressMenuFunction:   LP_MENU_KOSYNC=0, LP_MENU_DISABLED=1, LP_MENU_BOOKMARK=2
 * The call sites static_assert these literals against the real enum.
 */

#pragma once

namespace rdr {

// --- SEAM 1: page-turn direction decision -----------------------------------

// All IO already resolved by the caller (edge reads, sensor reads, decoded
// settings flags). 1:1 with the original ReaderUtils::detectPageTurn body.
struct PageTurnInputs {
  // MappedInputManager edge reads (already resolved by caller).
  bool pageBackPressed = false;
  bool pageBackReleased = false;
  bool pageForwardPressed = false;
  bool pageForwardReleased = false;
  bool leftPressed = false;
  bool leftReleased = false;
  bool rightPressed = false;
  bool rightReleased = false;
  bool powerReleased = false;
  bool navSwapped = false;  // input.isNavDirectionSwapped()
  // Resolved sensor reads.
  bool tiltForward = false;
  bool tiltBack = false;
  // Settings flags (passed as decoded bools, not raw enums).
  bool usePress = false;        // longPressButtonBehavior == OFF
  bool tiltEnabled = false;     // tiltPageTurn != TILT_OFF
  bool powerIsPageTurn = false; // shortPwrBtn == PAGE_TURN
};

struct PageTurnDecision {
  bool prev = false;
  bool next = false;
  bool fromTilt = false;
};

// Pure 1:1 port of ReaderUtils.h:43-59 (no behavior change).
//
// Notes pinned by the tests:
//  - navSwapped flips which of left/right drives prev vs next.
//  - tilt is OR'd OUTSIDE the usePress ternary (fires regardless of press mode).
//  - power-as-page-turn (powerIsPageTurn && powerReleased) contributes to `next`
//    inside BOTH ternary branches; it never drives prev and ignores swap.
//  - fromTilt is set only by tilt, independent of any button edge.
constexpr PageTurnDecision detectPageTurnCore(const PageTurnInputs& in) {
  const bool tiltNext = in.tiltEnabled && in.tiltForward;
  const bool tiltPrev = in.tiltEnabled && in.tiltBack;

  const bool prevBtnPressed = in.navSwapped ? in.rightPressed : in.leftPressed;
  const bool prevBtnReleased = in.navSwapped ? in.rightReleased : in.leftReleased;
  const bool nextBtnPressed = in.navSwapped ? in.leftPressed : in.rightPressed;
  const bool nextBtnReleased = in.navSwapped ? in.leftReleased : in.rightReleased;

  const bool prev =
      tiltPrev || (in.usePress ? (in.pageBackPressed || prevBtnPressed)
                               : (in.pageBackReleased || prevBtnReleased));

  const bool powerTurn = in.powerIsPageTurn && in.powerReleased;

  const bool next =
      tiltNext || (in.usePress ? (in.pageForwardPressed || powerTurn || nextBtnPressed)
                               : (in.pageForwardReleased || powerTurn || nextBtnReleased));

  return {prev, next, tiltPrev || tiltNext};
}

// --- SEAM 2: held-button action ladder --------------------------------------

// Which logical button the hold is on.
//  - Back/Confirm carry button-specific hold semantics (file browser / home /
//    bookmark / kosync).
//  - PageTurn means "the hold is on whichever button drove the page turn this
//    frame" and only participates in the page-turn-gated behaviors (chapter
//    skip / orientation change). It deliberately does NOT trigger the Back/Confirm
//    ladders, so a long page-turn hold can never be mistaken for a file-browser
//    or home request.
enum class HeldButton { Back, Confirm, PageTurn };

enum class HeldAction { None, GoHome, FileBrowser, Bookmark, ChapterSkip, OrientationChange, KoSync };

// Mirror of CrossPointSettings enums, decoded to ints to keep this header
// dependency-free. The call sites static_assert these against the real enum.
namespace settings {
// LONG_PRESS_BUTTON_BEHAVIOR
constexpr int LP_BEHAVIOR_OFF = 0;
constexpr int LP_BEHAVIOR_CHAPTER_SKIP = 1;
constexpr int LP_BEHAVIOR_ORIENTATION_CHANGE = 2;
// LONG_PRESS_MENU_FUNCTION
constexpr int LP_MENU_KOSYNC = 0;
constexpr int LP_MENU_DISABLED = 1;
constexpr int LP_MENU_BOOKMARK = 2;
}  // namespace settings

// Thresholds (mirror ReaderUtils.h:12-14). Duplicated here so the core stays
// dependency-free; the runtime ReaderUtils constants and these are pinned to
// each other by static_assert at the ReaderUtils.h call site.
constexpr unsigned long HELD_GO_HOME_MS = 1000;   // >= inclusive
constexpr unsigned long HELD_SKIP_HOLD_MS = 700;  // >  strict
constexpr unsigned long HELD_BOOKMARK_HOLD_MS = 400;  // >= inclusive

// Pure held-button resolver. Pins the control flow of the two reader loops'
// held-time ladders into one place (XtcReaderActivity.cpp:82-111,
// EpubReaderActivity.cpp:299-424).
//
// Asymmetry pinned by the tests: GO_HOME / Bookmark / KoSync use '>=' (inclusive);
// SKIP uses '>' (strict). The 700 boundary is where '>' vs '>=' flips behavior.
//
// LIMITATION (kept out of this pure core, owned by loop()):
//  - ignoreNextConfirmRelease bookkeeping.
//  - KoSync fall-through-to-menu when launchKOReaderSync() returns false: this
//    returns KoSync as the *intent*; the loop owns the "sync couldn't run -> open
//    menu" recovery.
//  - The screenshot-suppression raw-GPIO guard (POWER+DOWN released) is outside
//    both seams; it stays in loop().
constexpr HeldAction resolveHeldButtonAction(HeldButton button, unsigned long heldTimeMs,
                                             bool released, int longPressButtonBehavior,
                                             int longPressMenuFunction, bool pageTurnTriggered) {
  // BACK ladder (XtcReaderActivity.cpp:82-89, EpubReaderActivity.cpp:328-335).
  if (button == HeldButton::Back) {
    if (!released && heldTimeMs >= HELD_GO_HOME_MS) {
      return HeldAction::FileBrowser;  // long-press while still held
    }
    if (released && heldTimeMs < HELD_GO_HOME_MS) {
      return HeldAction::GoHome;  // short press on release
    }
  }

  // CONFIRM long-press menu function ladder (EpubReaderActivity.cpp:299-325).
  // Only fires while still held (isPressed), never on release.
  if (button == HeldButton::Confirm && !released) {
    if (longPressMenuFunction == settings::LP_MENU_BOOKMARK && heldTimeMs >= HELD_BOOKMARK_HOLD_MS) {
      return HeldAction::Bookmark;
    }
    if (longPressMenuFunction == settings::LP_MENU_KOSYNC && heldTimeMs >= HELD_GO_HOME_MS) {
      return HeldAction::KoSync;
    }
  }

  // Page-turn-gated long-press behaviors (XtcReaderActivity.cpp:110-111,
  // EpubReaderActivity.cpp:388-424). These sit AFTER the detectPageTurn
  // early-return, so they require an actual page turn firing this frame, AND a
  // strict held > SKIP_HOLD_MS.
  if (pageTurnTriggered && heldTimeMs > HELD_SKIP_HOLD_MS) {
    if (longPressButtonBehavior == settings::LP_BEHAVIOR_CHAPTER_SKIP) {
      return HeldAction::ChapterSkip;
    }
    if (longPressButtonBehavior == settings::LP_BEHAVIOR_ORIENTATION_CHANGE) {
      return HeldAction::OrientationChange;
    }
  }

  return HeldAction::None;
}

}  // namespace rdr
