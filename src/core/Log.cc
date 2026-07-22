#include "Log.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <unistd.h>

namespace ucache {
namespace {
std::atomic<int> gLevel{static_cast<int>(LogLevel::kWarn)};
std::atomic<int> gFd{STDERR_FILENO};
} // namespace

void Log::configure(const std::string& spec) {
  std::string level = spec, path;
  auto colon = spec.find(':');
  if (colon != std::string::npos) {
    level = spec.substr(0, colon);
    path = spec.substr(colon + 1);
  }
  if (level == "error")
    gLevel = 0;
  else if (level == "warn")
    gLevel = 1;
  else if (level == "info")
    gLevel = 2;
  else if (level == "debug")
    gLevel = 3;
  if (!path.empty()) {
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (fd >= 0)
      gFd = fd; // old fd (stderr) is not closed; leaked fds on reconfigure are fine
  }
}

bool Log::enabled(LogLevel lvl) { return static_cast<int>(lvl) <= gLevel.load(); }

void Log::write(LogLevel lvl, const char* fmt, ...) {
  static const char* names[] = {"ERROR", "WARN", "INFO", "DEBUG"};
  char buf[1024];
  time_t now = ::time(nullptr);
  struct tm tmv;
  ::localtime_r(&now, &tmv);
  int n = ::snprintf(buf, sizeof buf, "[%02d:%02d:%02d][ucache %s] ", tmv.tm_hour, tmv.tm_min,
                     tmv.tm_sec, names[static_cast<int>(lvl)]);
  va_list ap;
  va_start(ap, fmt);
  n += ::vsnprintf(buf + n, sizeof buf - n - 2, fmt, ap);
  va_end(ap);
  if (n > static_cast<int>(sizeof buf) - 2)
    n = sizeof buf - 2;
  buf[n++] = '\n';
  // Single write per message; failure is deliberately ignored (fail-open).
  [[maybe_unused]] ssize_t r = ::write(gFd.load(), buf, n);
}

} // namespace ucache
