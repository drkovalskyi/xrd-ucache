// Slot store tests. What matters most: a record is served only when its bytes
// are exactly what was committed, for that slot, in that store; commits from
// several processes neither lose nor duplicate a slot; a store whose header or
// layout cannot be trusted is never used; a dropped or replaced store takes no
// more records.
#include "SlotStore.h"

#include "IOBackend.h"
#include "TestUtil.h"
#include "vendor/crc32c.h"

#include <fcntl.h>
#include <gtest/gtest.h>
#include <atomic>
#include <cstring>
#include <chrono>
#include <map>
#include <sys/file.h>
#include <thread>
#include <set>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utime.h>

using namespace ucache;
using test::TempDir;

namespace {

const char* kHash = "abcdef0123456789";

SlotStoreHeader hdr(uint32_t nSlots = 100) {
  SlotStoreHeader h;
  h.layoutVersion = 1;
  h.container = 0;
  h.slotFactor100 = 300;
  h.codecs = "lzma,zlib";
  h.originSize = 123456;
  h.originMtime = 1700000000;
  h.virtualSize = 999999;
  h.nSlots = nSlots;
  h.layoutHash = 0x1234567890abcdefull;
  return h;
}

std::vector<uint8_t> blob(size_t n = 10000) {
  std::vector<uint8_t> b(n);
  for (size_t i = 0; i < n; ++i)
    b[i] = static_cast<uint8_t>(i * 13 + 5);
  return b;
}

SlotRecord rec(uint32_t slot, size_t n, uint8_t seed) {
  SlotRecord r;
  r.slot = slot;
  r.kind = SlotEntry::kZstd;
  r.bytes.resize(n);
  for (size_t i = 0; i < n; ++i)
    r.bytes[i] = static_cast<uint8_t>(seed + i * 7);
  return r;
}

std::shared_ptr<SlotStore> make(RealIO& io, const std::string& dir, bool* created = nullptr,
                                const SlotStoreHeader& h = hdr(),
                                const std::vector<uint8_t>& b = blob()) {
  bool c = false;
  std::string err;
  auto s = SlotStore::openOrCreate(io, dir, kHash, h, b, c, err);
  if (created)
    *created = c;
  return s;
}

int64_t commitAll(SlotStore& s, std::vector<SlotRecord> recs, std::vector<SlotEntry>& out) {
  return s.commit(recs, nullptr, nullptr, false, out);
}

std::map<uint32_t, SlotEntry> lastEntries(RealIO& io, const std::string& dir) {
  std::map<uint32_t, SlotEntry> m;
  auto s = SlotStore::open(io, dir, kHash);
  if (s)
    for (const auto& e : s->refresh(true))
      m[e.slot] = e;
  return m;
}

} // namespace

TEST(SlotStore, HeaderRoundTripsAndRejectsCorruption) {
  SlotStoreHeader in = hdr();
  in.storeId = 42;
  in.declined = true;
  auto b = encodeSlotHeader(in);
  SlotStoreHeader h;
  ASSERT_TRUE(decodeSlotHeader(b.data(), b.size(), h));
  EXPECT_EQ(h.slotFactor100, 300);
  EXPECT_EQ(h.codecs, "lzma,zlib");
  EXPECT_EQ(h.layoutHash, 0x1234567890abcdefull);
  EXPECT_EQ(h.storeId, 42u);
  EXPECT_TRUE(h.declined);
  b[70] ^= 1;
  EXPECT_FALSE(decodeSlotHeader(b.data(), b.size(), h));
}

// A fractional factor round-trips; a header of another format version (the
// first one kept the factor as a whole number in one byte) is not read, so a
// new store replaces it rather than serving slots of the wrong size.
TEST(SlotStore, TheFactorIsKeptInHundredthsAndOtherFormatsAreNotRead) {
  SlotStoreHeader in = hdr();
  in.slotFactor100 = 250;
  auto b = encodeSlotHeader(in);
  SlotStoreHeader h;
  ASSERT_TRUE(decodeSlotHeader(b.data(), b.size(), h));
  EXPECT_EQ(h.slotFactor100, 250);
  b[8] = 1; // format_version 1
  const uint32_t crc = crc32c(b.data(), SlotStore::kHeaderBytes - 4);
  std::memcpy(b.data() + SlotStore::kHeaderBytes - 4, &crc, 4);
  EXPECT_FALSE(decodeSlotHeader(b.data(), b.size(), h));
}

