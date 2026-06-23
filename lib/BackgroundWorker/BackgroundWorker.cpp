#include "BackgroundWorker.h"

#include <Arduino.h>  // millis(), ESP.getFreeHeap()/getMaxAllocHeap()
#include <Epub.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <SdCardFont.h>

#include <cstring>
#include <memory>
#include <string>

namespace {
constexpr char TAG[] = "BGW";

// EPUB cache root on the SD card. Same literal used by HomeActivity / reader.
constexpr char kCacheRoot[] = "/.crosspoint";

// RAII helper that takes the global render lock via the registered hooks for the
// duration of a scope, then releases it. No-op if hooks are unset. This is how a
// lib/ handler serializes shared renderer / SD-font mutation against the
// foreground render task without lib/ depending on src/RenderLock.
struct ScopedRenderLock {
  void (*unlockFn)();
  ScopedRenderLock(void (*lockFn)(), void (*unlockFn_)()) : unlockFn(unlockFn_) {
    if (lockFn) lockFn();
  }
  ~ScopedRenderLock() {
    if (unlockFn) unlockFn();
  }
  ScopedRenderLock(const ScopedRenderLock&) = delete;
  ScopedRenderLock& operator=(const ScopedRenderLock&) = delete;
};

// RAII guard for the queue mutex. Task context only — never used from an ISR.
struct QueueLock {
  SemaphoreHandle_t mtx;
  explicit QueueLock(SemaphoreHandle_t m) : mtx(m) {
    if (mtx) xSemaphoreTake(mtx, portMAX_DELAY);
  }
  ~QueueLock() {
    if (mtx) xSemaphoreGive(mtx);
  }
  QueueLock(const QueueLock&) = delete;
  QueueLock& operator=(const QueueLock&) = delete;
};
}  // namespace

BackgroundWorker BackgroundWorker::instance;
volatile uint32_t BackgroundWorker::lastInputMillis = 0;

uint32_t BackgroundWorker::millisNow() { return millis(); }

// --- Lifecycle ---------------------------------------------------------------

void BackgroundWorker::begin(GfxRenderer& renderer) {
  this->renderer = &renderer;
  if (taskHandle != nullptr) {
    return;  // already started — idempotent (renderer pointer refreshed above)
  }
  if (queueMutex == nullptr) {
    queueMutex = xSemaphoreCreateMutex();
    if (queueMutex == nullptr) {
      LOG_ERR(TAG, "Failed to create queue mutex");
      return;
    }
  }
  // (S1) Priority 0 (tskIDLE_PRIORITY): strictly below the UI render task and
  // the Arduino loopTask (both priority 1). A page turn preempts the worker.
  BaseType_t ok = xTaskCreate(&taskTrampoline, "BGW", kTaskStackBytes, this,
                              kTaskPriority, &taskHandle);
  if (ok != pdPASS || taskHandle == nullptr) {
    LOG_ERR(TAG, "Failed to create worker task");
    taskHandle = nullptr;
    return;
  }
  LOG_INF(TAG, "Worker started (prio %u, stack %u)", (unsigned)kTaskPriority,
          (unsigned)kTaskStackBytes);
}

void BackgroundWorker::taskTrampoline(void* self) {
  static_cast<BackgroundWorker*>(self)->taskLoop();
}

void BackgroundWorker::taskLoop() {
  for (;;) {
    // (S7) Block with no CPU spin until enqueue()/cancel() notifies us, or until
    // the deferred-start window elapses so we can re-check a pending job. We use
    // a bounded wait rather than portMAX_DELAY so that a job enqueued during
    // input pause is retried once the idle window passes (S6) without needing a
    // second notification.
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(kIdleBeforeRunMs));

    // (S6) Deferred start: do not run during/right after a foreground render or
    // active input. Loop back and block again until the user has been idle.
    if (shouldPauseForInput()) {
      continue;
    }

    Job job;
    if (!popHighestPriority(job)) {
      continue;  // nothing to do — block again
    }

    // (S5) Skip jobs that went stale between enqueue and now.
    if (isStale(job)) {
      LOG_DBG(TAG, "Skip stale job kind=%u ctx=%u", (unsigned)job.kind,
              (unsigned)job.contextId);
      continue;
    }

    // Mark in-flight so cancellation can target this job at chunk boundaries.
    inFlightContext = job.contextId;
    inFlightKind = job.kind;
    inFlightAbort = false;
    jobInFlight = true;

    runJob(job);

    jobInFlight = false;
    inFlightAbort = false;
    inFlightKind = Kind::None;

    // If more work is queued, loop immediately (without waiting for a new
    // notification) so the queue drains while the user stays idle.
    {
      QueueLock lock(queueMutex);
      for (size_t i = 0; i < kQueueCap; ++i) {
        if (slotUsed[i]) {
          xTaskNotifyGive(taskHandle);
          break;
        }
      }
    }
  }
}

