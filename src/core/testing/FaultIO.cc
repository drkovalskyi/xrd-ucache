#include "FaultIO.h"

#include <algorithm>
#include <cerrno>
#include <sys/file.h> // flock: reached transitively on glibc, declared here everywhere

namespace ucache {

void FaultIO::failNth(IoOp op, int nth, int err) {
  std::lock_guard<std::mutex> g(mu_);
  rules_.push_back({op, counts_[static_cast<int>(op)] + nth, err, false});
}

void FaultIO::shortWriteNth(int nth, uint64_t bytes) {
  std::lock_guard<std::mutex> g(mu_);
  shortNth_ = shortCounter_ + nth;
  shortBytes_ = bytes;
}

void FaultIO::dieAt(IoOp op, int nth, int err) {
  std::lock_guard<std::mutex> g(mu_);
  rules_.push_back({op, counts_[static_cast<int>(op)] + nth, err, true});
}

void FaultIO::forceAvail(uint64_t availBytes, uint64_t totalBytes) {
  std::lock_guard<std::mutex> g(mu_);
  forcedAvail_ = availBytes;
  forcedTotal_ = totalBytes;
}

void FaultIO::failSpaceInfo(bool on) {
  std::lock_guard<std::mutex> g(mu_);
  failSpace_ = on;
}

void FaultIO::reset() {
  std::lock_guard<std::mutex> g(mu_);
  rules_.clear();
  std::fill(std::begin(counts_), std::end(counts_), 0);
  shortNth_ = 0;
  shortCounter_ = 0;
  dead_ = false;
  forcedAvail_ = 0;
  forcedTotal_ = 0;
  failSpace_ = false;
}

int FaultIO::calls(IoOp op) const {
  std::lock_guard<std::mutex> g(mu_);
  return counts_[static_cast<int>(op)];
}

int FaultIO::check(IoOp op) {
  std::lock_guard<std::mutex> g(mu_);
  int idx = static_cast<int>(op);
  ++counts_[idx];
  if (dead_) {
    // Post-"crash": reads still work (page cache survives a process crash;
    // what we simulate is the writer dying), mutations fail.
    bool mutating = op == IoOp::kPwrite || op == IoOp::kFdatasync || op == IoOp::kFtruncate ||
                    op == IoOp::kUnlink || op == IoOp::kRename || op == IoOp::kMkdirs ||
                    op == IoOp::kPunchHole;
    if (mutating)
      return -deadErr_;
  }
  for (auto it = rules_.begin(); it != rules_.end(); ++it) {
    if (it->op == op && counts_[idx] >= it->nth) {
      int err = it->err;
      if (it->die) {
        dead_ = true;
        deadErr_ = err;
      } else {
        rules_.erase(it);
      }
      return -err;
    }
  }
  return 0;
}

int FaultIO::open(const std::string& path, int flags, mode_t mode) {
  if (int e = check(IoOp::kOpen))
    return e;
  return inner_.open(path, flags, mode);
}
int FaultIO::close(int fd) {
  if (int e = check(IoOp::kClose))
    return e;
  return inner_.close(fd);
}
int64_t FaultIO::pread(int fd, void* buf, uint64_t count, uint64_t offset) {
  if (int e = check(IoOp::kPread))
    return e;
  return inner_.pread(fd, buf, count, offset);
}
int64_t FaultIO::pwrite(int fd, const void* buf, uint64_t count, uint64_t offset) {
  if (int e = check(IoOp::kPwrite))
    return e;
  {
    std::lock_guard<std::mutex> g(mu_);
    ++shortCounter_;
    if (shortNth_ != 0 && shortCounter_ == shortNth_ && shortBytes_ < count) {
      shortNth_ = 0;
      count = shortBytes_; // deliver a short write
    }
  }
  return inner_.pwrite(fd, buf, count, offset);
}
int FaultIO::fdatasync(int fd) {
  if (int e = check(IoOp::kFdatasync))
    return e;
  return inner_.fdatasync(fd);
}
int FaultIO::ftruncate(int fd, uint64_t length) {
  if (int e = check(IoOp::kFtruncate))
    return e;
  return inner_.ftruncate(fd, length);
}
int FaultIO::fstat(int fd, struct ::stat* st) {
  if (int e = check(IoOp::kFstat))
    return e;
  return inner_.fstat(fd, st);
}
int FaultIO::stat(const std::string& path, struct ::stat* st) {
  if (int e = check(IoOp::kStat))
    return e;
  return inner_.stat(path, st);
}
int FaultIO::unlink(const std::string& path) {
  if (int e = check(IoOp::kUnlink))
    return e;
  return inner_.unlink(path);
}
int FaultIO::rename(const std::string& from, const std::string& to) {
  if (int e = check(IoOp::kRename))
    return e;
  return inner_.rename(from, to);
}
int FaultIO::mkdirs(const std::string& path, mode_t mode) {
  if (int e = check(IoOp::kMkdirs))
    return e;
  return inner_.mkdirs(path, mode);
}
int FaultIO::flock(int fd, int op) {
  if (int e = check(IoOp::kFlock))
    return e;
  return inner_.flock(fd, op);
}
int FaultIO::listDir(const std::string& path, std::vector<std::string>& names) {
  if (int e = check(IoOp::kListDir))
    return e;
  return inner_.listDir(path, names);
}
int FaultIO::punchHole(int fd, uint64_t offset, uint64_t length) {
  if (int e = check(IoOp::kPunchHole))
    return e;
  return inner_.punchHole(fd, offset, length);
}
int FaultIO::spaceInfo(const std::string& path, uint64_t& availBytes, uint64_t& totalBytes) {
  {
    std::lock_guard<std::mutex> g(mu_);
    if (failSpace_)
      return -EIO;
    if (forcedAvail_) {
      availBytes = forcedAvail_;
      totalBytes = forcedTotal_ ? forcedTotal_ : forcedAvail_;
      return 0;
    }
  }
  return inner_.spaceInfo(path, availBytes, totalBytes);
}

} // namespace ucache