TEST(SlotStore, TheFirstCreatorFixesHeaderAndLayout) {
  TempDir t;
  RealIO io;
  bool created = false;
  auto a = make(io, t.path(), &created);
  ASSERT_TRUE(a);
  EXPECT_TRUE(created);
  EXPECT_NE(a->header().storeId, 0u);
  EXPECT_EQ(a->layoutBlob(), blob());
  SlotStoreHeader other = hdr();
  other.slotFactor100 = 400;
  auto b = make(io, t.path(), &created, other, blob(50));
  ASSERT_TRUE(b);
  EXPECT_FALSE(created);
  EXPECT_EQ(b->header().slotFactor100, 300); // what was created, not what was asked for
  EXPECT_EQ(b->header().storeId, a->header().storeId);
  EXPECT_EQ(b->layoutBlob(), blob()); // the layout everyone serves
  EXPECT_FALSE(SlotStore::open(io, t.path(), "nosuchhash"));
}

TEST(SlotStore, ACorruptLayoutMakesTheStoreUnusable) {
  TempDir t;
  RealIO io;
  ASSERT_TRUE(make(io, t.path()));
  int fd = ::open(SlotStore::path(t.path(), kHash).c_str(), O_RDWR);
  uint8_t x;
  ASSERT_EQ(::pread(fd, &x, 1, SlotStore::kHeaderBytes + 77), 1);
  x ^= 1;
  ASSERT_EQ(::pwrite(fd, &x, 1, SlotStore::kHeaderBytes + 77), 1);
  ::close(fd);
  EXPECT_FALSE(SlotStore::open(io, t.path(), kHash));
}

TEST(SlotStore, CommittedRecordsReadBackVerified) {
  TempDir t;
  RealIO io;
  auto s = make(io, t.path());
  ASSERT_TRUE(s);
  std::vector<SlotEntry> out;
  std::vector<SlotRecord> recs = {rec(3, 1000, 1), rec(9, 25000, 2)};
  SlotRecord kept;
  kept.slot = 11;
  kept.kind = SlotEntry::kKept;
  recs.push_back(kept);
  ASSERT_EQ(commitAll(*s, recs, out), 26000);
  ASSERT_EQ(out.size(), 3u);
  std::vector<uint8_t> got;
  ASSERT_TRUE(s->readRecord(out[1], got));
  EXPECT_EQ(got, rec(9, 25000, 2).bytes);
  EXPECT_EQ(out[2].kind, SlotEntry::kKept);
  EXPECT_FALSE(s->readRecord(out[2], got)); // a kept slot has no bytes here

  auto r = SlotStore::open(io, t.path(), kHash);
  ASSERT_TRUE(r);
  auto seen = r->refresh(true);
  ASSERT_EQ(seen.size(), 3u);
  EXPECT_EQ(seen[0].slot, 3u);
  EXPECT_TRUE(r->refresh(true).empty()); // nothing new
  ASSERT_TRUE(r->readRecord(seen[0], got));
  EXPECT_EQ(got, rec(3, 1000, 1).bytes);
}

TEST(SlotStore, ARecordValidatesOnlyAsItselfInItsOwnStore) {
  const auto r = rec(5, 300, 9);
  const uint32_t c = SlotStore::recordCrc(1, 5, r.bytes.data(), r.bytes.size());
  EXPECT_NE(c, SlotStore::recordCrc(2, 5, r.bytes.data(), r.bytes.size())); // another store
  EXPECT_NE(c, SlotStore::recordCrc(1, 6, r.bytes.data(), r.bytes.size())); // another slot
  EXPECT_NE(c, SlotStore::recordCrc(1, 5, r.bytes.data(), r.bytes.size() - 1)); // shorter
  // An entry naming another slot's record fails.
  TempDir t;
  RealIO io;
  auto s = make(io, t.path());
  std::vector<SlotEntry> out;
  ASSERT_GT(commitAll(*s, {rec(1, 500, 1), rec(2, 500, 2)}, out), 0);
  SlotEntry swapped = out[0];
  swapped.slot = 2;
  std::vector<uint8_t> got;
  EXPECT_FALSE(s->readRecord(swapped, got));
}

