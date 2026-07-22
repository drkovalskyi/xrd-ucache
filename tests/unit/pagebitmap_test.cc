#include "PageBitmap.h"

#include <cstring>
#include <gtest/gtest.h>

using namespace ucache;

TEST(PageBitmap, SetGetClearCount) {
  PageBitmap b(100);
  EXPECT_EQ(b.npages(), 100u);
  EXPECT_EQ(b.count(), 0u);
  EXPECT_FALSE(b.get(0));
  b.set(0);
  b.set(7);
  b.set(8); // crosses byte boundary
  b.set(99);
  EXPECT_EQ(b.count(), 4u);
  b.set(0); // idempotent
  EXPECT_EQ(b.count(), 4u);
  EXPECT_TRUE(b.get(0));
  EXPECT_TRUE(b.get(99));
  EXPECT_FALSE(b.get(50));
  b.clear(7);
  EXPECT_EQ(b.count(), 3u);
  b.clear(7); // idempotent
  EXPECT_EQ(b.count(), 3u);
  b.clearAll();
  EXPECT_EQ(b.count(), 0u);
  EXPECT_FALSE(b.get(0));
}

TEST(PageBitmap, RangeSet) {
  PageBitmap b(64);
  for (uint64_t i = 10; i <= 20; ++i)
    b.set(i);
  EXPECT_TRUE(b.rangeSet(10, 20));
  EXPECT_TRUE(b.rangeSet(15, 15));
  EXPECT_FALSE(b.rangeSet(9, 20));
  EXPECT_FALSE(b.rangeSet(10, 21));
}

TEST(PageBitmap, RawRoundtripRecount) {
  PageBitmap a(77);
  for (uint64_t i = 0; i < 77; i += 3)
    a.set(i);
  PageBitmap b(77);
  std::memcpy(b.raw(), a.raw(), a.rawSize());
  b.recount();
  EXPECT_EQ(b.count(), a.count());
  for (uint64_t i = 0; i < 77; ++i)
    EXPECT_EQ(b.get(i), a.get(i));
}
