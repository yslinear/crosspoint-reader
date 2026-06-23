#pragma once

#include <Arduino.h>  // ESP.getFreeHeap / getMaxAllocHeap / getMinFreeHeap

#include "HalSystem.h"
#include "Logging.h"

// Standardized out-of-memory error report.
//
// Logs the requested allocation size alongside the three heap metrics that
// actually explain an OOM on a fragmented heap, then records the requested size
// into HalSystem's RTC_NOINIT field so it survives a panic reboot and appears in
// the crash report (HalSystem::getPanicInfo).
//
// Why MaxAlloc matters: ESP.getFreeHeap() is total free, but a large contiguous
// allocation (e.g. a ~42KB or ~96KB buffer) fails based on the largest
// contiguous block, which is ESP.getMaxAllocHeap(). Guards that check only
// getFreeHeap() use the wrong metric.
//
// The "Free:" / "MaxAlloc:" tokens are kept verbatim so scripts/debugging_monitor.py
// continues to parse them.
//
// Usage:
//   auto buf = makeUniqueNoThrow<uint8_t[]>(size);
//   if (!buf) { LOG_ERR_OOM("MOD", "frame buffer", size); return false; }
//
// The log is diagnostics-only (compiled out in slim builds), so callers MUST
// still perform graceful degradation independently of whether the log emits.
#define LOG_ERR_OOM(tag, what, size)                                                                            \
  do {                                                                                                         \
    HalSystem::setLastAttemptedAllocSize((uint32_t)(size));                                                    \
    LOG_ERR(tag, "OOM %s: req=%u Free: %u bytes, MaxAlloc: %u bytes, MinFree: %u bytes", what, (unsigned)(size), \
            (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap(), (unsigned)ESP.getMinFreeHeap());     \
  } while (0)
