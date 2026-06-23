#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <numeric>
#include <vector>

#include "lib/Serialization/BufferedHalReader.h"

// Pure in-memory mock with the same duck-typed surface BufferedReader requires
// (int read(void*, size_t) / bool seek(size_t) / size_t position() / size_t
// size()). No SD, no HalStorage — keeps this test IO-free. It also counts the
// number of read() round-trips so we can assert the buffer issues block reads,
// not per-byte reads.
class MockReader {
 public:
  explicit MockReader(std::vector<uint8_t> bytes) : data(std::move(bytes)) {}

  int read(void* dst, size_t n) {
    size_t avail = (pos < data.size()) ? (data.size() - pos) : 0;
    size_t take = (n < avail) ? n : avail;
    if (take > 0) memcpy(dst, data.data() + pos, take);
    pos += take;
    ++readCalls;
    return static_cast<int>(take);
  }
  bool seek(size_t p) {
    if (p > data.size()) return false;
    pos = p;
    ++seekCalls;
    return true;
  }
  size_t position() const { return pos; }
  size_t size() { return data.size(); }

  const std::vector<uint8_t> data;
  size_t pos = 0;
  int readCalls = 0;
  int seekCalls = 0;
};

namespace {
std::vector<uint8_t> makeRamp(size_t n) {
  std::vector<uint8_t> v(n);
  for (size_t i = 0; i < n; ++i) v[i] = static_cast<uint8_t>(i * 31 + 7);  // pseudo-pattern
  return v;
}
}  // namespace

// (a) Byte-for-byte fidelity vs reading the mock directly, for a mix of read
// sizes including reads that straddle the 4 KiB buffer boundary.
TEST(BufferedReader, ByteForByteAcrossBoundary) {
  // Larger than several buffers so refills and boundary spans are exercised.
  const std::vector<uint8_t> bytes = makeRamp(BufferedReader<MockReader>::kCapacity * 3 + 123);
  MockReader src(bytes);
  BufferedReader<MockReader> reader(src);

  // Read sizes chosen so cumulative offsets land on, before, and after the 4 KiB
  // boundary, and so single reads span it (e.g. 1000 + 1000 + ... crosses 4096).
  const size_t sizes[] = {1, 2, 3, 7, 13, 1000, 1000, 1000, 1000, 1000, 5000, 17, 4096, 2, 250};
  std::vector<uint8_t> out;
  out.reserve(bytes.size());
  for (size_t want : sizes) {
    std::vector<uint8_t> chunk(want);
    int got = reader.read(chunk.data(), want);
    ASSERT_GE(got, 0);
    out.insert(out.end(), chunk.begin(), chunk.begin() + got);
    if (static_cast<size_t>(got) < want) break;  // hit EOF
  }
  // Drain the remainder so we compare the whole stream.
  for (;;) {
    uint8_t tmp[333];
    int got = reader.read(tmp, sizeof(tmp));
    if (got <= 0) break;
    out.insert(out.end(), tmp, tmp + got);
  }

  EXPECT_EQ(out, bytes);
  EXPECT_EQ(reader.position(), bytes.size());
  EXPECT_EQ(reader.read(nullptr, 0), 0);  // no-op read at EOF
}

// (a') A single read larger than the buffer capacity must be served correctly by
// draining the window, refilling, and continuing.
TEST(BufferedReader, SingleReadLargerThanBuffer) {
  const std::vector<uint8_t> bytes = makeRamp(BufferedReader<MockReader>::kCapacity * 2 + 500);
  MockReader src(bytes);
  BufferedReader<MockReader> reader(src);

  std::vector<uint8_t> out(bytes.size());
  int got = reader.read(out.data(), out.size());
  ASSERT_EQ(static_cast<size_t>(got), bytes.size());
  EXPECT_EQ(out, bytes);
}

// (b) seek forward / backward / random, then read yields the correct bytes.
TEST(BufferedReader, SeekForwardBackwardRandom) {
  const std::vector<uint8_t> bytes = makeRamp(BufferedReader<MockReader>::kCapacity * 2 + 64);
  MockReader src(bytes);
  BufferedReader<MockReader> reader(src);

  auto readAt = [&](size_t pos, size_t n) {
    EXPECT_TRUE(reader.seek(pos));
    EXPECT_EQ(reader.position(), pos);
    std::vector<uint8_t> chunk(n);
    int got = reader.read(chunk.data(), n);
    EXPECT_EQ(static_cast<size_t>(got), n);
    std::vector<uint8_t> expect(bytes.begin() + pos, bytes.begin() + pos + n);
    EXPECT_EQ(chunk, expect);
  };

  readAt(0, 16);                                  // start
  readAt(100, 50);                                // forward, within first buffer
  readAt(10, 20);                                 // backward into already-buffered window
  readAt(BufferedReader<MockReader>::kCapacity + 5, 40);  // random, forces a fresh window
  readAt(BufferedReader<MockReader>::kCapacity - 3, 10);  // straddle boundary after a seek
  readAt(bytes.size() - 8, 8);                    // last bytes

  // In-window seek must not touch the underlying file: read a block, then seek
  // back inside it and confirm no extra underlying read happened.
  reader.seek(0);
  std::vector<uint8_t> warm(200);
  reader.read(warm.data(), warm.size());
  const int readsBefore = src.readCalls;
  EXPECT_TRUE(reader.seek(50));  // inside [0, bufLen]
  uint8_t one;
  EXPECT_EQ(reader.read(&one, 1), 1);
  EXPECT_EQ(one, bytes[50]);
  EXPECT_EQ(src.readCalls, readsBefore);  // served from buffer, no new underlying read
}

