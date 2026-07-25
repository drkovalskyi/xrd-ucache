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
#include <unistd.h>

namespace ucache {

// True when the drainer helper can be executed. `exeOut` always receives what
// was looked for, resolvable or not, so callers can name it in a message.
// UCACHE_RECOMPRESS_HELPER (an explicit path, used by tests and by installs
// that keep the CLI outside PATH) must itself be executable; otherwise a bare
// `ucache` is searched along PATH.
inline bool recompressHelperResolvable(std::string& exeOut) {
  if (const char* helper = ::getenv("UCACHE_RECOMPRESS_HELPER"); helper && *helper) {
    exeOut = helper;
    return ::access(helper, X_OK) == 0;
  }
  exeOut = "ucache";
  const char* path = ::getenv("PATH");
  if (!path || !*path)
    return false;
  const std::string p(path);
  for (size_t at = 0; at <= p.size();) {
    const size_t colon = p.find(':', at);
    const std::string dir =
        p.substr(at, colon == std::string::npos ? std::string::npos : colon - at);
    if (!dir.empty() && ::access((dir + "/ucache").c_str(), X_OK) == 0)
      return true;
    if (colon == std::string::npos)
      break;
    at = colon + 1;
  }
  return false;
}

} // namespace ucache

#endif // UCACHE_CORE_HELPERPATH_H
