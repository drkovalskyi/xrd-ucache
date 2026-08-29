// Read footprints: what a run READ, in origin coordinates.
//
// Every property here was established by a route disagreeing with another in
// the field, not by reasoning. Four routes over one analysis must produce one
// signature, and the ways that failed are what these tests hold shut.
#include "ReadFootprint.h"

#include <gtest/gtest.h>

using ucache::ReadFootprint;

namespace {
constexpr uint64_t kB = ReadFootprint::kBucket;
}

TEST(ReadFootprint, EmptyHasNoSignature) {
  ReadFootprint f;
  EXPECT_TRUE(f.empty());
  EXPECT_TRUE(f.sig().empty());
  EXPECT_EQ(f.count(), 0u);
}

TEST(ReadFootprint, OrderOfReadsCannotChangeIt) {
  // Threads deliver reads in whatever order they finish; a signature that
  // depended on that would differ between two identical runs.
  ReadFootprint a, b;
  a.note(0, 10);
  a.note(5 * kB, 10);
  a.note(2 * kB, 10);
  b.note(2 * kB, 10);
  b.note(0, 10);
  b.note(5 * kB, 10);
  EXPECT_EQ(a.sig(), b.sig());
  EXPECT_EQ(a.count(), 3u);
}

TEST(ReadFootprint, ReadingTheSameBucketTwiceIsTheSameAsOnce) {
  // It is a SET of touched regions. A commutative combination that cancelled
  // on repetition (xor of values) would erase a re-read.
  ReadFootprint a, b;
  a.note(0, 10);
  b.note(0, 10);
  b.note(3, 4);
  b.note(0, 10);
  EXPECT_EQ(a.sig(), b.sig());
  EXPECT_EQ(b.count(), 1u);
}

TEST(ReadFootprint, DifferentRegionsGiveDifferentSignatures) {
  ReadFootprint a, b;
  a.note(0, 10);
  b.note(9 * kB, 10);
  EXPECT_NE(a.sig(), b.sig());
}

TEST(ReadFootprint, ARangeSpanningBucketsMarksAllOfThem) {
  ReadFootprint f;
  f.note(kB - 1, 2 * kB); // straddles three
  EXPECT_EQ(f.count(), 3u);
}

// The failure that made four routes give three answers. ROOT fetches a file's
// tail in fixed-size blocks and so asks past the end; a route serving the
// request verbatim recorded those bytes, while a route mapping them through a
// replica's layout did not. Past-EOF bytes are not file content.
TEST(ReadFootprint, ReadsPastTheEndOfTheFileAreNotContent) {
  const uint64_t size = 3 * kB + 100;
  ReadFootprint plain, overrun;
  plain.note(0, size);
  overrun.note(0, size);
  overrun.note(size, 5 * kB); // the tail fetch that runs off the end
  EXPECT_NE(plain.sig(0), overrun.sig(0)) << "unclamped, they differ";
  EXPECT_EQ(plain.sig(size), overrun.sig(size))
      << "clamped to the origin size, the overrun contributes nothing";
  EXPECT_EQ(overrun.count(size), 4u);
}

TEST(ReadFootprint, AHostileRangeIsRefusedRatherThanAllocated) {
  // A corrupt or adversarial length must not turn a diagnostic into an
  // enormous allocation; losing the signature is the right price.
  ReadFootprint f;
  f.note(0, ~uint64_t(0));
  EXPECT_TRUE(f.empty());
}

// The granularity was chosen by measurement: the routes coalesce differently,
// so the byte tier transfers gaps between baskets that a replica read never
// names. At 64 KiB those gaps fell in buckets one route marked and the other
// did not. This pins the property that made 1 MiB the choice -- a sub-bucket
// gap cannot separate two runs -- without pinning the constant itself.
TEST(ReadFootprint, ASmallGapBetweenTwoReadsDoesNotChangeTheAnswer) {
  ReadFootprint merged, split;
  merged.note(0, kB / 2);            // one coalesced request over the gap
  split.note(0, kB / 8);             // the two pieces either side of it
  split.note(kB / 4, kB / 4);
  EXPECT_EQ(merged.sig(), split.sig());
}