// --- Enqueue / queue management (S7) -----------------------------------------

bool BackgroundWorker::enqueue(const Job& job) {
  if (job.kind == Kind::None) {
    return false;
  }
  QueueLock lock(queueMutex);
  for (size_t i = 0; i < kQueueCap; ++i) {
    if (!slotUsed[i]) {
      queue[i] = job;
      queue[i].seq = nextSeq++;
      // Defensive: guarantee null-termination of the copied char buffers (S3).
      queue[i].path[sizeof(queue[i].path) - 1] = '\0';
      queue[i].glyphs[sizeof(queue[i].glyphs) - 1] = '\0';
      slotUsed[i] = true;
      if (taskHandle) {
        xTaskNotifyGive(taskHandle);  // wake the worker (S7 — no spin)
      }
      return true;
    }
  }
  LOG_DBG(TAG, "Queue full, dropping kind=%u", (unsigned)job.kind);
  return false;  // full — drop, never block the caller
}

bool BackgroundWorker::popHighestPriority(Job& out) {
  QueueLock lock(queueMutex);
  int best = -1;
  for (size_t i = 0; i < kQueueCap; ++i) {
    if (!slotUsed[i]) continue;
    if (best < 0) {
      best = static_cast<int>(i);
      continue;
    }
    // Higher Priority first; tie broken by lower seq (FIFO).
    if (queue[i].priority > queue[best].priority ||
        (queue[i].priority == queue[best].priority &&
         queue[i].seq < queue[best].seq)) {
      best = static_cast<int>(i);
    }
  }
  if (best < 0) {
    return false;
  }
  out = queue[best];
  slotUsed[best] = false;
  return true;
}

void BackgroundWorker::clearQueue() {
  QueueLock lock(queueMutex);
  for (size_t i = 0; i < kQueueCap; ++i) {
    slotUsed[i] = false;
  }
}

// --- Cancellation / context (S5) ---------------------------------------------

void BackgroundWorker::setContext(uint32_t contextId) {
  currentContext = contextId;
  cancelStale(contextId);
}

size_t BackgroundWorker::cancel(bool (*pred)(const Job&, void*), void* ctx) {
  if (!pred) return 0;
  size_t dropped = 0;
  {
    QueueLock lock(queueMutex);
    for (size_t i = 0; i < kQueueCap; ++i) {
      if (slotUsed[i] && pred(queue[i], ctx)) {
        slotUsed[i] = false;
        ++dropped;
      }
    }
  }
  // Ask the in-flight job to abort if it matches (checked at next chunk).
  if (jobInFlight) {
    Job probe = {};
    probe.contextId = inFlightContext;
    if (pred(probe, ctx)) {
      inFlightAbort = true;
    }
  }
  return dropped;
}

void BackgroundWorker::cancelAll() {
  clearQueue();
  if (jobInFlight) {
    inFlightAbort = true;
  }
}

size_t BackgroundWorker::cancelStale(uint32_t keepContextId) {
  size_t dropped = 0;
  {
    QueueLock lock(queueMutex);
    for (size_t i = 0; i < kQueueCap; ++i) {
      if (slotUsed[i] && queue[i].contextId != keepContextId) {
        slotUsed[i] = false;
        ++dropped;
      }
    }
  }
  if (jobInFlight && inFlightContext != keepContextId) {
    inFlightAbort = true;
  }
  if (dropped) {
    LOG_DBG(TAG, "Dropped %u stale jobs (keep ctx=%u)", (unsigned)dropped,
            (unsigned)keepContextId);
  }
  return dropped;
}

