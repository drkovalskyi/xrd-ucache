// Locating our own CLI, the way posix_spawnp will.
//
// The plugin spawns `ucache recompress --drain` to build replicas in the
// background. posix_spawnp reports success as soon as the fork succeeds — an
// exec failure surfaces only in the child, which nothing waits for, because the
// worker is detached by design. An unresolvable helper therefore produced
// perfect silence: files queued at every close and no replica ever built.
//
// So both sides ask this question up front and say so: the plugin warns once
// and stops queueing work nothing can drain, and `doctor`/`status` name it as
// the reason background recompression is producing nothing.
#ifndef UCACHE_CORE_HELPERPATH_H
#define UCACHE_CORE_HELPERPATH_H

#include <cstdlib>
#include <string>
#include <sys/stat.h> // stat/S_ISREG below; glibc supplies it transitively, other libcs do not
#include <unistd.h>

namespace ucache {

// True when the drainer helper can be executed. `exeOut` always receives what
// was looked for, resolvable or not, so callers can name it in a message.
// UCACHE_RECOMPRESS_HELPER (an explicit path, used by tests and by installs
// that keep the CLI outside PATH) must itself be executable; otherwise a bare
// `ucache` is searched along PATH.
// An executable REGULAR file: `access(X_OK)` alone says yes to a directory
// named `ucache` that happens to be searchable, and a spawn on that fails with
// EACCES — back to the silence this is here to prevent.
inline bool isExecutableFile(const std::string& path) {
  struct ::stat st;
  return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) && ::access(path.c_str(), X_OK) == 0;
}

inline bool recompressHelperResolvable(std::string& exeOut) {
  if (const char* helper = ::getenv("UCACHE_RECOMPRESS_HELPER"); helper && *helper) {
    exeOut = helper;
    return isExecutableFile(helper);
  }
  exeOut = "ucache";
  // Mirror execvp's search, or the answer can differ from what the spawn will
  // actually do — and since this gate SUPPRESSES the spawn, a divergence turns a
  // working setup into a silently disabled one. Two rules are easy to miss:
  // an unset PATH means the confstr default (usually /bin:/usr/bin), NOT "no
  // search"; and an empty element (leading/trailing/doubled ':') means the
  // current directory.
  std::string p;
  if (const char* env = ::getenv("PATH"); env && *env) {
    p = env;
  } else {
    const size_t n = ::confstr(_CS_PATH, nullptr, 0);
    if (n) {
      std::string buf(n, '\0');
      ::confstr(_CS_PATH, buf.data(), n);
      buf.resize(n ? n - 1 : 0); // drop the trailing NUL
      p = buf;
    }
    if (p.empty())
      p = "/bin:/usr/bin";
  }
  for (size_t at = 0;; ) {
    const size_t colon = p.find(':', at);
    std::string dir = p.substr(at, colon == std::string::npos ? std::string::npos : colon - at);
    if (dir.empty())
      dir = "."; // POSIX: an empty PATH element is the current directory
    if (isExecutableFile(dir + "/ucache"))
      return true;
    if (colon == std::string::npos)
      break;
    at = colon + 1;
  }
  return false;
}

} // namespace ucache

#endif // UCACHE_CORE_HELPERPATH_H
