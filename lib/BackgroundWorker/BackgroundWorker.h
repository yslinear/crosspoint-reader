#pragma once

// BackgroundWorker — a single low-priority FreeRTOS task that runs deferrable,
// cancellable, memory-gated background jobs (cover generation, glyph prewarm)
// WITHOUT ever stealing CPU, SD bus, or heap from the foreground reading
// experience on a single-core ESP32-C3.
//
// Two job kinds are implemented: JOB 1 cover-thumbnail generation, and JOB 3
// glyph prewarm (byte-budgeted against kMaxHeldBytes — see the prewarm handler).
// (Background chapter pre-indexing was removed: it held the RenderLock across a
// whole CJK chapter parse and raced the foreground section.bin write. The
// foreground N+1 pre-index in EpubReaderActivity covers chapter crossings safely
// on the render task instead.)
//
// Safety contract (see CLAUDE.md + design):
//   S1  ONE worker task at priority 0 (tskIDLE_PRIORITY), strictly below the
//       UI render task and the Arduino loopTask (both priority 1). A page turn
//       preempts the worker at the next scheduler tick.
//   S2  ALL SD access goes through HalStorage. Jobs do work in small chunks,
//       releasing the storageMutex + yielding between chunks (chunkedSdGuard),
//       so a foreground page turn waits at most ~one chunk, never a whole job.
//   S3  NO use-after-free: a Job is a fixed-size POD that COPIES everything it
//       needs (path, indices) at enqueue time. It NEVER holds an Activity*.
//       The worker is a process-lifetime singleton; it outlives all activities.
//   S4  HEAP GATE: before ANY background allocation, heapGate() keeps a
//       foreground safety floor (~96 KB). Under pressure the worker DEFERS the
//       job rather than risk a foreground OOM. All allocs via makeUniqueNoThrow.
//   S5  PAUSE-ON-INPUT + cancellation: shouldPauseForInput() is checked between
//       chunks; recent input defers the remainder. cancel()/setContext() drop
//       stale jobs (chapter jump, book/home exit).
//   S6  DEFERRED START: a job enqueued on an activity event does not run during
//       the foreground render; it starts only after a short idle window
//       (kIdleBeforeRunMs) so it overlaps reading, not the render.
//   S7  BOUNDED + no spin: fixed-capacity POD queue; the task BLOCKS on a task
//       notification when idle (zero CPU, battery-friendly). Total
//       background-held cache has a hard cap (kMaxHeldBytes).

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <cstddef>
#include <cstdint>

class GfxRenderer;

class BackgroundWorker {
 public:
  // Job kinds. Each maps to a per-kind handler (runJob dispatch). All handlers
  // are implemented (see the .cpp).
  enum class Kind : uint8_t {
    None = 0,
    GenerateCover,    // JOB 1: render a book's home-screen cover thumbnail
    PrewarmGlyphs,    // JOB 3: warm SD-font glyph bitmaps for upcoming pages
  };

  // Higher value = runs first. Within the same priority, jobs run FIFO.
  enum class Priority : uint8_t {
    Low = 0,     // opportunistic (glyph prewarm of pages further out)
    Normal = 1,  // default (cover gen)
    High = 2,    // user is likely to need this very soon
  };

  // POD descriptor — copied BY VALUE at enqueue. Fixed-size, no heap, no
  // pointers into activity-owned state (S3). `a`/`b` carry kind-specific scalars
  // (e.g. font id, target height). `estBytes` is the worst-case allocation this
  // job will attempt, used by the heap gate (S4). `glyphs` carries Job #3's
  // copied upcoming-page codepoints (UTF-8, NUL-term).
  struct Job {
    Kind kind = Kind::None;
    Priority priority = Priority::Normal;
    uint32_t contextId = 0;            // cancellation key (e.g. book hash)
    uint32_t seq = 0;                  // FIFO tiebreaker within a priority
    char path[128] = {};              // copied, null-terminated source EPUB path
    uint32_t a = 0;                    // kind-specific (e.g. fontId)
    uint32_t b = 0;                    // kind-specific (e.g. cover height)
    size_t estBytes = 0;               // worst-case alloc for the heap gate (S4)
    char glyphs[512] = {};             // JOB 3: copied upcoming codepoints (UTF-8)
  };