size_t BackgroundWorker::cancelKind(Kind kind) {
  size_t dropped = 0;
  {
    QueueLock lock(queueMutex);
    for (size_t i = 0; i < kQueueCap; ++i) {
      if (slotUsed[i] && queue[i].kind == kind) {
        slotUsed[i] = false;
        ++dropped;
      }
    }
  }
  if (jobInFlight && inFlightKind == kind) {
    inFlightAbort = true;  // honoured at the in-flight job's next boundary (S5)
  }
  if (dropped) {
    LOG_DBG(TAG, "Dropped %u jobs of kind=%u", (unsigned)dropped, (unsigned)kind);
  }
  return dropped;
}

bool BackgroundWorker::isStale(const Job& job) const {
  // A job is stale if a context has been set and it no longer matches.
  return currentContext != 0 && job.contextId != currentContext;
}

// --- Pause-on-input / deferred-start (S5/S6) ---------------------------------

bool BackgroundWorker::shouldPauseForInput() {
  uint32_t last = lastInputMillis;  // single 32-bit read (atomic on RISC-V)
  return (millisNow() - last) < kIdleBeforeRunMs;
}

// --- Heap gate (S4) ----------------------------------------------------------

bool BackgroundWorker::heapGate(size_t neededBytes, size_t floorBytes) {
  // Fragmentation, not total free, decides whether an allocation succeeds, so
  // gate on the largest contiguous block. Then ensure that consuming
  // `neededBytes` still leaves the foreground safety floor intact.
  if (ESP.getMaxAllocHeap() < neededBytes) {
    return false;
  }
  size_t freeHeap = ESP.getFreeHeap();
  if (freeHeap < floorBytes) {
    return false;
  }
  return (freeHeap - neededBytes) >= floorBytes;
}

// --- Chunked SD helper (S2) --------------------------------------------------

bool BackgroundWorker::chunkedSdGuard(Step (*step)(void*), void* ctx) {
  if (!step) return false;
  for (;;) {
    // (S5) Abort fast on cancellation or a stale in-flight context.
    if (inFlightAbort) {
      LOG_DBG(TAG, "chunkedSdGuard aborted (cancel)");
      return false;
    }
    // (S5) A page turn / input mid-job defers the remainder immediately.
    if (shouldPauseForInput()) {
      LOG_DBG(TAG, "chunkedSdGuard deferred (input)");
      return false;
    }

    Step s = step(ctx);  // does at most ONE small mutex-released SD op (S2)
    if (s == Step::Done) {
      return true;
    }
    if (s == Step::Abort) {
      return false;
    }

    // (S1/S2) Yield so any ready priority-1 task (render / loopTask) runs before
    // the next chunk. Between chunks we hold NO storage mutex, so a foreground
    // SD access interleaves at chunk granularity (a few ms).
    vTaskDelay(1);
  }
}

// --- Per-kind dispatch -------------------------------------------------------

void BackgroundWorker::runJob(const Job& job) {
  switch (job.kind) {
    case Kind::GenerateCover:
      handleGenerateCover(job);
      break;
    case Kind::PrewarmGlyphs:
      handlePrewarmGlyphs(job);
      break;
    case Kind::None:
    default:
      break;
  }
}

