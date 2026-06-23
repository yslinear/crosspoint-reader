#pragma once
#include <cstddef>
#include <cstdint>

// Pure (SDK-free) serialization limits, split out from Serialization.h so the
// corrupt-cache length guard is host-unit-testable without the Arduino/SdFat deps
// that Serialization.h pulls in via HalStorage.h.
namespace serialization {

// Generous upper bound on a single serialized string. No real EPUB title, href, or
// word approaches this; a larger length field means the cache is corrupt. Rejecting
// here (and leaving the string empty) instead of resize()-ing to a bogus length
// prevents a multi-GB allocation that aborts under -fno-exceptions on the ~380KB heap.
constexpr uint32_t MAX_SERIALIZED_STRING_LEN = 64 * 1024;

// True if a serialized string length is within the cap (the first, length-only gate).
// Callers reading from a sized source should ALSO check len against bytes-remaining.
inline bool serializedStringLenWithinCap(uint32_t len) { return len <= MAX_SERIALIZED_STRING_LEN; }

}  // namespace serialization
