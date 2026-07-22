#include "vendor/crc32c.h"

#include "TestUtil.h"
#include <gtest/gtest.h>

using namespace ucache;

TEST(Crc32c, StandardCheckVector) {
  // The canonical CRC-32C check value.
  EXPECT_EQ(crc32c("123456789", 9), 0xE3069283u);
}

TEST(Crc32c, Empty) { EXPECT_EQ(crc32c("", 0), 0u); }

TEST(Crc32c, IncrementalMatchesOneShot) {
  auto buf = test::randomBytes(100000, 42);
  uint32_t one = crc32c(buf.data(), buf.size());
  uint32_t inc = 0;
  size_t chunks[] = {1, 7, 8, 63, 64, 4096, 95761};
  size_t off = 0;
  for (size_t c : chunks) {
    inc = crc32c(inc, buf.data() + off, c);
    off += c;
  }
  ASSERT_EQ(off, buf.size());
  EXPECT_EQ(inc, one);
}

TEST(Crc32c, SwPathCheckVector) {
  EXPECT_EQ(detail::crc32cSw(0, "123456789", 9), 0xE3069283u);
}

TEST(Crc32c, HwAndSwAgree) {
  if (!detail::crc32cHwAvailable())
    GTEST_SKIP() << "no SSE4.2";
  for (uint64_t seed : {1u, 2u, 3u}) {
    auto buf = test::randomBytes(4096 + seed * 13, seed);
    EXPECT_EQ(detail::crc32cHw(0, buf.data(), buf.size()),
              detail::crc32cSw(0, buf.data(), buf.size()));
  }
  // Unaligned starts exercise the byte-loop heads.
  auto buf = test::randomBytes(1000, 99);
  for (int shift = 0; shift < 8; ++shift)
    EXPECT_EQ(detail::crc32cHw(0, buf.data() + shift, 900),
              detail::crc32cSw(0, buf.data() + shift, 900));
}
