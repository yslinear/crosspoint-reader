#pragma once

#include <cstdint>
#include <string>

namespace HalSystem {
struct StackFrame {
  uint32_t sp;
  uint32_t spp[8];
};

void begin();

// Dump panic info to SD card if necessary
void checkPanic();
void clearPanic();

std::string getPanicInfo(bool full = false);
bool isRebootFromPanic();

// --- Observability (PHASE 1) ---
// Record a coarse-grained breadcrumb describing the operation currently in
// progress. Persisted in RTC_NOINIT memory so it survives a panic reboot and is
// appended to the crash report by getPanicInfo(true). Hand-rolled bounded copy:
// no heap, no snprintf. Safe to call from normal runtime (not from an ISR or the
// panic hook).
void setBreadcrumb(const char* op);

// Sample current heap stats into RTC_NOINIT memory so the panic hook can report
// them post-crash without touching heap_caps_* (which is unsafe in the hook).
void sampleHeap();

// Store the size of the most recent failed/attempted allocation into RTC_NOINIT
// memory. Called by LOG_ERR_OOM so the value appears in the crash report.
void setLastAttemptedAllocSize(uint32_t size);
}  // namespace HalSystem