TEST(SlotStore, ACommitSkipsSlotsSomeoneElseCommitted) {
  TempDir t;
  RealIO io;
  auto a = make(io, t.path());
  auto b = SlotStore::open(io, t.path(), kHash);
  ASSERT_TRUE(a && b);
  std::vector<SlotEntry> out;
  ASSERT_GT(commitAll(*a, {rec(5, 100, 5), rec(6, 100, 6)}, out), 0);

  std::set<uint32_t> known;
  std::vector<SlotRecord> mine = {rec(5, 100, 50), rec(7, 100, 7)};
  size_t others = 0;
  const int64_t n = b->commit(
      mine, [&](const std::vector<SlotEntry>& es) {
        for (const auto& e : es)
          known.insert(e.slot);
        others += es.size();
      },
      [&](uint32_t slot) { return known.count(slot) > 0; }, false, out);
  EXPECT_EQ(n, 100); // only slot 7 was written
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0].slot, 7u);
  EXPECT_EQ(others, 2u);
  auto last = lastEntries(io, t.path());
  ASSERT_EQ(last.size(), 3u);
  auto c = SlotStore::open(io, t.path(), kHash);
  std::vector<uint8_t> got;
  ASSERT_TRUE(c->readRecord(last[5], got));
  EXPECT_EQ(got, rec(5, 100, 5).bytes); // a's bytes, not b's
}

TEST(SlotStore, ARottedRecordFailsItsCheck) {
  TempDir t;
  RealIO io;
  auto s = make(io, t.path());
  std::vector<SlotEntry> out;
  ASSERT_GT(commitAll(*s, {rec(1, 4096, 1)}, out), 0);
  int fd = ::open(SlotStore::path(t.path(), kHash).c_str(), O_RDWR);
  uint8_t b;
  ASSERT_EQ(::pread(fd, &b, 1, out[0].off + 100), 1);
  b ^= 0x40;
  ASSERT_EQ(::pwrite(fd, &b, 1, out[0].off + 100), 1);
  ::close(fd);
  std::vector<uint8_t> got;
  EXPECT_FALSE(s->readRecord(out[0], got));
}

TEST(SlotStore, ABlockCutShortIsSteppedOverAndTheNextOneFound) {
  TempDir t;
  RealIO io;
  auto s = make(io, t.path());
  std::vector<SlotEntry> out;
  ASSERT_GT(commitAll(*s, {rec(1, 300, 1)}, out), 0);
  const std::string p = SlotStore::path(t.path(), kHash);
  struct ::stat st;
  ASSERT_EQ(::stat(p.c_str(), &st), 0);
  // A commit that died: a block whose header landed and whose records did not.
  const uint64_t before = static_cast<uint64_t>(st.st_size);
  {
    auto b2 = SlotStore::open(io, t.path(), kHash);
    ASSERT_GT(commitAll(*b2, {rec(2, 9000, 2)}, out), 0);
  }
  ASSERT_EQ(::truncate(p.c_str(), static_cast<off_t>(out[0].off + 100)), 0);
  // Garbage after it, too.
  {
    int fd = ::open(p.c_str(), O_WRONLY | O_APPEND);
    std::vector<uint8_t> junk(777, 0xAB);
    ASSERT_EQ(::write(fd, junk.data(), junk.size()), 777);
    ::close(fd);
  }
  auto c = SlotStore::open(io, t.path(), kHash);
  ASSERT_TRUE(c);
  ASSERT_GT(commitAll(*c, {rec(3, 400, 3)}, out), 0);
  EXPECT_GT(out[0].off, before);
  auto last = lastEntries(io, t.path());
  EXPECT_EQ(last.count(1), 1u);
  ASSERT_EQ(last.count(3), 1u);
  std::vector<uint8_t> got;
  ASSERT_TRUE(c->readRecord(last[3], got));
  EXPECT_EQ(got, rec(3, 400, 3).bytes);
  // Cut short: its header may pass once the file grows past its extent, but
  // its record never does.
  if (last.count(2)) {
    EXPECT_FALSE(c->readRecord(last[2], got));
  }
}

// The same, with a follow-up long enough to reach past the torn block's
// claimed extent: had it been written inside that extent, the torn header
// would pass its checks once the file grew, and a reader would jump over it.
TEST(SlotStore, NothingIsWrittenInsideATornBlocksExtent) {
  TempDir t;
  RealIO io;
  auto s = make(io, t.path());
  std::vector<SlotEntry> out;
  ASSERT_GT(commitAll(*s, {rec(1, 300, 1)}, out), 0);
  const std::string p = SlotStore::path(t.path(), kHash);
  {
    auto b2 = SlotStore::open(io, t.path(), kHash);
    ASSERT_GT(commitAll(*b2, {rec(2, 9000, 2)}, out), 0);
  }
  ASSERT_EQ(::truncate(p.c_str(), static_cast<off_t>(out[0].off + 100)), 0);
  auto c = SlotStore::open(io, t.path(), kHash);
  ASSERT_TRUE(c);
  ASSERT_GT(commitAll(*c, {rec(3, 12000, 3)}, out), 0);
  ASSERT_GT(commitAll(*c, {rec(4, 500, 4)}, out), 0);
  auto last = lastEntries(io, t.path());
  EXPECT_EQ(last.count(1), 1u);
  ASSERT_EQ(last.count(3), 1u);
  ASSERT_EQ(last.count(4), 1u);
  std::vector<uint8_t> got;
  ASSERT_TRUE(c->readRecord(last[3], got));
  EXPECT_EQ(got, rec(3, 12000, 3).bytes);
  if (last.count(2)) { // its header may now pass, but its record never does
    EXPECT_FALSE(c->readRecord(last[2], got));
  }
}

