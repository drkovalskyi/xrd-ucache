// Minimal leveled logger to stderr or a file (UCACHE_LOG).
// The cache must never break a job — logging failures are swallowed.
//
// Thread-safety: fully thread-safe; each message is one write() call.
#pragma once

#include <cstdarg>
#include <string>

namespace ucache {

enum class LogLevel { kError = 0, kWarn = 1, kInfo = 2, kDebug = 3 };

class Log {
 public:
  // "error"/"warn"/"info"/"debug", optionally "level:/path/to/file".
  static void configure(const std::string& spec);
  static bool enabled(LogLevel lvl);
  static void write(LogLevel lvl, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
};

#define UCACHE_LOG(lvl, ...)                                                                       \
  do {                                                                                             \
    if (::ucache::Log::enabled(lvl))                                                               \
      ::ucache::Log::write(lvl, __VA_ARGS__);                                                      \
  } while (0)
#define UCACHE_ERROR(...) UCACHE_LOG(::ucache::LogLevel::kError, __VA_ARGS__)
#define UCACHE_WARN(...) UCACHE_LOG(::ucache::LogLevel::kWarn, __VA_ARGS__)
#define UCACHE_INFO(...) UCACHE_LOG(::ucache::LogLevel::kInfo, __VA_ARGS__)
#define UCACHE_DEBUG(...) UCACHE_LOG(::ucache::LogLevel::kDebug, __VA_ARGS__)

} // namespace ucache