// JOB 1 — Cover thumbnail generation.
//
// Builds the home-screen thumbnail BMP for job.path at height job.b. The render
// work is pure SD + heap (JPEG/PNG decode → 1-bit BMP); it does NOT touch the
// framebuffer or the shared SD-font state, so no RenderLock is needed — only
// HalStorage's own mutex serializes it against the foreground (S2). Heap-gated
// (S4): the decode needs a transient line/scanline buffer (~27KB worst case);
// we skip if free heap would fall below the floor. Abort/defer is checked once
// up front; generateThumbBmp is a single bounded op and is idempotent (it
// no-ops if the thumb already exists), so a re-enqueue after a deferral is cheap.
void BackgroundWorker::handleGenerateCover(const Job& job) {
  if (inFlightShouldAbort()) {
    LOG_DBG(TAG, "CoverGen deferred before start (input/cancel): %s", job.path);
    return;
  }
  if (!heapGate(job.estBytes)) {
    LOG_DBG(TAG, "CoverGen skipped (heap below floor, need %u): %s", (unsigned)job.estBytes, job.path);
    return;  // back off under memory pressure (S4) — re-enqueued next reader entry
  }

  const int height = static_cast<int>(job.b);
  Epub epub(job.path, kCacheRoot);
  // Metadata only (skip CSS); the thumbnail just needs the cover item href.
  if (!epub.load(/*buildIfMissing=*/false, /*skipLoadingCss=*/true)) {
    LOG_DBG(TAG, "CoverGen: epub load failed: %s", job.path);
    return;
  }
  if (Storage.exists(epub.getThumbBmpPath(height).c_str())) {
    return;  // already cached (race with Home/foreground) — nothing to do
  }
  if (inFlightShouldAbort()) {
    return;  // user came back while we loaded metadata (S5)
  }

  const bool ok = epub.generateThumbBmp(height);
  LOG_DBG(TAG, "CoverGen %s for %s (h=%d)", ok ? "done" : "failed", job.path, height);
}