// Reading others' commits never waits: not for another process's commit, and
// not for this process's own commit that is waiting for the file lock.
TEST(SlotStore, ARefreshNeverWaitsForACommit) {
  TempDir t;
  RealIO io;
  auto s = make(io, t.path());
  std::vector<SlotEntry> out;
  ASSERT_GT(commitAll(*s, {rec(1, 100, 1)}, out), 0);
  // Another holder of the exclusive lock, as a stopped process would be.
  int fd = ::open(SlotStore::path(t.path(), kHash).c_str(), O_RDWR);
  ASSERT_GE(fd, 0);
  ASSERT_EQ(::flock(fd, LOCK_EX), 0);
  auto r = SlotStore::open(io, t.path(), kHash);
  ASSERT_TRUE(r);
  std::atomic<bool> committing{false}, done{false};
  std::thread committer([&] {
    std::vector<SlotEntry> o;
    committing = true;
    std::vector<SlotRecord> recs = {rec(2, 100, 2)};
    r->commit(recs, nullptr, nullptr, false, o); // blocks on the lock
    done = true;
  });
  while (!committing)
    std::this_thread::yield();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  const auto t0 = std::chrono::steady_clock::now();
  EXPECT_TRUE(r->refresh().empty());
  EXPECT_LT(std::chrono::steady_clock::now() - t0, std::chrono::milliseconds(100));
  EXPECT_FALSE(done.load());
  ::flock(fd, LOCK_UN);
  ::close(fd);
  committer.join();
  EXPECT_TRUE(done.load());
}

TEST(SlotStore, AStoreBeingCreatedIsNotUsedAndOldDebrisIsReplaced) {
  TempDir t;
  RealIO io;
  const std::string p = SlotStore::path(t.path(), kHash);
  {
    int fd = ::open(p.c_str(), O_WRONLY | O_CREAT, 0600);
    ASSERT_EQ(::write(fd, "UCSL", 4), 4);
    ::close(fd);
  }
  bool created = true;
  std::string err;
  EXPECT_FALSE(SlotStore::openOrCreate(io, t.path(), kHash, hdr(), blob(), created, err));
  EXPECT_NE(err.find("being written"), std::string::npos);
  EXPECT_FALSE(SlotStore::open(io, t.path(), kHash));
  struct utimbuf old{::time(nullptr) - 3600, ::time(nullptr) - 3600};
  ASSERT_EQ(::utime(p.c_str(), &old), 0);
  auto s = SlotStore::openOrCreate(io, t.path(), kHash, hdr(), blob(), created, err);
  ASSERT_TRUE(s);
  EXPECT_TRUE(created);
}

TEST(SlotStore, ADroppedOrReplacedStoreTakesNoMoreRecords) {
  TempDir t;
  RealIO io;
  auto s = make(io, t.path());
  std::vector<SlotEntry> out;
  ASSERT_GT(commitAll(*s, {rec(1, 64, 1)}, out), 0);
  SlotStore::drop(io, t.path(), kHash);
  std::vector<uint8_t> got;
  EXPECT_TRUE(s->readRecord(out[0], got)); // the open file outlives its name
  EXPECT_EQ(commitAll(*s, {rec(2, 64, 2)}, out), -ESTALE);
  auto fresh = make(io, t.path());
  ASSERT_TRUE(fresh);
  EXPECT_NE(fresh->header().storeId, s->header().storeId);
  EXPECT_EQ(commitAll(*s, {rec(3, 64, 3)}, out), -ESTALE); // replaced
  EXPECT_TRUE(fresh->refresh(true).empty()); // nothing of the old store's reached it
}

