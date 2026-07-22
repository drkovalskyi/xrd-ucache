#include "OpenRetry.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <random>
#include <thread>

namespace ucache {
namespace {

// XRootD 5.8.3 status `code` values (XrdCl/XrdClStatus.hh). Hardcoded to keep
// this TU XrdCl-free; UCacheFile.cc static_asserts them against the headers.
constexpr int kErrSocketError = 102;
constexpr int kErrSocketTimeout = 103;
constexpr int kErrConnectionError = 108;
constexpr int kErrOperationExpired = 206;
constexpr int kErrThresholdExceeded = 208; // NB: not "errThreshold" (no such name)
constexpr int kErrErrorResponse = 400;     // server sent kXR_error; errNo holds the wire code

// XRootD server error wire codes (XProtocol/XProtocol.hh, base kXR_ArgInvalid
// = 3000). A server error arrives in XRootDStatus.errNo UNMAPPED (verified
// from the XRootD source), so we compare against these raw values.
constexpr int kXR_FileLocked = 3003;
constexpr int kXR_FSError = 3005;
constexpr int kXR_IOError = 3007;
constexpr int kXR_ServerError = 3012;
constexpr int kXR_noserver = 3014;
constexpr int kXR_Cancelled = 3017;
constexpr int kXR_ItExists = 3018;   // the observed MIT transient failure
constexpr int kXR_Overloaded = 3024; // canonical load-induced refusal

// Seed a per-thread RNG. steady_clock ns mixed with the thread-id hash so two
// threads seeding in the same tick draw independent sequences (decorrelation
// is the whole point of the jitter).
uint64_t makeSeed() {
  uint64_t ns = static_cast<uint64_t>(
      std::chrono::steady_clock::now().time_since_epoch().count());
  uint64_t tid = static_cast<uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
  return ns ^ (tid << 1) ^ (tid >> 31);
}

} // namespace

bool isRetryableOpen(int statusCode, int errNo) {
  switch (statusCode) {
    // Transient transport failures — retry regardless of errNo.
    case kErrConnectionError:
    case kErrSocketError:
    case kErrSocketTimeout:
    case kErrOperationExpired:
    case kErrThresholdExceeded:
      return true;
    // The server sent an error: retry only the transient / load-induced subset.
    case kErrErrorResponse:
      switch (errNo) {
        case kXR_ItExists:
        case kXR_ServerError:
        case kXR_FSError:
        case kXR_IOError:
        case kXR_noserver:
        case kXR_Cancelled:
        case kXR_FileLocked:
        case kXR_Overloaded:
          return true;
        default:
          // NotFound / NotAuthorized / bad-args / NoSpace / ... : fail fast.
          return false;
      }
    default:
      return false;
  }
}

uint64_t backoffBoundMs(int attempt, const Config& cfg) {
  const uint64_t base = cfg.openRetryBaseMs > 0 ? static_cast<uint64_t>(cfg.openRetryBaseMs) : 0;
  const uint64_t cap = cfg.openRetryMaxMs > 0 ? static_cast<uint64_t>(cfg.openRetryMaxMs) : 0;
  if (attempt < 1)
    attempt = 1;
  uint64_t bound = base;
  // Double per attempt, stopping at the cap. Breaking once bound >= cap keeps
  // the doubling from overflowing (cap is a modest ms value).
  for (int i = 1; i < attempt; ++i) {
    if (bound >= cap)
      break;
    bound <<= 1;
  }
  return std::min(bound, cap);
}

uint64_t backoffMs(int attempt, const Config& cfg) {
  const uint64_t bound = backoffBoundMs(attempt, cfg);
  if (bound == 0)
    return 0;
  thread_local std::mt19937 rng(static_cast<std::mt19937::result_type>(makeSeed()));
  std::uniform_int_distribution<uint64_t> dist(0, bound);
  return dist(rng);
}

} // namespace ucache