// (c) The underlying read() is called a handful of times (block reads), not once
// per byte. Reading the whole file one byte at a time must issue ~ceil(N/kCap)
// underlying reads, far fewer than N.
TEST(BufferedReader, BlockReadsNotPerByte) {
  const size_t n = BufferedReader<MockReader>::kCapacity * 3 + 200;  // 3 full + 1 partial
  const std::vector<uint8_t> bytes = makeRamp(n);
  MockReader src(bytes);
  BufferedReader<MockReader> reader(src);

  std::vector<uint8_t> out;
  out.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    uint8_t b;
    int got = reader.read(&b, 1);
    ASSERT_EQ(got, 1);
    out.push_back(b);
  }
  EXPECT_EQ(out, bytes);

  // Refills needed: 4 full/partial windows for 3*kCap+200 bytes. Allow the EOF
  // probe read (returns 0) as one extra. The key assertion: << n.
  const int expectedRefills = static_cast<int>((n + BufferedReader<MockReader>::kCapacity - 1) /
                                               BufferedReader<MockReader>::kCapacity);
  EXPECT_LE(src.readCalls, expectedRefills + 1);
  EXPECT_GE(src.readCalls, expectedRefills);
  EXPECT_LT(src.readCalls, static_cast<int>(n));  // emphatically not per-byte
}

// (d) Logical position invariant: position() reports bytes consumed by the
// caller, never the prefetch offset of the underlying source. This is what
// serialization::readString's remaining-bytes guard relies on.
TEST(BufferedReader, LogicalPositionNotPrefetchOffset) {
  const std::vector<uint8_t> bytes = makeRamp(BufferedReader<MockReader>::kCapacity * 2);
  MockReader src(bytes);
  BufferedReader<MockReader> reader(src);

  uint8_t tmp[10];
  reader.read(tmp, sizeof(tmp));
  // The buffer has prefetched a full window, so the underlying source has
  // advanced far past 10. The reader must still report logical position 10.
  EXPECT_EQ(reader.position(), 10u);
  EXPECT_GT(src.position(), reader.position());  // prefetch ran ahead underneath
  EXPECT_EQ(reader.size(), bytes.size());        // size is the fixed file length
  EXPECT_EQ(reader.available(), bytes.size() - 10);
}

// (e) Reads past EOF return a short count and stop, mirroring HalFile semantics
// (so existing short-read checks keep working).
TEST(BufferedReader, ShortReadAtEof) {
  const std::vector<uint8_t> bytes = makeRamp(100);
  MockReader src(bytes);
  BufferedReader<MockReader> reader(src);

  uint8_t buf[256];
  int got = reader.read(buf, sizeof(buf));
  EXPECT_EQ(got, 100);
  EXPECT_EQ(reader.position(), 100u);
  EXPECT_EQ(reader.read(buf, 10), 0);  // nothing left
}

// (f) Pre-positioned source: production wraps the file AFTER seek(pagePos), so the
// reader is constructed over a source already at a non-zero offset. position() must
// equal that offset immediately and the first read must return bytes from there, not
// from offset 0. (Regression: the constructor previously seeded logicalPos=0 while the
// underlying was at pagePos, so the first refill seeked back to 0 and read the wrong page.)
TEST(BufferedReader, PrePositionedSource) {
  const std::vector<uint8_t> bytes = makeRamp(BufferedReader<MockReader>::kCapacity * 2 + 300);
  const size_t startAt = 777;  // non-zero, non-aligned, like a real pagePos
  MockReader src(bytes);
  ASSERT_TRUE(src.seek(startAt));
  BufferedReader<MockReader> reader(src);

  EXPECT_EQ(reader.position(), startAt);                  // logical == absolute file offset
  EXPECT_EQ(reader.size(), bytes.size());
  EXPECT_EQ(reader.available(), bytes.size() - startAt);

  std::vector<uint8_t> chunk(64);
  int got = reader.read(chunk.data(), chunk.size());
  ASSERT_EQ(static_cast<size_t>(got), chunk.size());
  const std::vector<uint8_t> expect(bytes.begin() + startAt, bytes.begin() + startAt + 64);
  EXPECT_EQ(chunk, expect);                               // bytes from startAt, NOT from 0
  EXPECT_EQ(reader.position(), startAt + 64);
}
