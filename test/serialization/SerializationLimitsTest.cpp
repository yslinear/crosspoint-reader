#include <gtest/gtest.h>

#include "lib/Serialization/SerializationLimits.h"

using namespace serialization;

// Locks the corrupt-cache guard added to readString(): a bogus on-disk length must be
// rejected BEFORE it reaches std::string::resize(), which on the ~380KB device heap
// would attempt a huge allocation that aborts under -fno-exceptions. If anyone removes
// or weakens the cap, these fail.

TEST(SerializationLimits, AcceptsPlausibleLengths) {
  EXPECT_TRUE(serializedStringLenWithinCap(0));
  EXPECT_TRUE(serializedStringLenWithinCap(1));
  EXPECT_TRUE(serializedStringLenWithinCap(255));
  EXPECT_TRUE(serializedStringLenWithinCap(MAX_SERIALIZED_STRING_LEN));  // boundary: inclusive
}

TEST(SerializationLimits, RejectsOverCapAndCorruptLengths) {
  EXPECT_FALSE(serializedStringLenWithinCap(MAX_SERIALIZED_STRING_LEN + 1));
  EXPECT_FALSE(serializedStringLenWithinCap(1u << 20));        // 1 MB
  EXPECT_FALSE(serializedStringLenWithinCap(0xFFFFFFFFu));     // classic corrupt-cache length
}

TEST(SerializationLimits, CapStaysWithinASafeSingleAllocation) {
  // The cap itself is the worst-case single resize() — keep it modest vs the heap.
  EXPECT_LE(MAX_SERIALIZED_STRING_LEN, 64u * 1024u);
}
