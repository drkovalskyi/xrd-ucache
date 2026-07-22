#include "IOBackend.h"

#include "TestUtil.h"
#include "testing/FaultIO.h"

#include <cerrno>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/file.h>

using namespace ucache;
using test::TempDir;

TEST(RealIO, BasicRoundtrip) {
  TempDir td;
  RealIO io;
  std::string f = td.path() + "/sub/dir/file.bin";
  ASSERT_EQ(io.mkdirs(td.path() + "/sub/dir", 0700), 0);
  int fd = io.open(f, O_RDWR | O_CREAT, 0600);
  ASSERT_GE(fd, 0);
  auto data = test::randomBytes(10000, 7);
  EXPECT_EQ(io.pwriteFull(fd, data.data(), data.size(), 100), 10000);
  std::vector<uint8_t> back(10000);
  EXPECT_EQ(io.preadFull(fd, back.data(), back.size(), 100), 10000);
  EXPECT_EQ(back, data);
  // Short read at EOF.
  EXPECT_EQ(io.preadFull(fd, back.data(), 10000, 10000), 100);
  struct ::stat st;
  EXPECT_EQ(io.fstat(fd, &st), 0);
  EXPECT_EQ(st.st_size, 10100);
  EXPECT_EQ(io.ftruncate(fd, 50), 0);
  EXPECT_EQ(io.fstat(fd, &st), 0);
  EXPECT_EQ(st.st_size, 50);
  EXPECT_EQ(io.flock(fd, LOCK_EX), 0);
  EXPECT_EQ(io.flock(fd, LOCK_UN), 0);
  EXPECT_EQ(io.close(fd), 0);

  std::string f2 = td.path() + "/sub/dir/file2.bin";
  EXPECT_EQ(io.rename(f, f2), 0);
  EXPECT_EQ(io.stat(f2, &st), 0);
  EXPECT_LT(io.stat(f, &st), 0);
  std::vector<std::string> names;
  EXPECT_EQ(io.listDir(td.path() + "/sub/dir", names), 0);
  EXPECT_EQ(names, (std::vector<std::string>{"file2.bin"}));
  EXPECT_EQ(io.unlink(f2), 0);
  EXPECT_EQ(io.unlink(f2), -ENOENT);
  EXPECT_EQ(io.listDir(td.path() + "/nope", names), -ENOENT);
  EXPECT_LT(io.open(td.path() + "/nope/x", O_RDONLY, 0), 0);
}

TEST(FaultIO, FailNthAndCounts) {
  TempDir td;
  RealIO real;
  FaultIO io(real);
  std::string f = td.path() + "/f";
  int fd = io.open(f, O_RDWR | O_CREAT, 0600);
  ASSERT_GE(fd, 0);
  io.failNth(IoOp::kPwrite, 2, ENOSPC);
  char b[4] = "abc";
  EXPECT_EQ(io.pwrite(fd, b, 3, 0), 3);       // 1st ok
  EXPECT_EQ(io.pwrite(fd, b, 3, 3), -ENOSPC); // 2nd fails
  EXPECT_EQ(io.pwrite(fd, b, 3, 3), 3);       // rule consumed
  EXPECT_EQ(io.calls(IoOp::kPwrite), 3);
  io.close(fd);
}

TEST(FaultIO, ShortWriteRetriedByPwriteFull) {
  TempDir td;
  RealIO real;
  FaultIO io(real);
  int fd = io.open(td.path() + "/f", O_RDWR | O_CREAT, 0600);
  ASSERT_GE(fd, 0);
  auto data = test::randomBytes(8192, 3);
  io.shortWriteNth(1, 100); // first pwrite delivers only 100 bytes
  EXPECT_EQ(io.pwriteFull(fd, data.data(), data.size(), 0), 8192);
  std::vector<uint8_t> back(8192);
  EXPECT_EQ(io.preadFull(fd, back.data(), 8192, 0), 8192);
  EXPECT_EQ(back, data);
  io.close(fd);
}

TEST(FaultIO, DieAtBlocksMutationsAllowsReads) {
  TempDir td;
  RealIO real;
  FaultIO io(real);
  int fd = io.open(td.path() + "/f", O_RDWR | O_CREAT, 0600);
  ASSERT_GE(fd, 0);
  char b[4] = "xyz";
  ASSERT_EQ(io.pwriteFull(fd, b, 3, 0), 3);
  io.dieAt(IoOp::kPwrite, 1, EIO);
  EXPECT_EQ(io.pwrite(fd, b, 3, 3), -EIO);   // trips
  EXPECT_EQ(io.pwrite(fd, b, 3, 6), -EIO);   // stays dead
  EXPECT_EQ(io.rename(td.path() + "/f", td.path() + "/g"), -EIO);
  EXPECT_EQ(io.unlink(td.path() + "/f"), -EIO);
  char r[4] = {0};
  EXPECT_EQ(io.pread(fd, r, 3, 0), 3); // reads survive
  io.reset();
  EXPECT_EQ(io.pwrite(fd, b, 3, 3), 3);
  io.close(fd);
}
