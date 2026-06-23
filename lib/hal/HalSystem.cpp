#include "HalSystem.h"

#include <string>

#include "Arduino.h"
#include "HalStorage.h"
#include "Logging.h"
#include "esp_debug_helpers.h"
#include "esp_private/esp_cpu_internal.h"
#include "esp_private/esp_system_attr.h"
#include "esp_private/panic_internal.h"

#define MAX_PANIC_STACK_DEPTH 32

RTC_NOINIT_ATTR char panicMessage[256];
RTC_NOINIT_ATTR HalSystem::StackFrame panicStack[MAX_PANIC_STACK_DEPTH];

// --- Observability (PHASE 1) ---
// RTC_NOINIT survives a panic reboot but is garbage on cold boot, so guard the
// breadcrumb with a magic word (mirrors the rtcLogMagic idiom in Logging.cpp).
static constexpr uint32_t BREADCRUMB_MAGIC = 0xB00B5EED;
RTC_NOINIT_ATTR char breadcrumb[48];
RTC_NOINIT_ATTR uint32_t breadcrumbMagic;
RTC_NOINIT_ATTR uint32_t lastSampledFreeHeap;
RTC_NOINIT_ATTR uint32_t lastSampledMaxAlloc;
RTC_NOINIT_ATTR uint32_t lastAttemptedAllocSize;

extern "C" {

void __real_panic_abort(const char* message);
void __real_panic_print_backtrace(const void* frame, int core);

static DRAM_ATTR const char PANIC_REASON_UNKNOWN[] = "(unknown panic reason)";
void IRAM_ATTR __wrap_panic_abort(const char* message) {
  if (!message) message = PANIC_REASON_UNKNOWN;
  // IRAM-safe bounded copy (strncpy is not IRAM-safe in panic context)
  int i = 0;
  for (; i < (int)sizeof(panicMessage) - 1 && message[i]; i++) {
    panicMessage[i] = message[i];
  }
  panicMessage[i] = '\0';

  __real_panic_abort(message);
}

void IRAM_ATTR __wrap_panic_print_backtrace(const void* frame, int core) {
  if (!frame) {
    __real_panic_print_backtrace(frame, core);
    return;
  }
  for (size_t i = 0; i < MAX_PANIC_STACK_DEPTH; i++) {
    panicStack[i].sp = 0;
  }

  // Copied from components/esp_system/port/arch/riscv/panic_arch.c
  uint32_t sp = (uint32_t)((RvExcFrame*)frame)->sp;
  const int per_line = 8;
  int depth = 0;
  for (int x = 0; x < 1024; x += per_line * sizeof(uint32_t)) {
    uint32_t* spp = (uint32_t*)(sp + x);
    // panic_print_hex(sp + x);
    // panic_print_str(": ");
    panicStack[depth].sp = sp + x;
    for (int y = 0; y < per_line; y++) {
      // panic_print_str("0x");
      // panic_print_hex(spp[y]);
      // panic_print_str(y == per_line - 1 ? "\r\n" : " ");
      panicStack[depth].spp[y] = spp[y];
    }

    depth++;
    if (depth >= MAX_PANIC_STACK_DEPTH) {
      break;
    }
  }

  __real_panic_print_backtrace(frame, core);
}
}