  static BackgroundWorker& getInstance() { return instance; }

  // Create the priority-0 worker task. Call ONCE from main.cpp after
  // Storage.begin() succeeds. Idempotent: a second call is a no-op.
  //
  // `renderer` is the single, process-lifetime GfxRenderer (created in main.cpp).
  // Job #3 needs it for SD-font glyph prewarm. It is borrowed, never owned, and
  // outlives the worker (both are static/global), so there is no lifetime hazard
  // (S3). Job #3 serializes its use of the shared renderer / SD-font state against
  // the foreground render task via the global RenderLock at the call boundary
  // (see the .cpp handler).
  void begin(GfxRenderer& renderer);

  // Render-serialization hooks. The global rendering mutex (RenderLock) lives in
  // src/, which lib/ must not include. main.cpp registers a take/give pair here
  // so Job #3 can serialize its use of the shared renderer / SD-font state
  // against the foreground render task WITHOUT lib/ depending on src/. Plain
  // function pointers (no std::function) per the no-bloat rule. If unset, the
  // handler runs without the lock (degraded but safe in a single-render-task
  // build only — always set in production).
  void setRenderLockHooks(void (*lockFn)(), void (*unlockFn)()) {
    renderLockFn = lockFn;
    renderUnlockFn = unlockFn;
  }

  // Enqueue a COPY of `job`. Never blocks the caller. Returns false if the
  // queue is full (S7) or `job.kind` is None. `seq` is assigned internally so
  // callers leave it zero. Safe to call from the loopTask / activities.
  bool enqueue(const Job& job);

  // --- Typed enqueue helpers (build the POD; copy everything — S3) -------------

  // JOB 1. Generate the home-screen cover thumbnail for `epubPath` at `height`.
  // Enqueue from reader onEnter only if the thumb is not already cached.
  bool enqueueCoverGen(const char* epubPath, uint32_t bookHash, int height);

  // JOB 3. Prewarm SD-font glyph bitmaps for the codepoints in `utf8Codepoints`
  // (the next pages' text, already copied from laid-out PageLines — S3).
  bool enqueuePrewarmGlyphs(const char* epubPath, uint32_t bookHash, int fontId, const char* utf8Codepoints);

  // --- Cancellation / context lifecycle (S5) ----------------------------------

  // Set the active context (e.g. current book hash). Drops every queued job
  // whose contextId differs, and marks any in-flight job of a stale context to
  // abort at its next chunk boundary. Call on book open / chapter context
  // changes so the worker never warms data the user has navigated away from.
  void setContext(uint32_t contextId);

  // Drop queued jobs matching a predicate (return true to drop). Also requests
  // abort of the in-flight job if the predicate matches it. The predicate runs
  // under the queue lock; keep it trivial (compare fields only, no SD/heap).
  // Returns the number of queued jobs dropped.
  size_t cancel(bool (*pred)(const Job&, void* ctx), void* ctx);

  // Convenience: drop everything queued and abort the in-flight job (S5,
  // e.g. on reader onExit before the shared Epub is freed).
  void cancelAll();

  // Drop queued jobs whose contextId != `keepContextId`. In-flight job of a
  // stale context is asked to abort. Returns number dropped.
  size_t cancelStale(uint32_t keepContextId);

  // Drop queued jobs of a given kind and abort the in-flight job if it is that
  // kind (S5). General-purpose selective cancellation. Returns number dropped.
  size_t cancelKind(Kind kind);

  // --- Pause-on-input / deferred-start signal (S5/S6) -------------------------

