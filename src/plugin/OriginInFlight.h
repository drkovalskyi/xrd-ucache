#pragma once
#include "Stats.h"

#include <atomic>
#include <cstdint>

namespace ucache {

// One read outstanding at the ORIGIN, for as long as this object lives. It
// goes INSIDE the handler that owns a wire read, because that handler is
// constructed at the issue and destroyed exactly once -- on completion or on a
// failed issue -- which is the same span the origin sees. Counting on entry to
// Read and releasing on return measured the handoff instead: every route here
// dispatches and returns while the caller waits above this library, so 32
// threads with a read each reported a handful.
struct OriginInFlight {
  Stats* s = nullptr;
  OriginInFlight() = default;
  explicit OriginInFlight(Stats* st) : s(st) {
    if (!s)
      return;
    const uint64_t n = s->originReadsInFlight.fetch_add(1, std::memory_order_relaxed) + 1;
    uint64_t hw = s->originReadsInFlightHighWater.load(std::memory_order_relaxed);
    while (n > hw && !s->originReadsInFlightHighWater.compare_exchange_weak(
                         hw, n, std::memory_order_relaxed))
      ;
  }
  OriginInFlight(const OriginInFlight&) = delete;
  OriginInFlight& operator=(const OriginInFlight&) = delete;
  OriginInFlight(OriginInFlight&& o) noexcept : s(o.s) { o.s = nullptr; }
  OriginInFlight& operator=(OriginInFlight&& o) noexcept {
    if (this != &o) {
      if (s)
        s->originReadsInFlight.fetch_sub(1, std::memory_order_relaxed);
      s = o.s;
      o.s = nullptr;
    }
    return *this;
  }
  // Give the read back as soon as it has landed. Waiting for the destructor
  // means holding it across the caller's own completion handler, and a caller
  // that issues its next read from in there -- which is the shape that creates
  // real concurrency -- takes the next guard before this one is released. The
  // count would then be an artefact of when handlers happen to be destroyed
  // rather than a measure of what the origin was being asked for.
  void release() {
    if (s)
      s->originReadsInFlight.fetch_sub(1, std::memory_order_relaxed);
    s = nullptr;
  }
  ~OriginInFlight() { release(); }
};

} // namespace ucache