// JOB 3 — CJK glyph prewarm for upcoming pages.
//
// job.glyphs holds the unique upcoming-page codepoints (UTF-8), copied at enqueue
// from the already-laid-out PageLines (S3). job.a is the SD reader font id. The
// look-ahead depth is NOT chosen here — it is exactly the codepoint set the caller
// copied into job.glyphs (bounded by its 512-byte capacity); within that set,
// prewarm() loads only as many glyphs as the heap/held-cache byte budget allows.
//
// What persists vs what doesn't (the architecture, see SdCardFont):
//   - The ADVANCE table (advanceTable_, cap 3072 CJK regular ≈ 24KB) is the only
//     per-glyph state that survives clearCache() across page renders. Populating
//     it for upcoming codepoints (buildAdvanceTable) is the durable anti-stutter
//     win: fast flips no longer pay a per-glyph SD advance read during layout.
//   - The glyph BITMAP store (miniData) is rebuilt and CLEARED every page by the
//     foreground PrewarmScope, so prewarmed bitmaps cannot outlive one render.
//     We still call prewarm() to warm the SD-font kern/lig path and the SD
//     controller, but the lasting benefit is the advance table.
//
// Both calls mutate shared SD-font state and need the renderer's SD-font map, so
// they run under the RenderLock (serialized vs foreground render). Heap-gated and
// charged against the S7 held-cache hard cap; depth is implicit in how many
// upcoming codepoints the caller copied (bounded by job.glyphs capacity).
void BackgroundWorker::handlePrewarmGlyphs(const Job& job) {
  if (!renderer) {
    LOG_ERR(TAG, "PrewarmGlyphs: no renderer");
    return;
  }
  if (job.glyphs[0] == '\0') {
    return;  // nothing to warm
  }
  if (inFlightShouldAbort()) {
    LOG_DBG(TAG, "PrewarmGlyphs deferred before start: %s", job.path);
    return;
  }

  const int fontId = static_cast<int>(job.a);
  if (!renderer->isSdCardFont(fontId)) {
    return;  // built-in font — bitmaps already in flash, nothing to prewarm
  }

  // FIX A (total-footprint byte budget): the prewarm's RESIDENT footprint — the
  // mini bitmap store PLUS the per-glyph metadata (miniGlyphs/miniIntervals) PLUS
  // the mini-kern tables — is bounded by BOTH the S7 held-cache hard cap
  // (kMaxHeldBytes, 40KB) and the S4 foreground heap floor (96KB). Compute an
  // explicit byte budget = the smaller of:
  //   - the held-cache headroom still under the 40KB cap, and
  //   - the heap headroom that keeps getFreeHeap() at/above the 96KB floor
  //     (and that the largest contiguous block can actually satisfy).
  // prewarm() loads only as many glyphs as fit that budget (charging each its full
  // resident cost) and reports the ACTUAL resident bytes it allocated, which we
  // charge against the held cache. This is what makes kMaxHeldBytes bind the REAL
  // resident footprint — previously only the bitmap store was budgeted/charged, so
  // the ~28 B/glyph metadata (×4 styles) plus the mini-kern tables were resident
  // but uncounted and ungated by the 96KB floor.
  //
  // Each SdCardFont prewarm frees the prior mini store (freeStyleMiniData) before
  // building the new one, so the worker's OWN outstanding prewarm charge is
  // released here before the next is reserved (releaseOutstandingPrewarm()).
  releaseOutstandingPrewarm();
  const size_t held = bytesHeld();
  if (held >= kMaxHeldBytes) {
    LOG_DBG(TAG, "PrewarmGlyphs skipped (held-cache cap reached: %u)", (unsigned)held);
    return;
  }
  size_t budget = kMaxHeldBytes - held;

  // Clamp the budget to the heap headroom above the foreground floor. This keeps
  // a fragmented/low-heap device from prewarming at all rather than risking a
  // foreground OOM (graceful, no abort).
  const size_t freeHeap = ESP.getFreeHeap();
  const size_t maxBlock = ESP.getMaxAllocHeap();
  if (freeHeap <= kForegroundFloor) {
    LOG_DBG(TAG, "PrewarmGlyphs skipped (heap below floor: %u)", (unsigned)freeHeap);
    return;
  }
  const size_t heapHeadroom = freeHeap - kForegroundFloor;
  if (budget > heapHeadroom) budget = heapHeadroom;
  if (budget > maxBlock) budget = maxBlock;  // fragmentation, not total free, gates the alloc
  if (budget == 0) {
    LOG_DBG(TAG, "PrewarmGlyphs skipped (no heap headroom under floor)");
    return;
  }

  size_t prewarmedBytes = 0;
  {
    ScopedRenderLock lock(renderLockFn, renderUnlockFn);
    if (!inFlightShouldAbort()) {
      const auto& fonts = renderer->getSdCardFonts();
      auto it = fonts.find(fontId);
      if (it != fonts.end() && it->second) {
        SdCardFont* font = it->second;
        // Persistent: advance metrics for the upcoming codepoints (survives).
        font->buildAdvanceTable(job.glyphs);
        // Transient: warm the mini store (bitmaps + per-glyph metadata + kern/lig
        // path), cleared by the next foreground PrewarmScope, but the SD reads are
        // now amortized for this look-ahead. Pass the byte budget so the TOTAL
        // resident footprint can never exceed the cap, and capture the ACTUAL
        // resident bytes allocated for the held-cache accounting.
        font->prewarm(job.glyphs, /*styleMask=*/0x0F, /*metadataOnly=*/false,
                      /*maxBitmapBytes=*/budget, &prewarmedBytes);
        LOG_DBG(TAG, "PrewarmGlyphs done: font=%d, %u resident bytes (budget %u)", fontId, (unsigned)prewarmedBytes,
                (unsigned)budget);

        // Charge the held cache with what we actually prewarmed and remember it as
        // the worker's outstanding prewarm charge, released either when the NEXT
        // prewarm runs (releaseOutstandingPrewarm above, since the prior mini store
        // is freed by the new prewarm) or on book-close cancel / reader onExit
        // (BackgroundWorker::releasePrewarmHeld()). This keeps kMaxHeldBytes a live
        // high-water gate over the full resident prewarm footprint (bitmaps +
        // per-glyph metadata + mini-kern).
        //
        // FIX 4: charge INSIDE the render lock, before it is released. The reader's
        // onExit frees these same mini-bitmaps and calls releasePrewarmHeld() while
        // holding the global RenderLock; if we charged after releasing it here, an
        // onExit landing in that window would release nothing (charge not yet
        // recorded) and leave a phantom totalHeld for already-freed bitmaps. Holding
        // the render lock across the charge makes the alloc+charge atomic vs. that
        // free+release.
        if (prewarmedBytes > 0) {
          chargeOutstandingPrewarm(prewarmedBytes);
        }
      }
    }
  }
}