// A drop decided on one store never removes another: the one that replaced
// it, or one a commit holds right now.
TEST(SlotStore, ADropRemovesOnlyTheStoreItExamined) {
  TempDir t;
  RealIO io;
  auto s = make(io, t.path());
  ASSERT_TRUE(s);
  // Replaced meanwhile: the drop leaves the new store alone.
  SlotStore::drop(io, t.path(), kHash);
  auto fresh = make(io, t.path());
  ASSERT_TRUE(fresh);
  EXPECT_FALSE(s->dropIfCurrent());
  auto again = SlotStore::open(io, t.path(), kHash);
  ASSERT_TRUE(again);
  EXPECT_EQ(again->header().storeId, fresh->header().storeId);
  // Held by a commit (another process's lock on the file): not now.
  const int fd = ::open(SlotStore::path(t.path(), kHash).c_str(), O_RDONLY);
  ASSERT_GE(fd, 0);
  ASSERT_EQ(::flock(fd, LOCK_EX), 0);
  EXPECT_FALSE(fresh->dropIfCurrent());
  EXPECT_TRUE(SlotStore::open(io, t.path(), kHash));
  ::flock(fd, LOCK_UN);
  ::close(fd);
  // Its own, and free: removed, and its commits refused from then on.
  EXPECT_TRUE(fresh->dropIfCurrent());
  EXPECT_FALSE(SlotStore::open(io, t.path(), kHash));
  std::vector<SlotEntry> out;
  EXPECT_EQ(commitAll(*fresh, {rec(1, 64, 1)}, out), -ESTALE);
}

TEST(SlotStore, ADeclinedStoreHoldsNoRecords) {
  TempDir t;
  RealIO io;
  SlotStoreHeader h = hdr(0);
  h.declined = true;
  auto s = make(io, t.path(), nullptr, h, {});
  ASSERT_TRUE(s);
  EXPECT_TRUE(s->header().declined);
  std::vector<SlotEntry> out;
  EXPECT_LT(commitAll(*s, {rec(1, 10, 1)}, out), 0);
  auto r = SlotStore::open(io, t.path(), kHash);
  ASSERT_TRUE(r);
  EXPECT_TRUE(r->header().declined);
}

// Several processes committing at once, with overlapping slots: every slot
// ends up with one readable record whose bytes are one writer's copy.
TEST(SlotStore, ConcurrentProcessesNeitherLoseNorCorruptSlots) {
  TempDir t;
  RealIO io;
  ASSERT_TRUE(make(io, t.path()));
  constexpr int kProcs = 6, kSlots = 60, kRounds = 20;
  std::vector<pid_t> kids;
  for (int p = 0; p < kProcs; ++p) {
    pid_t pid = ::fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
      RealIO cio;
      auto s = SlotStore::open(cio, t.path(), kHash);
      if (!s)
        ::_exit(2);
      std::set<uint32_t> known;
      for (int r = 0; r < kRounds; ++r) {
        std::vector<SlotRecord> recs;
        for (int k = 0; k < 3; ++k) {
          const uint32_t slot = static_cast<uint32_t>((p * 7 + r * 3 + k) % kSlots);
          recs.push_back(rec(slot, 2000 + slot * 10, static_cast<uint8_t>(slot)));
        }
        for (const auto& e : s->refresh())
          known.insert(e.slot);
        std::vector<SlotEntry> out;
        const int64_t n = s->commit(
            recs, [&](const std::vector<SlotEntry>& es) {
              for (const auto& e : es)
                known.insert(e.slot);
            },
            [&](uint32_t slot) { return known.count(slot) > 0; }, false, out);
        if (n < 0)
          ::_exit(3);
        for (const auto& e : out)
          known.insert(e.slot);
      }
      ::_exit(0);
    }
    kids.push_back(pid);
  }
  for (pid_t k : kids) {
    int status = 0;
    ASSERT_EQ(::waitpid(k, &status, 0), k);
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 0);
  }
  auto s = SlotStore::open(io, t.path(), kHash);
  ASSERT_TRUE(s);
  std::map<uint32_t, SlotEntry> last;
  std::map<uint32_t, int> count;
  for (const auto& e : s->refresh(true)) {
    last[e.slot] = e;
    ++count[e.slot];
  }
  EXPECT_EQ(last.size(), static_cast<size_t>(kSlots));
  for (const auto& [slot, n] : count)
    EXPECT_EQ(n, 1) << "slot " << slot << " committed more than once";
  for (const auto& [slot, e] : last) {
    std::vector<uint8_t> got;
    ASSERT_TRUE(s->readRecord(e, got)) << "slot " << slot;
    EXPECT_EQ(got, rec(slot, 2000 + slot * 10, static_cast<uint8_t>(slot)).bytes);
  }
}
