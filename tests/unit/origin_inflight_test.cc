// The origin-in-flight guard. It is a plain RAII counter over Stats, with no
// XrdCl in it, so its contract is proven here rather than through the plugin.
//
// What it is FOR: how many reads the origin was being asked for at once, which
// is the only width that sets the origin's delivery rate. It therefore has to
// span the READ -- issue to completion -- and not the call that issued it, and
// not the lifetime of whatever object happens to hold it.
#include "OriginInFlight.h"

#include <gtest/gtest.h>

using ucache::OriginInFlight;
using ucache::Stats;

TEST(OriginInFlight, CountsUpAtIssueAndDownAtRelease) {
  Stats s;
  EXPECT_EQ(s.originReadsInFlight.load(), 0u);
  {
    OriginInFlight g(&s);
    EXPECT_EQ(s.originReadsInFlight.load(), 1u);
    EXPECT_EQ(s.originReadsInFlightHighWater.load(), 1u);
  }
  EXPECT_EQ(s.originReadsInFlight.load(), 0u);
  // The high-water is the point of the counter and does not decay.
  EXPECT_EQ(s.originReadsInFlightHighWater.load(), 1u);
}

TEST(OriginInFlight, ReleaseIsWhereTheReadEndsNotWhereTheObjectDies) {
  // The property the read path depends on. A completion handler releases at
  // the TOP, then forwards to the caller; a caller that issues its next read
  // from inside its own handler must not be counted alongside the read that
  // has already landed. Holding until destruction reports two where the origin
  // had one, and the count becomes an artefact of handler lifetimes.
  Stats s;
  OriginInFlight first(&s);
  EXPECT_EQ(s.originReadsInFlight.load(), 1u);
  first.release(); // the read landed; the handler has not been destroyed yet
  EXPECT_EQ(s.originReadsInFlight.load(), 0u);
  {
    OriginInFlight next(&s); // what the caller starts from inside its handler
    EXPECT_EQ(s.originReadsInFlight.load(), 1u);
  }
  EXPECT_EQ(s.originReadsInFlight.load(), 0u);
  // One read was outstanding at a time, and that is what is reported.
  EXPECT_EQ(s.originReadsInFlightHighWater.load(), 1u);
}

TEST(OriginInFlight, ReleasingTwiceGivesBackOnlyOne) {
  // release() then ~OriginInFlight() is the ordinary path once the handler is
  // destroyed, so a second give-back must be a no-op. Underflowing an unsigned
  // counter here would report about eighteen quintillion reads in flight.
  Stats s;
  {
    OriginInFlight g(&s);
    g.release();
    g.release();
    EXPECT_EQ(s.originReadsInFlight.load(), 0u);
  }
  EXPECT_EQ(s.originReadsInFlight.load(), 0u);
}

TEST(OriginInFlight, MovingHandsOverTheOneRead) {
  // The guard is handed into the handler that owns the read, so a move must
  // transfer the debt rather than duplicate or drop it.
  Stats s;
  OriginInFlight a(&s);
  EXPECT_EQ(s.originReadsInFlight.load(), 1u);
  OriginInFlight b(std::move(a));
  EXPECT_EQ(s.originReadsInFlight.load(), 1u) << "the move is not a second read";
  {
    OriginInFlight c;
    c = std::move(b);
    EXPECT_EQ(s.originReadsInFlight.load(), 1u);
  }
  EXPECT_EQ(s.originReadsInFlight.load(), 0u) << "the destination owns it now";
}

TEST(OriginInFlight, MoveAssigningOverALiveGuardGivesThatOneBack) {
  // relayToInner assigns a constructed guard over a default-constructed member.
  // If the destination were ever live, dropping it silently would leak a read
  // that never comes back and pin the counter above zero for the process.
  Stats s;
  OriginInFlight dst(&s);
  OriginInFlight src(&s);
  EXPECT_EQ(s.originReadsInFlight.load(), 2u);
  dst = std::move(src);
  EXPECT_EQ(s.originReadsInFlight.load(), 1u) << "the overwritten guard was given back";
}

TEST(OriginInFlight, NoStatsIsNotACrash) {
  // Handles built before a store exists carry a null Stats; every route
  // constructs the guard unconditionally.
  OriginInFlight g(nullptr);
  g.release();
  OriginInFlight d;
  d.release();
  SUCCEED();
}

TEST(OriginInFlight, HighWaterIsTheConcurrentPeakNotATotal) {
  // Sequential reads are the discriminating case: ten of them one after
  // another are ten reads and a width of ONE, and a counter that failed to
  // give each back would report ten.
  Stats s;
  for (int i = 0; i < 10; ++i)
    OriginInFlight g(&s);
  EXPECT_EQ(s.originReadsInFlightHighWater.load(), 1u);
  {
    OriginInFlight a(&s), b(&s), c(&s);
    EXPECT_EQ(s.originReadsInFlightHighWater.load(), 3u);
  }
  EXPECT_EQ(s.originReadsInFlight.load(), 0u);
  EXPECT_EQ(s.originReadsInFlightHighWater.load(), 3u);
}
