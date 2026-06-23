#pragma once
#include <Memory.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

// Forward-biased buffered reader over a duck-typed sequential source.
//
// Why: every HalFile::read()/seek()/position()/size() call takes the storage
// mutex and hits SdFat. A single Page::deserialize() issues dozens of tiny POD
// and string reads, so one page turn = dozens of mutex+SdFat round-trips. This
// wrapper pulls bytes from the underlying file in >=512B blocks and serves the
// fine-grained reads from RAM, collapsing those round-trips to a handful.
//
// The wrapped source must expose:
//   int    read(void* dst, size_t n)   // returns bytes read (>=0), <n at EOF
//   bool   seek(size_t pos)            // absolute reposition
//   size_t position() const            // current absolute offset
//   size_t size()                      // total length in bytes
// HalFile satisfies this, so BufferedHalReader = BufferedReader<HalFile>.
//
// CRITICAL INVARIANT (serialization::readString depends on it):
//   position() returns the LOGICAL stream position (bytes the caller has
//   consumed), NOT the prefetch offset of the underlying file. size() returns
//   the underlying file size (fixed). readString()'s remaining-bytes guard does
//   (size() - position()); if position() leaked the prefetch offset that guard
//   would under-report remaining bytes and spuriously reject valid strings.
//
// On-disk layout is unchanged: this only alters HOW bytes are pulled, not the
// byte sequence or order. No cache version bump is needed.
template <typename Underlying>
class BufferedReader {
 public:
  // 4 KiB: a full SD page-deserialize fits in a few refills. The buffer is
  // transient — alive only for the duration of one loadPageFromSectionFile()
  // call (stack-scoped at the call site), freed at scope exit. One 4 KiB
  // allocation against the ~380 KB ceiling, with no per-read heap churn.
  static constexpr size_t kCapacity = 4096;

  explicit BufferedReader(Underlying& src)
      : underlying(src),
        fileSize(src.size()),
        underlyingPos(src.position()),
        logicalPos(src.position()),
        bufStart(src.position()) {
    // logicalPos is seeded to the source's ABSOLUTE offset (the file is usually
    // pre-seeked to pagePos before wrapping). Keeping logicalPos == absolute file
    // offset means refill() reads from the right place (no spurious seek-to-0) and
    // readString()'s size()-position() remaining-bytes guard stays correct.
    buffer = makeUniqueNoThrow<uint8_t[]>(kCapacity);
    // On OOM, buffer is null: read()/seek() fall back to direct underlying
    // access (correct, just unbuffered) rather than aborting.
  }

  // Serve n bytes into dst from the buffer, refilling from the underlying file
  // as needed. Handles a request spanning the buffer boundary (drain remainder,
  // refill, continue). Returns the number of bytes actually delivered (may be
  // < n only at end of file), matching HalFile::read() semantics so existing
  // short-read checks (e.g. footnote loop) keep working.
  int read(void* dst, size_t n) {
    if (!buffer) {
      return unbufferedRead(dst, n);
    }
    auto* out = static_cast<uint8_t*>(dst);
    size_t delivered = 0;
    while (delivered < n) {
      if (bufPos >= bufLen) {
        if (!refill()) break;  // EOF or read error
        if (bufLen == 0) break;
      }
      const size_t avail = bufLen - bufPos;
      const size_t take = (n - delivered < avail) ? (n - delivered) : avail;
      memcpy(out + delivered, buffer.get() + bufPos, take);
      bufPos += take;
      logicalPos += take;
      delivered += take;
    }
    return static_cast<int>(delivered);
  }

  // Absolute reposition of the LOGICAL stream. If pos lands inside the bytes
  // currently buffered, just move the cursor (no SD access). Otherwise drop the
  // window; the underlying file is repositioned lazily on the next refill.
  bool seek(size_t pos) {
    if (!buffer) {
      logicalPos = pos;
      if (!underlying.seek(pos)) return false;
      underlyingPos = pos;
      return true;
    }
    if (pos >= bufStart && pos <= bufStart + bufLen) {
      bufPos = pos - bufStart;
      logicalPos = pos;
      return true;
    }
    // Out of window: invalidate. Defer the underlying seek to refill() so a
    // seek immediately followed by another seek costs no SD access.
    bufStart = pos;
    bufLen = 0;
    bufPos = 0;
    logicalPos = pos;
    return true;
  }

  // Logical stream position: bytes consumed by the caller. See class invariant.
  size_t position() const { return logicalPos; }

  // Underlying file size (fixed). See class invariant.
  size_t size() const { return fileSize; }

  // Bytes remaining from the current logical position to EOF.
  size_t available() const { return (logicalPos < fileSize) ? (fileSize - logicalPos) : 0; }

 private:
  // Refill the window starting at the current logical position. Returns false
  // on EOF/error (bufLen left at 0). Repositions the underlying file only when
  // its actual offset has drifted from where we need to read.
  bool refill() {
    const size_t readFrom = logicalPos;
    if (readFrom >= fileSize) {
      bufStart = readFrom;
      bufLen = 0;
      bufPos = 0;
      return false;
    }
    if (underlyingPos != readFrom) {
      if (!underlying.seek(readFrom)) return false;
      underlyingPos = readFrom;
    }
    size_t want = fileSize - readFrom;
    if (want > kCapacity) want = kCapacity;
    const int got = underlying.read(buffer.get(), want);
    if (got <= 0) {
      bufStart = readFrom;
      bufLen = 0;
      bufPos = 0;
      return false;
    }
    underlyingPos += static_cast<size_t>(got);
    bufStart = readFrom;
    bufLen = static_cast<size_t>(got);
    bufPos = 0;
    return true;
  }

  // Fallback path when the buffer could not be allocated: pass reads straight
  // through to the underlying file, keeping logical/underlying offsets in sync.
  int unbufferedRead(void* dst, size_t n) {
    if (underlyingPos != logicalPos) {
      if (!underlying.seek(logicalPos)) return 0;
      underlyingPos = logicalPos;
    }
    const int got = underlying.read(dst, n);
    if (got > 0) {
      logicalPos += static_cast<size_t>(got);
      underlyingPos += static_cast<size_t>(got);
    }
    return got;
  }

  Underlying& underlying;
  std::unique_ptr<uint8_t[]> buffer;  // kCapacity bytes, or null on OOM
  size_t fileSize = 0;                // cached underlying size (fixed)
  size_t underlyingPos = 0;          // actual offset of the underlying file
  size_t logicalPos = 0;             // bytes consumed by the caller (logical position)
  size_t bufStart = 0;               // logical offset of buffer[0]
  size_t bufLen = 0;                 // valid bytes currently in buffer
  size_t bufPos = 0;                 // cursor within buffer [0, bufLen]
};

class HalFile;
using BufferedHalReader = BufferedReader<HalFile>;
