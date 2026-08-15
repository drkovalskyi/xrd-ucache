// Injectable filesystem backend: everything the core does to disk
// goes through this interface so tests can inject ENOSPC/EIO/short/torn
// writes and prove every path fail-opens.
//
// Error convention: methods return >= 0 on success; negative -errno on
// failure (never throw — no exceptions cross the plugin ABI later, §5.3).
//
// Thread-safety: RealIO is stateless and fully thread-safe. FaultIO (in
// tests) is thread-safe via an internal mutex.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace ucache {

// Nanosecond timestamps out of a stat buffer. POSIX 2008 named these fields
// st_mtim/st_atim; Darwin had them first under the names st_mtimespec and
// st_atimespec and kept those. Same data, two spellings, so the choice is made
// once here rather than at each use — a compile error on the second platform is
// the cheapest possible outcome of getting this wrong, and it is still one
// round trip per site.
inline const struct timespec& statMtime(const struct ::stat& st) {
#if defined(__APPLE__)
  return st.st_mtimespec;
#else
  return st.st_mtim;
#endif
}
inline const struct timespec& statAtime(const struct ::stat& st) {
#if defined(__APPLE__)
  return st.st_atimespec;
#else
  return st.st_atim;
#endif
}

// Upper bound on a single coalesced cache read. Read granularity and
// checksum granularity are independent: a reader may pull a contiguous run
// of pages with one pread and verify each page's crc32c out of that buffer.
// The cap bounds the transient buffer (and the latency of one syscall) while
// still amortizing the call over hundreds of pages.
constexpr uint64_t kMaxCoalescedRead = 1ull << 20; // 1 MiB

class IOBackend {
 public:
  virtual ~IOBackend() = default;

  // Returns fd >= 0 or -errno.
  virtual int open(const std::string& path, int flags, mode_t mode) = 0;
  virtual int close(int fd) = 0;
  virtual int64_t pread(int fd, void* buf, uint64_t count, uint64_t offset) = 0;
  virtual int64_t pwrite(int fd, const void* buf, uint64_t count, uint64_t offset) = 0;
  virtual int fdatasync(int fd) = 0;
  virtual int ftruncate(int fd, uint64_t length) = 0;
  virtual int fstat(int fd, struct ::stat* st) = 0;
  virtual int stat(const std::string& path, struct ::stat* st) = 0;
  virtual int unlink(const std::string& path) = 0;
  virtual int rename(const std::string& from, const std::string& to) = 0;
  virtual int mkdirs(const std::string& path, mode_t mode) = 0; // mkdir -p
  virtual int flock(int fd, int op) = 0;                        // LOCK_SH/EX/UN
  // Plain entries of a directory (no "."/".."); -errno if unreadable.
  virtual int listDir(const std::string& path, std::vector<std::string>& names) = 0;
  // Free/total bytes of the filesystem holding `path` (statvfs): availBytes is
  // space usable by this (non-root) user, totalBytes the volume capacity.
  // 0 or -errno; used by the eviction disk-space floor.
  virtual int spaceInfo(const std::string& path, uint64_t& availBytes,
                        uint64_t& totalBytes) = 0;
  // Deallocate [offset, offset+length) keeping the file size (sparse hole).
  // 0 or -errno; -EOPNOTSUPP on filesystems without punch support — callers
  // treat that as best-effort space reclaim, never a correctness failure
  // (replica punch-and-clear).
  virtual int punchHole(int fd, uint64_t offset, uint64_t length) = 0;

  // Complete pwrite loop: retries short writes; negative -errno on failure.
  int64_t pwriteFull(int fd, const void* buf, uint64_t count, uint64_t offset);
  // Complete pread loop; returns bytes read (may be < count only at EOF).
  int64_t preadFull(int fd, void* buf, uint64_t count, uint64_t offset);
};

// POSIX passthrough.
class RealIO : public IOBackend {
 public:
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

  static IOBackend& instance();
};

} // namespace ucache
