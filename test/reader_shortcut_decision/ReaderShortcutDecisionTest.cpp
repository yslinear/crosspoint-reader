// Host-side regression test for the pure reader input decision cores in
// src/activities/reader/ReaderShortcutDecision.h.
//
// Two seams are covered, both pure (plain bools/ints/enums, no device deps):
//
//   SEAM 1 - rdr::detectPageTurnCore: the prev/next/fromTilt decision extracted
//            from ReaderUtils::detectPageTurn. The runtime function is now a thin
//            IO adapter; this test pins the actual direction logic.
//
//   SEAM 2 - rdr::resolveHeldButtonAction: the held-time -> action ladder
//            extracted from the two reader loops. This test pins the exact
//            threshold boundaries (the >= vs > asymmetry) and the gating rules.
//
// Each test names the regression it catches in a comment, so the value of the
// assertion is explicit and survives future edits.

#include <gtest/gtest.h>

#include "ReaderShortcutDecision.h"

using rdr::HeldAction;
using rdr::HeldButton;
using rdr::PageTurnDecision;
using rdr::PageTurnInputs;
using rdr::detectPageTurnCore;
using rdr::resolveHeldButtonAction;

namespace {

// Decoded settings literals (mirror CrossPointSettings; pinned to the real enum
// by static_assert at the ReaderUtils.h call site).
constexpr int LP_OFF = rdr::settings::LP_BEHAVIOR_OFF;                  // 0
constexpr int LP_CHAPTER_SKIP = rdr::settings::LP_BEHAVIOR_CHAPTER_SKIP;  // 1
constexpr int LP_ORIENT = rdr::settings::LP_BEHAVIOR_ORIENTATION_CHANGE;  // 2
constexpr int MENU_KOSYNC = rdr::settings::LP_MENU_KOSYNC;    // 0
constexpr int MENU_DISABLED = rdr::settings::LP_MENU_DISABLED;  // 1
constexpr int MENU_BOOKMARK = rdr::settings::LP_MENU_BOOKMARK;  // 2

// ===========================================================================
// SEAM 1 - detectPageTurnCore
// ===========================================================================

// A baseline in release mode (usePress=false): only button RELEASE edges drive
// page turns, no tilt, no swap, no power-as-page-turn.
PageTurnInputs releaseBase() {
  PageTurnInputs in;
  in.usePress = false;
  in.tiltEnabled = false;
  in.powerIsPageTurn = false;
  in.navSwapped = false;
  return in;
}

// --- Button-swap inversion (guards ReaderUtils.h:47-48, the single most likely
//     inversion bug) ---------------------------------------------------------

TEST(DetectPageTurn, NoSwap_LeftIsPrev_RightIsNext) {
  // Release mode, not swapped: Left release => prev, Right release => next.
  PageTurnInputs in = releaseBase();
  in.leftReleased = true;
  PageTurnDecision d = detectPageTurnCore(in);
  EXPECT_TRUE(d.prev);
  EXPECT_FALSE(d.next);

  in = releaseBase();
  in.rightReleased = true;
  d = detectPageTurnCore(in);
  EXPECT_FALSE(d.prev);
  EXPECT_TRUE(d.next);
}

TEST(DetectPageTurn, Swapped_RightIsPrev_LeftIsNext) {
  // navSwapped flips which of left/right drives prev vs next. If this inverts,
  // page turns go the wrong way in rotated orientations.
  PageTurnInputs in = releaseBase();
  in.navSwapped = true;
  in.rightReleased = true;
  PageTurnDecision d = detectPageTurnCore(in);
  EXPECT_TRUE(d.prev);  // right -> prev when swapped
  EXPECT_FALSE(d.next);

  in = releaseBase();
  in.navSwapped = true;
  in.leftReleased = true;
  d = detectPageTurnCore(in);
  EXPECT_FALSE(d.prev);
  EXPECT_TRUE(d.next);  // left -> next when swapped
}

// --- Tilt gating ------------------------------------------------------------

TEST(DetectPageTurn, TiltDisabled_NeverFires) {
  // tiltForward set but tiltEnabled=false => next=false, fromTilt=false.
  PageTurnInputs in = releaseBase();
  in.tiltForward = true;
  in.tiltBack = true;
  PageTurnDecision d = detectPageTurnCore(in);
  EXPECT_FALSE(d.next);
  EXPECT_FALSE(d.prev);
  EXPECT_FALSE(d.fromTilt);
}

TEST(DetectPageTurn, TiltEnabled_ForwardDrivesNext_BackDrivesPrev) {
  PageTurnInputs in = releaseBase();
  in.tiltEnabled = true;
  in.tiltForward = true;
  PageTurnDecision d = detectPageTurnCore(in);
  EXPECT_TRUE(d.next);
  EXPECT_FALSE(d.prev);
  EXPECT_TRUE(d.fromTilt);

  in = releaseBase();
  in.tiltEnabled = true;
  in.tiltBack = true;
  d = detectPageTurnCore(in);
  EXPECT_TRUE(d.prev);
  EXPECT_FALSE(d.next);
  EXPECT_TRUE(d.fromTilt);
}

// --- Tilt OR-dominance: fires with no button edge AND irrespective of usePress
//     (tilt is outside the usePress ternary) ----------------------------------

TEST(DetectPageTurn, TiltFiresWithNoButtonEdge) {
  PageTurnInputs in = releaseBase();
  in.tiltEnabled = true;
  in.tiltForward = true;
  // no button edges at all
  PageTurnDecision d = detectPageTurnCore(in);
  EXPECT_TRUE(d.next);
  EXPECT_TRUE(d.fromTilt);
}

TEST(DetectPageTurn, TiltIgnoresUsePressMode) {
  // Same tilt result whether usePress is true or false.
  PageTurnInputs in = releaseBase();
  in.tiltEnabled = true;
  in.tiltBack = true;
  in.usePress = false;
  PageTurnDecision a = detectPageTurnCore(in);
  in.usePress = true;
  PageTurnDecision b = detectPageTurnCore(in);
  EXPECT_TRUE(a.prev);
  EXPECT_TRUE(b.prev);
  EXPECT_EQ(a.prev, b.prev);
  EXPECT_TRUE(a.fromTilt);
  EXPECT_TRUE(b.fromTilt);
}

// --- Press-vs-release mode --------------------------------------------------

TEST(DetectPageTurn, UsePressReactsToPressedIgnoresReleased) {
  // usePress=true: *Pressed drives, *Released ignored. Covers pageBack/pageForward
  // AND left/right.
  PageTurnInputs in;
  in.usePress = true;
  in.pageBackReleased = true;       // ignored
  in.pageForwardReleased = true;    // ignored
  in.leftReleased = true;           // ignored
  in.rightReleased = true;          // ignored
  PageTurnDecision d = detectPageTurnCore(in);
  EXPECT_FALSE(d.prev);
  EXPECT_FALSE(d.next);

  in = PageTurnInputs{};
  in.usePress = true;
  in.pageBackPressed = true;
  d = detectPageTurnCore(in);
  EXPECT_TRUE(d.prev);

  in = PageTurnInputs{};
  in.usePress = true;
  in.pageForwardPressed = true;
  d = detectPageTurnCore(in);
  EXPECT_TRUE(d.next);

  in = PageTurnInputs{};
  in.usePress = true;
  in.leftPressed = true;  // not swapped -> prev
  d = detectPageTurnCore(in);
  EXPECT_TRUE(d.prev);

  in = PageTurnInputs{};
  in.usePress = true;
  in.rightPressed = true;  // not swapped -> next
  d = detectPageTurnCore(in);
  EXPECT_TRUE(d.next);
}

TEST(DetectPageTurn, ReleaseModeReactsToReleasedIgnoresPressed) {
  // usePress=false: the inverse. *Pressed ignored, *Released drives.
  PageTurnInputs in;
  in.usePress = false;
  in.pageBackPressed = true;     // ignored
  in.pageForwardPressed = true;  // ignored
  in.leftPressed = true;         // ignored
  in.rightPressed = true;        // ignored
  PageTurnDecision d = detectPageTurnCore(in);
  EXPECT_FALSE(d.prev);
  EXPECT_FALSE(d.next);

  in = PageTurnInputs{};
  in.usePress = false;
  in.pageBackReleased = true;
  d = detectPageTurnCore(in);
  EXPECT_TRUE(d.prev);

  in = PageTurnInputs{};
  in.usePress = false;
  in.leftReleased = true;
  d = detectPageTurnCore(in);
  EXPECT_TRUE(d.prev);

  in = PageTurnInputs{};
  in.usePress = false;
  in.rightReleased = true;
  d = detectPageTurnCore(in);
  EXPECT_TRUE(d.next);
}

// --- Power-as-page-turn -----------------------------------------------------

TEST(DetectPageTurn, PowerDrivesNextOnlyWhenPowerIsPageTurn) {
  // powerReleased=true => next only when powerIsPageTurn=true.
  PageTurnInputs in = releaseBase();
  in.powerReleased = true;
  in.powerIsPageTurn = false;
  PageTurnDecision d = detectPageTurnCore(in);
  EXPECT_FALSE(d.next);
  EXPECT_FALSE(d.prev);

  in = releaseBase();
  in.powerReleased = true;
  in.powerIsPageTurn = true;
  d = detectPageTurnCore(in);
  EXPECT_TRUE(d.next);
  EXPECT_FALSE(d.prev);  // power never drives prev
}

TEST(DetectPageTurn, PowerContributesToNextInsideBothTernaryBranches) {
  // Pin current behavior: power (powerIsPageTurn && powerReleased) is OR'd into
  // `next` inside BOTH the usePress and the !usePress branch. So power drives
  // next regardless of usePress, and regardless of swap.
  for (bool usePress : {true, false}) {
    for (bool swapped : {true, false}) {
      PageTurnInputs in;
      in.usePress = usePress;
      in.navSwapped = swapped;
      in.powerIsPageTurn = true;
      in.powerReleased = true;
      PageTurnDecision d = detectPageTurnCore(in);
      EXPECT_TRUE(d.next) << "usePress=" << usePress << " swapped=" << swapped;
      EXPECT_FALSE(d.prev);
    }
  }
}

// --- fromTilt independent of prev/next button edges -------------------------

TEST(DetectPageTurn, FromTiltSetOnlyByTilt) {
  // Heavy button activity but no tilt => fromTilt stays false.
  PageTurnInputs in;
  in.usePress = false;
  in.leftReleased = true;
  in.rightReleased = true;
  in.pageBackReleased = true;
  in.pageForwardReleased = true;
  in.powerReleased = true;
  in.powerIsPageTurn = true;
  PageTurnDecision d = detectPageTurnCore(in);
  EXPECT_TRUE(d.prev);
  EXPECT_TRUE(d.next);
  EXPECT_FALSE(d.fromTilt);  // no tilt -> not from tilt, even with both directions firing
}

// --- constexpr static_assert on representative vectors (zero runtime cost) ---
static_assert(detectPageTurnCore([] {
                PageTurnInputs in;
                in.usePress = false;
                in.leftReleased = true;
                return in;
              }())
                  .prev,
              "left release => prev when not swapped");
static_assert(detectPageTurnCore([] {
                PageTurnInputs in;
                in.usePress = false;
                in.navSwapped = true;
                in.leftReleased = true;
                return in;
              }())
                  .next,
              "left release => next when swapped");
static_assert(detectPageTurnCore([] {
                PageTurnInputs in;
                in.tiltEnabled = true;
                in.tiltForward = true;
                return in;
              }())
                  .fromTilt,
              "tilt forward enabled => fromTilt");

// ===========================================================================
// SEAM 2 - resolveHeldButtonAction (threshold boundaries)
// ===========================================================================

// --- BACK long-press: GO_HOME >= 1000 (inclusive) ---------------------------

TEST(ResolveHeld, BackLongPress_GoHomeBoundary_999_NotFileBrowser) {
  // 999 held, still pressed: FileBrowser NOT yet triggered (held < 1000).
  EXPECT_EQ(resolveHeldButtonAction(HeldButton::Back, 999, /*released=*/false, LP_OFF, MENU_DISABLED, false),
            HeldAction::None);
}

TEST(ResolveHeld, BackLongPress_GoHomeBoundary_1000_FileBrowser) {
  // 1000 held, still pressed: FileBrowser triggers (>= inclusive). Pinning 1000
  // vs 999 guards against an off-by-one flip of the inclusive comparison.
  EXPECT_EQ(resolveHeldButtonAction(HeldButton::Back, 1000, /*released=*/false, LP_OFF, MENU_DISABLED, false),
            HeldAction::FileBrowser);
}

TEST(ResolveHeld, BackShortPress_ReleasedUnder1000_GoHome) {
  // Released with held < 1000 => GoHome.
  EXPECT_EQ(resolveHeldButtonAction(HeldButton::Back, 999, /*released=*/true, LP_OFF, MENU_DISABLED, false),
            HeldAction::GoHome);
  // Released exactly at 1000 is NOT a short press (held not < 1000) => None.
  EXPECT_EQ(resolveHeldButtonAction(HeldButton::Back, 1000, /*released=*/true, LP_OFF, MENU_DISABLED, false),
            HeldAction::None);
}

// --- CONFIRM Bookmark: >= 400 (inclusive) -----------------------------------

TEST(ResolveHeld, Bookmark_Boundary_399_None_400_Bookmark) {
  EXPECT_EQ(resolveHeldButtonAction(HeldButton::Confirm, 399, /*released=*/false, LP_OFF, MENU_BOOKMARK, false),
            HeldAction::None);
  EXPECT_EQ(resolveHeldButtonAction(HeldButton::Confirm, 400, /*released=*/false, LP_OFF, MENU_BOOKMARK, false),
            HeldAction::Bookmark);
}

// --- CONFIRM KoSync: >= 1000 (inclusive), only when menuFn == KOSYNC ---------

TEST(ResolveHeld, KoSync_Boundary_999_None_1000_KoSync) {
  EXPECT_EQ(resolveHeldButtonAction(HeldButton::Confirm, 999, /*released=*/false, LP_OFF, MENU_KOSYNC, false),
            HeldAction::None);
  EXPECT_EQ(resolveHeldButtonAction(HeldButton::Confirm, 1000, /*released=*/false, LP_OFF, MENU_KOSYNC, false),
            HeldAction::KoSync);
}

// --- SKIP: strict > 700 (the regression magnet) -----------------------------

TEST(ResolveHeld, ChapterSkip_StrictBoundary_699_700_701) {
  // 699 => None; 700 => None (STRICT '>', not '>='); 701 => ChapterSkip.
  EXPECT_EQ(resolveHeldButtonAction(HeldButton::PageTurn, 699, false, LP_CHAPTER_SKIP, MENU_DISABLED,
                                    /*pageTurnTriggered=*/true),
            HeldAction::None);
  EXPECT_EQ(resolveHeldButtonAction(HeldButton::PageTurn, 700, false, LP_CHAPTER_SKIP, MENU_DISABLED, true),
            HeldAction::None)
      << "700 must be None: SKIP uses strict '>', flipping to '>=' breaks here";
  EXPECT_EQ(resolveHeldButtonAction(HeldButton::PageTurn, 701, false, LP_CHAPTER_SKIP, MENU_DISABLED, true),
            HeldAction::ChapterSkip);
}

TEST(ResolveHeld, OrientationChange_StrictBoundary) {
  EXPECT_EQ(resolveHeldButtonAction(HeldButton::PageTurn, 700, false, LP_ORIENT, MENU_DISABLED, true),
            HeldAction::None);
  EXPECT_EQ(resolveHeldButtonAction(HeldButton::PageTurn, 701, false, LP_ORIENT, MENU_DISABLED, true),
            HeldAction::OrientationChange);
}

// --- Gating: page-turn-gated behaviors require pageTurnTriggered -------------

TEST(ResolveHeld, ChapterSkip_RequiresPageTurnTriggered) {
  // pageTurnTriggered=false => None even past 700.
  EXPECT_EQ(resolveHeldButtonAction(HeldButton::PageTurn, 5000, false, LP_CHAPTER_SKIP, MENU_DISABLED,
                                    /*pageTurnTriggered=*/false),
            HeldAction::None);
  EXPECT_EQ(resolveHeldButtonAction(HeldButton::PageTurn, 5000, false, LP_ORIENT, MENU_DISABLED, false),
            HeldAction::None);
}

// --- Mode coverage ----------------------------------------------------------

TEST(ResolveHeld, BehaviorOff_NeverChapterSkipOrOrientation) {
  // behavior==OFF => never ChapterSkip/OrientationChange regardless of held.
  EXPECT_EQ(resolveHeldButtonAction(HeldButton::PageTurn, 5000, false, LP_OFF, MENU_DISABLED, true),
            HeldAction::None);
}

TEST(ResolveHeld, MenuDisabled_ConfirmNeverBookmarkOrKoSync) {
  // menuFn==DISABLED => Confirm never Bookmark/KoSync.
  EXPECT_EQ(resolveHeldButtonAction(HeldButton::Confirm, 5000, false, LP_OFF, MENU_DISABLED, false),
            HeldAction::None);
}

TEST(ResolveHeld, ConfirmLadderOnlyWhileHeld_NotOnRelease) {
  // Bookmark/KoSync fire only while still held (released=false). On release,
  // Confirm carries no held action here (the loop opens the menu instead).
  EXPECT_EQ(resolveHeldButtonAction(HeldButton::Confirm, 1000, /*released=*/true, LP_OFF, MENU_BOOKMARK, false),
            HeldAction::None);
  EXPECT_EQ(resolveHeldButtonAction(HeldButton::Confirm, 1000, /*released=*/true, LP_OFF, MENU_KOSYNC, false),
            HeldAction::None);
}

TEST(ResolveHeld, BookmarkVsKoSync_SelectedByMenuFunction) {
  // Same hold time, different menuFn => different intent.
  EXPECT_EQ(resolveHeldButtonAction(HeldButton::Confirm, 1000, false, LP_OFF, MENU_BOOKMARK, false),
            HeldAction::Bookmark);  // bookmark fires at >=400, so 1000 is Bookmark
  EXPECT_EQ(resolveHeldButtonAction(HeldButton::Confirm, 1000, false, LP_OFF, MENU_KOSYNC, false),
            HeldAction::KoSync);
}

// --- constexpr static_asserts: boundary decisions usable at compile time -----
static_assert(resolveHeldButtonAction(HeldButton::PageTurn, 700, false, LP_CHAPTER_SKIP, MENU_DISABLED, true) ==
                  HeldAction::None,
              "SKIP boundary 700 is strict '>' -> None");
static_assert(resolveHeldButtonAction(HeldButton::PageTurn, 701, false, LP_CHAPTER_SKIP, MENU_DISABLED, true) ==
                  HeldAction::ChapterSkip,
              "SKIP boundary 701 -> ChapterSkip");
static_assert(resolveHeldButtonAction(HeldButton::Back, 1000, false, LP_OFF, MENU_DISABLED, false) ==
                  HeldAction::FileBrowser,
              "GO_HOME boundary 1000 inclusive -> FileBrowser");
static_assert(resolveHeldButtonAction(HeldButton::Confirm, 400, false, LP_OFF, MENU_BOOKMARK, false) ==
                  HeldAction::Bookmark,
              "Bookmark boundary 400 inclusive -> Bookmark");

}  // namespace