namespace HalSystem {

void begin() {
  // This is mostly for the first boot, we need to initialize the panic info and logs to empty state
  // If we reboot from a panic state, we want to keep the panic info until we successfully dump it to the SD card, use
  // `clearPanic()` to clear it after dumping
  if (!isRebootFromPanic()) {
    clearPanic();
  } else {
    // Panic reboot: preserve logs and panic info, but clamp logHead in case the
    // panic occurred before begin() ever ran (e.g. in a static constructor).
    // If logHead was out of range, logMessages is also garbage — clear it so
    // getLastLogs() does not dump corrupt data into the crash report.
    if (sanitizeLogHead()) {
      clearLastLogs();
    }
  }
}

void checkPanic() {
  if (isRebootFromPanic()) {
    auto panicInfo = getPanicInfo(true);
    auto file = Storage.open("/crash_report.txt", O_WRITE | O_CREAT | O_TRUNC);
    if (file) {
      file.write(panicInfo.c_str(), panicInfo.size());
      file.close();
      LOG_INF("SYS", "Dumped panic info to SD card");
    } else {
      LOG_ERR("SYS", "Failed to open crash_report.txt for writing");
    }
  }
}

void clearPanic() {
  panicMessage[0] = '\0';
  for (size_t i = 0; i < MAX_PANIC_STACK_DEPTH; i++) {
    panicStack[i].sp = 0;
  }
  breadcrumb[0] = '\0';
  lastAttemptedAllocSize = 0;
  clearLastLogs();
}

std::string getPanicInfo(bool full) {
  if (!full) {
    return panicMessage;
  } else {
    std::string info;

    info += "CrossPoint version: " CROSSPOINT_VERSION;
    info += "\n\nPanic reason: " + std::string(panicMessage);

    // Observability breadcrumb + last-sampled heap stats (PHASE 1). Guarded by
    // the magic word so cold-boot RTC garbage is not reported as a breadcrumb.
    if (breadcrumbMagic == BREADCRUMB_MAGIC) {
      breadcrumb[sizeof(breadcrumb) - 1] = '\0';
      // breadcrumb may be empty if a panic preceded the first setBreadcrumb (the magic
      // is also stamped by sampleHeap); only print the operation line when it is set.
      if (breadcrumb[0] != '\0') {
        info += "\n\nLast operation: " + std::string(breadcrumb);
      }
      info += "\n\nFree heap (last sampled): " + std::to_string(lastSampledFreeHeap);
      info += "\nLargest free block (last sampled): " + std::to_string(lastSampledMaxAlloc);
      info += "\nLast attempted alloc: " + std::to_string(lastAttemptedAllocSize);
    }

    info += "\n\nLast logs:\n" + getLastLogs();
    info += "\n\nStack memory:\n";

    auto toHex = [](uint32_t value) {
      char buffer[9];
      snprintf(buffer, sizeof(buffer), "%08X", value);
      return std::string(buffer);
    };
    for (size_t i = 0; i < MAX_PANIC_STACK_DEPTH; i++) {
      if (panicStack[i].sp == 0) {
        break;
      }
      info += "0x" + toHex(panicStack[i].sp) + ": ";
      for (size_t j = 0; j < 8; j++) {
        info += "0x" + toHex(panicStack[i].spp[j]) + " ";
      }
      info += "\n";
    }

    return info;
  }
}

void setBreadcrumb(const char* op) {
  if (!op) return;
  // Hand-rolled bounded copy + magic stamp: no heap, no snprintf. Mirrors the
  // IRAM-safe copy idiom in __wrap_panic_abort above. Runs at normal runtime.
  int i = 0;
  for (; i < (int)sizeof(breadcrumb) - 1 && op[i]; i++) {
    breadcrumb[i] = op[i];
  }
  breadcrumb[i] = '\0';
  breadcrumbMagic = BREADCRUMB_MAGIC;
}

void sampleHeap() {
  lastSampledFreeHeap = ESP.getFreeHeap();
  lastSampledMaxAlloc = ESP.getMaxAllocHeap();
  // Validate the RTC fields so getPanicInfo reports the heap stats even if a panic
  // happens before the first setBreadcrumb (sampleHeap runs at boot + every ~10s).
  breadcrumbMagic = BREADCRUMB_MAGIC;
}

void setLastAttemptedAllocSize(uint32_t size) { lastAttemptedAllocSize = size; }

bool isRebootFromPanic() {
  const auto resetReason = esp_reset_reason();
  return resetReason == ESP_RST_PANIC || resetReason == ESP_RST_CPU_LOCKUP;
}

}  // namespace HalSystem