// Atomically charge `n` bytes to the held total and record them as the worker's
// outstanding prewarm reservation, under the held mutex so a concurrent
// releasePrewarmHeld() (reader onExit) cannot miss the just-charged bytes.
void BackgroundWorker::chargeOutstandingPrewarm(size_t n) {
  QueueLock lock(queueMutex);
  // Clamp to the cap defensively; the caller already sized `budget` to fit.
  if (totalHeld + n > kMaxHeldBytes) {
    n = (kMaxHeldBytes > totalHeld) ? (kMaxHeldBytes - totalHeld) : 0;
  }
  totalHeld += n;
  outstandingPrewarmBytes = n;
}

// Release the worker's outstanding prewarm bitmap charge (S7 accounting). The
// underlying SdCardFont mini-bitmaps are freed elsewhere (next prewarm's
// freeStyleMiniData, the foreground PrewarmScope's clearCache, or the reader's
// onExit clear — FIX 4); this only drops the held-byte reservation so the cap
// reflects what is actually resident. The read-zero-subtract is done atomically
// under the held mutex because this can be called from BOTH the worker task
// (next prewarm) and the reader's render task (releasePrewarmHeld on onExit).
void BackgroundWorker::releaseOutstandingPrewarm() {
  QueueLock lock(queueMutex);
  if (outstandingPrewarmBytes > 0) {
    totalHeld = (outstandingPrewarmBytes >= totalHeld) ? 0 : (totalHeld - outstandingPrewarmBytes);
    outstandingPrewarmBytes = 0;
  }
}

void BackgroundWorker::releasePrewarmHeld() { releaseOutstandingPrewarm(); }

// --- Typed enqueue helpers (S3: copy everything into the POD) -----------------

bool BackgroundWorker::enqueueCoverGen(const char* epubPath, uint32_t bookHash, int height) {
  Job job;
  job.kind = Kind::GenerateCover;
  job.priority = Priority::Normal;
  job.contextId = bookHash;
  job.b = static_cast<uint32_t>(height);
  // True worst-case CONCURRENT heap for a cover decode. generateThumbBmp handles
  // BOTH a JPEG and a PNG cover path; the gate (S4, heapGate(estBytes)) must size
  // for the LARGER so a decode it admits never breaches the 96KB foreground floor.
  //
  // JPEG path (~44KB): the JPEGDEC object (JPEG_DECODER_APPROX_SIZE = 20KB,
  //   JpegToFramebufferConverter.cpp) and the PixelCache streaming band
  //   (MAX_BAND_BYTES = 24KB, converters/PixelCache.h) coexist mid-decode.
  //
  // PNG path (~70KB, FIX B — previously uncounted, the old 48KB underestimated it):
  //   PngToBmpConverter holds, concurrently, the streaming inflate ring buffer
  //   (INFLATE_DICT_SIZE = 32KB, InflateReader.cpp) PLUS two defilter scanlines
  //   currentRow + previousRow at rawRowBytes each (capped at 16KB each → up to
  //   32KB combined at MAX_IMAGE_WIDTH), plus the source-width grayRow (≤2KB) and
  //   the downscale accumulators/ditherer (a few KB). Peak ≈ 32 + 32 + ~6 ≈ 70KB.
  //
  // Use the PNG worst case with a small margin so EPUB PNG covers honour the floor.
  job.estBytes = 72 * 1024;
  strncpy(job.path, epubPath ? epubPath : "", sizeof(job.path) - 1);
  return enqueue(job);
}

bool BackgroundWorker::enqueuePrewarmGlyphs(const char* epubPath, uint32_t bookHash, int fontId,
                                            const char* utf8Codepoints) {
  Job job;
  job.kind = Kind::PrewarmGlyphs;
  job.priority = Priority::Low;  // opportunistic — lowest priority (runs after cover gen)
  job.contextId = bookHash;
  job.a = static_cast<uint32_t>(fontId);
  job.estBytes = kPrewarmBytesPerPage;
  strncpy(job.path, epubPath ? epubPath : "", sizeof(job.path) - 1);
  strncpy(job.glyphs, utf8Codepoints ? utf8Codepoints : "", sizeof(job.glyphs) - 1);
  return enqueue(job);
}