  // Record that the user just interacted. Single writer = the loopTask, called
  // once per frame from main.cpp's existing activity-detection block. Lock-free
  // 32-bit store (single-writer / single-reader); no mutex, no ISR.
  static void noteUserInput() { lastInputMillis = millisNow(); }

  // True while the user is interacting or just stopped (within kIdleBeforeRunMs).
  // The worker checks this before starting a job (S6) and between chunks (S5).
  static bool shouldPauseForInput();

  // --- Heap gate (S4) ---------------------------------------------------------

  // True iff it is safe to allocate `neededBytes` in the background right now:
  // the largest contiguous block can hold it AND free heap stays at/above the
  // foreground safety floor afterwards. Defaults to kForegroundFloor.
  // Fragmentation, not total free, decides allocation success — so this checks
  // getMaxAllocHeap(), not just getFreeHeap().
  static bool heapGate(size_t neededBytes, size_t floorBytes = kForegroundFloor);

  // --- Reusable chunked-SD helper (S2) ----------------------------------------

  // Run `step` repeatedly until it reports completion, yielding between calls so
  // a foreground page turn interleaves at chunk granularity. `step` should do at
  // most ONE small mutex-released SD operation (e.g. one HalFile::read of a few
  // KB) per call and return a Step. Returns true if the work completed, false if
  // it was deferred/aborted (input arrived, context changed, or step errored).
  //
  //   step signature: Step (*)(void* ctx)
  //   ctx is forwarded unchanged. Between steps the loop:
  //     - checks the abort flag and the in-flight context (S5),
  //     - checks shouldPauseForInput() (S5),
  //     - vTaskDelay(1) to let priority-1 tasks run (S1/S2).
  enum class Step : uint8_t { Continue, Done, Abort };
  bool chunkedSdGuard(Step (*step)(void* ctx), void* ctx);

  // True if the in-flight job has been asked to abort (cancel / stale context)
  // OR the user just interacted. A handler that runs ONE long non-chunkable op
  // (e.g. generateThumbBmp) calls this at its natural boundaries to bail out
  // before starting the heavy step (S5/S6).
  bool inFlightShouldAbort() const { return inFlightAbort || shouldPauseForInput(); }

  // --- Held-cache accounting (S7) ---------------------------------------------

  // Total bytes of background-generated cache currently held in RAM by jobs.
  size_t bytesHeld() const { return totalHeld; }

  // Drop the worker's outstanding glyph-prewarm held-byte charge (S7). Call on
  // book close / reader onExit (FIX 4) so the prewarmed-bitmap accounting does
  // not leak across reading sessions once the reader clears the SD-font cache.
  // Idempotent; safe to call when nothing is outstanding.
  void releasePrewarmHeld();

  // Tuning constants (see safety contract above).
  static constexpr size_t kQueueCap = 8;            // (S7) fixed POD ring
  static constexpr size_t kForegroundFloor = 96 * 1024;  // (S4) free-heap floor
  static constexpr size_t kMaxHeldBytes = 40 * 1024;     // (S7) held-cache hard cap
  static constexpr size_t kChunkBytes = 4 * 1024;        // (S2) SD chunk size
  static constexpr uint32_t kIdleBeforeRunMs = 1500;     // (S6) idle before run
  // Cover generation (Job #1) runs epub.load() metadata parse + JPEG/PNG decode.
  // Matches the foreground render task's deep stack (8192 B, see
  // ActivityManager::begin) so the shared decode/parse paths have identical
  // headroom — 4096 risks overflow on the decode call chain.
  static constexpr uint32_t kTaskStackBytes = 8192;      // EPUB/decode work (matches render task)
  static constexpr UBaseType_t kTaskPriority = 0;        // (S1) below UI (=1)

