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

template <typename T>
void readPod(HalFile& file, T& value) {
  file.read(reinterpret_cast<uint8_t*>(&value), sizeof(T));
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

inline void readString(HalFile& file, std::string& s) {
  uint32_t len;
  readPod(file, len);
  if (!serializedStringLenWithinCap(len)) {
    LOG_ERR("SER", "corrupt cache: string len %u exceeds cap (%u)", (unsigned)len,
            (unsigned)MAX_SERIALIZED_STRING_LEN);
    s.clear();
    return;
  }
  // Remaining-bytes check: a length larger than what is left in the file is a
  // sure sign of corruption. HalFile exposes size()/position(), so reject before
  // the resize() rather than read past EOF into an over-sized buffer.
  const size_t fileLen = file.size();
  const size_t pos = file.position();
  if (pos <= fileLen && len > (fileLen - pos)) {
    LOG_ERR("SER", "corrupt cache: string len %u exceeds remaining %u bytes", (unsigned)len,
            (unsigned)(fileLen - pos));
    s.clear();
    return;
  }
  s.resize(len);
  file.read(&s[0], len);
}
}  // namespace serialization
