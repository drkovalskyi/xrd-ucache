#include "IOBackend.h"

#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <linux/falloc.h>
#include <sys/file.h>
#include <sys/statvfs.h>
#include <unistd.h>

namespace ucache {

int64_t IOBackend::pwriteFull(int fd, const void* buf, uint64_t count, uint64_t offset) {
  const auto* p = static_cast<const uint8_t*>(buf);
  uint64_t done = 0;
  while (done < count) {
    int64_t n = pwrite(fd, p + done, count - done, offset + done);
    if (n < 0) {
      if (n == -EINTR)
        continue;
      return n;
    }
    if (n == 0)
      return -EIO; // no forward progress
    done += static_cast<uint64_t>(n);
  }
  return static_cast<int64_t>(done);
}

int64_t IOBackend::preadFull(int fd, void* buf, uint64_t count, uint64_t offset) {
  auto* p = static_cast<uint8_t*>(buf);
  uint64_t done = 0;
  while (done < count) {
    int64_t n = pread(fd, p + done, count - done, offset + done);
    if (n < 0) {
      if (n == -EINTR)
        continue;
      return n;
    }
    if (n == 0)
      break; // EOF
    done += static_cast<uint64_t>(n);
  }
  return static_cast<int64_t>(done);
}

namespace {
inline int retErrno(int r) { return r < 0 ? -errno : r; }
inline int64_t retErrno64(int64_t r) { return r < 0 ? -errno : r; }
} // namespace

int RealIO::open(const std::string& path, int flags, mode_t mode) {
  return retErrno(::open(path.c_str(), flags | O_CLOEXEC, mode));
}
int RealIO::close(int fd) { return retErrno(::close(fd)); }
int64_t RealIO::pread(int fd, void* buf, uint64_t count, uint64_t offset) {
  return retErrno64(::pread(fd, buf, count, offset));
}
int64_t RealIO::pwrite(int fd, const void* buf, uint64_t count, uint64_t offset) {
  return retErrno64(::pwrite(fd, buf, count, offset));
}
int RealIO::fdatasync(int fd) { return retErrno(::fdatasync(fd)); }
int RealIO::ftruncate(int fd, uint64_t length) { return retErrno(::ftruncate(fd, length)); }
int RealIO::fstat(int fd, struct ::stat* st) { return retErrno(::fstat(fd, st)); }
int RealIO::stat(const std::string& path, struct ::stat* st) {
  return retErrno(::stat(path.c_str(), st));
}
int RealIO::unlink(const std::string& path) { return retErrno(::unlink(path.c_str())); }
int RealIO::rename(const std::string& from, const std::string& to) {
  return retErrno(::rename(from.c_str(), to.c_str()));
}
int RealIO::mkdirs(const std::string& path, mode_t mode) {
  std::string cur;
  for (size_t i = 0; i < path.size(); ++i) {
    cur += path[i];
    if ((path[i] == '/' && i > 0) || i == path.size() - 1) {
      if (::mkdir(cur.c_str(), mode) < 0 && errno != EEXIST)
        return -errno;
    }
  }
  return 0;
}
int RealIO::flock(int fd, int op) {
  int r;
  do {
    r = ::flock(fd, op);
  } while (r < 0 && errno == EINTR);
  return retErrno(r);
}
int RealIO::listDir(const std::string& path, std::vector<std::string>& names) {
  DIR* d = ::opendir(path.c_str());
  if (!d)
    return -errno;
  while (struct dirent* e = ::readdir(d)) {
    if (e->d_name[0] == '.' &&
        (e->d_name[1] == 0 || (e->d_name[1] == '.' && e->d_name[2] == 0)))
      continue;
    names.emplace_back(e->d_name);
  }
  ::closedir(d);
  return 0;
}
int RealIO::spaceInfo(const std::string& path, uint64_t& availBytes, uint64_t& totalBytes) {
  struct ::statvfs vfs;
  if (::statvfs(path.c_str(), &vfs) < 0)
    return -errno;
  availBytes = static_cast<uint64_t>(vfs.f_bavail) * vfs.f_frsize;
  totalBytes = static_cast<uint64_t>(vfs.f_blocks) * vfs.f_frsize;
  return 0;
}
int RealIO::punchHole(int fd, uint64_t offset, uint64_t length) {
  if (length == 0)
    return 0;
  int r;
  do {
    r = ::fallocate(fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
                    static_cast<off_t>(offset), static_cast<off_t>(length));
  } while (r < 0 && errno == EINTR);
  return retErrno(r);
}

IOBackend& RealIO::instance() {
  // Leaked deliberately: detached executor threads (plugin) may still issue
  // IO during static destruction; a destroyed singleton is a vptr race
  // (found by TSan). RealIO is stateless — leaking it costs nothing.
  static RealIO* io = new RealIO();
  return *io;
}

} // namespace ucache
