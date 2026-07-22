// Fault-injecting IOBackend wrapper: deterministic
// programmable failures — ENOSPC, EIO, short writes, and "torn write then
// crash" schedules — layered over any inner backend (usually RealIO on a
// temp dir). Used by the fault-injection and crash suites to prove every
// core path fail-opens with no crash and no bad data.
//
// Thread-safety: thread-safe via internal mutex.
#pragma once

#include "../IOBackend.h"

#include <functional>
#include <mutex>
#include <vector>

namespace ucache {

enum class IoOp { kOpen, kClose, kPread, kPwrite, kFdatasync, kFtruncate, kFstat, kStat,
                  kUnlink, kRename, kMkdirs, kFlock, kListDir, kPunchHole };

class FaultIO : public IOBackend {
 public:
  explicit FaultIO(IOBackend& inner) : inner_(inner) {}

  // The n-th future call (1-based) of `op` fails with -err.
  void failNth(IoOp op, int nth, int err);
  // The n-th future pwrite writes only `bytes` bytes (short write).
  void shortWriteNth(int nth, uint64_t bytes);
  // From the n-th future call of `op` (inclusive), that op and all mutating
  // ops fail with -err — simulates a dying filesystem / post-crash state.
  void dieAt(IoOp op, int nth, int err);
  // Make spaceInfo report a fixed available byte count (0 = pass through to
  // inner). Drives the eviction disk-space-floor tests deterministically.
  void forceAvail(uint64_t availBytes, uint64_t totalBytes = 0);
  // Make spaceInfo fail with -EIO (fail-open path when statvfs errors).
  void failSpaceInfo(bool on);
  // Clear all schedules and counters.
  void reset();
  // Count of calls seen per op (for schedule construction in tests).
  int calls(IoOp op) const;

  int open(const std::string& path, int flags, mode_t mode) override;
  int close(int fd) override;
  int64_t pread(int fd, void* buf, uint64_t count, uint64_t offset) override;
  int64_t pwrite(int fd, const void* buf, uint64_t count, uint64_t offset) override;
  int fdatasync(int fd) override;
  int ftruncate(int fd, uint64_t length) override;
  int fstat(int fd, struct ::stat* st) override;
  int stat(const std::string& path, struct ::stat* st) override;
  int unlink(const std::string& path) override;
  int rename(const std::string& from, const std::string& to) override;
  int mkdirs(const std::string& path, mode_t mode) override;
  int flock(int fd, int op) override;
  int listDir(const std::string& path, std::vector<std::string>& names) override;
  int spaceInfo(const std::string& path, uint64_t& availBytes, uint64_t& totalBytes) override;
  int punchHole(int fd, uint64_t offset, uint64_t length) override;

 private:
  struct Rule {
    IoOp op;
    int nth; // triggers when per-op counter reaches this value
    int err;
    bool die = false;
  };
  // Returns 0 = proceed, else -err to return. Increments counters.
  int check(IoOp op);

  IOBackend& inner_;
  mutable std::mutex mu_;
  std::vector<Rule> rules_;
  int counts_[14] = {};
  int shortNth_ = 0;
  uint64_t shortBytes_ = 0;
  int shortCounter_ = 0;
  bool dead_ = false;
  int deadErr_ = 0;
  uint64_t forcedAvail_ = 0;
  uint64_t forcedTotal_ = 0;
  bool failSpace_ = false;
};

} // namespace ucache
