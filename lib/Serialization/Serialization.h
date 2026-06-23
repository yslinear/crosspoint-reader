#pragma once
#include <HalStorage.h>
#include <Logging.h>

#include <iostream>

#include "SerializationLimits.h"  // MAX_SERIALIZED_STRING_LEN + serializedStringLenWithinCap (host-testable)

namespace serialization {

template <typename T>
void writePod(std::ostream& os, const T& value) {
  os.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

template <typename T>
void writePod(HalFile& file, const T& value) {
  file.write(reinterpret_cast<const uint8_t*>(&value), sizeof(T));
}

template <typename T>
void readPod(std::istream& is, T& value) {
  is.read(reinterpret_cast<char*>(&value), sizeof(T));
}

// Generic reader overload: binds to HalFile and to BufferedReader<HalFile>
// (BufferedHalReader). Both expose int read(void*, size_t)/size()/position(),
// so the per-element readPod/readString loops are unchanged at every call site —
// only the stream they read from differs. The non-template std::istream overload
// above is preferred for std::istream arguments by overload resolution, so the
// host (stream-based) serialization path is unaffected.
//
// read() may return a short count across a buffer-boundary refill, so loop until
// sizeof(T) bytes are delivered (or EOF); a partially filled POD would silently
// corrupt the value otherwise.
template <typename Reader, typename T>
void readPod(Reader& file, T& value) {
  auto* dst = reinterpret_cast<uint8_t*>(&value);
  size_t total = 0;
  while (total < sizeof(T)) {
    const int got = file.read(dst + total, sizeof(T) - total);
    if (got <= 0) break;
    total += static_cast<size_t>(got);
  }
}

inline void writeString(std::ostream& os, const std::string& s) {
  const uint32_t len = s.size();
  writePod(os, len);
  os.write(s.data(), len);
}

inline void writeString(HalFile& file, const std::string& s) {
  const uint32_t len = s.size();
  writePod(file, len);
  file.write(reinterpret_cast<const uint8_t*>(s.data()), len);
}

inline void readString(std::istream& is, std::string& s) {
  uint32_t len;
  readPod(is, len);
  if (!serializedStringLenWithinCap(len)) {
    LOG_ERR("SER", "corrupt cache: string len %u exceeds cap (%u)", (unsigned)len,
            (unsigned)MAX_SERIALIZED_STRING_LEN);
    s.clear();
    return;
  }
  s.resize(len);
  is.read(&s[0], len);
}

// Generic reader overload: binds to HalFile and to BufferedReader<HalFile>.
// BufferedReader::position() reports the LOGICAL position (bytes consumed) and
// size() the underlying file size, so the remaining-bytes guard below stays
// correct under buffering (it must not see the prefetch offset). The non-template
// std::istream overload above (no size()/position()) is unaffected.
template <typename Reader>
void readString(Reader& file, std::string& s) {
  uint32_t len;
  readPod(file, len);
  if (!serializedStringLenWithinCap(len)) {
    LOG_ERR("SER", "corrupt cache: string len %u exceeds cap (%u)", (unsigned)len,
            (unsigned)MAX_SERIALIZED_STRING_LEN);
    s.clear();
    return;
  }
  // Remaining-bytes check: a length larger than what is left in the file is a
  // sure sign of corruption. Reject before the resize() rather than read past
  // EOF into an over-sized buffer.
  const size_t fileLen = file.size();
  const size_t pos = file.position();
  if (pos <= fileLen && len > (fileLen - pos)) {
    LOG_ERR("SER", "corrupt cache: string len %u exceeds remaining %u bytes", (unsigned)len,
            (unsigned)(fileLen - pos));
    s.clear();
    return;
  }
  s.resize(len);
  if (len > 0) {
    // read() may return a short count across a buffer refill; loop to fill.
    size_t total = 0;
    while (total < len) {
      const int got = file.read(&s[total], len - total);
      if (got <= 0) break;
      total += static_cast<size_t>(got);
    }
  }
}
}  // namespace serialization