  // JOB 3 (glyph prewarm) sizing. The prewarmed SD-font bitmaps are charged
  // against the S7 held-cache hard cap (kMaxHeldBytes, 40KB) and gated by the S4
  // heap floor. Look-ahead DEPTH is not chosen here: it is implicit in how many
  // upcoming codepoints the caller copied into job.glyphs (bounded by its 512-byte
  // capacity). kPrewarmBytesPerPage is only the per-job heap-gate estimate
  // (job.estBytes) — a conservative guess at one page's worth of CJK glyph bitmaps.
  static constexpr size_t kPrewarmBytesPerPage = 4 * 1024;  // ~conservative CJK page bitmaps

 private:
  BackgroundWorker() = default;
  ~BackgroundWorker() = default;
  BackgroundWorker(const BackgroundWorker&) = delete;
  BackgroundWorker& operator=(const BackgroundWorker&) = delete;

  static BackgroundWorker instance;

  // millis() wrapper kept in the .cpp so this header stays Arduino-free.
  static uint32_t millisNow();

  static void taskTrampoline(void* self);
  void taskLoop();  // blocks on notify when idle (no spin) — S7

  // Queue internals (S7). Guarded by `queueMutex`; the lock is held only for the
  // O(kQueueCap) scan, never across SD/heap work.
  bool popHighestPriority(Job& out);  // returns false if queue empty
  void clearQueue();

  // Dispatch to the per-kind handler. Each handler heap-gates (S4) and aborts at
  // its boundaries on cancel / pause-on-input (S5); PrewarmGlyphs is additionally
  // byte-budgeted against kMaxHeldBytes (FIX 2).
  void runJob(const Job& job);
  void handleGenerateCover(const Job& job);
  void handlePrewarmGlyphs(const Job& job);

  // True if `job` is stale relative to the current context (S5).
  bool isStale(const Job& job) const;

  // Release the worker's outstanding glyph-prewarm held-byte charge (FIX 2/4).
  void releaseOutstandingPrewarm();
  // Atomically charge `n` prewarm bytes to the held total and record them as the
  // outstanding reservation (FIX 2). Held mutex internally.
  void chargeOutstandingPrewarm(size_t n);

  Job queue[kQueueCap] = {};
  bool slotUsed[kQueueCap] = {};
  SemaphoreHandle_t queueMutex = nullptr;  // task↔task; never taken from ISR
  TaskHandle_t taskHandle = nullptr;
  uint32_t nextSeq = 0;

  // Borrowed, set in begin(). Used by Job #3 (SD-font glyph prewarm). Never
  // owned; outlives the worker. nullptr until begin() runs.
  GfxRenderer* renderer = nullptr;

  // Render-serialization hooks (set from src/ via setRenderLockHooks). Take/give
  // the global RenderLock. nullptr until registered; handlers no-op the lock.
  void (*renderLockFn)() = nullptr;
  void (*renderUnlockFn)() = nullptr;

  // Active context + in-flight tracking for cancellation (S5).
  volatile uint32_t currentContext = 0;
  volatile uint32_t inFlightContext = 0;
  volatile Kind inFlightKind = Kind::None;  // kind of the in-flight job (cancelKind)
  volatile bool inFlightAbort = false;
  volatile bool jobInFlight = false;

  size_t totalHeld = 0;  // (S7) guarded by queueMutex

  // Bytes the worker's last glyph-prewarm charged against totalHeld and has not
  // yet released. Single-writer (the worker task in handlePrewarmGlyphs); read on
  // the same task and by releasePrewarmHeld() (called from the reader's render
  // task on onExit — a benign cross-task release of a bounded counter under the
  // held mutex). Released when superseded by the next prewarm or on book close.
  size_t outstandingPrewarmBytes = 0;

  // Pause/deferred-start clock (S5/S6). Single writer = loopTask via
  // noteUserInput(); single reader = worker. Lock-free 32-bit access.
  static volatile uint32_t lastInputMillis;
};

#define BG_WORKER BackgroundWorker::getInstance()
