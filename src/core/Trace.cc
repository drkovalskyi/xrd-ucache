#include "Trace.h"

#include "IOBackend.h"
#include <chrono>
#include <cstdio>
#include <fcntl.h>
#include <functional>
#include <utility>

namespace ucache {
namespace {

uint64_t wallUs() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                   std::chrono::system_clock::now().time_since_epoch())
                                   .count());
}

// Minimal JSON string escape for URLs (quote/backslash/control bytes).
std::string jsonEscape(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (static_cast<unsigned char>(c) < 0x20) {
      char b[8];
      std::snprintf(b, sizeof b, "\\u%04x", c);
      out += b;
    } else {
      out += c;
    }
  }
  return out;
}

} // namespace

Tracer::Tracer(IOBackend& io, std::string path, int sample)
    : io_(io), path_(std::move(path)), sample_(sample < 1 ? 1 : sample) {}

Tracer::~Tracer() {
  if (fd_ >= 0)
    io_.close(fd_);
}

uint32_t Tracer::slot() {
  // One counter per tracer, one cached id per thread. A process with a single
  // tracer (the normal case) therefore numbers its workers 0..N-1 in the order
  // they first do IO, which is what a replay wants to group by.
  static thread_local const Tracer* owner = nullptr;
  static thread_local uint32_t cached = 0;
  if (owner != this) {
    owner = this;
    cached = nextSlot_.fetch_add(1, std::memory_order_relaxed);
  }
  return cached;
}

void Tracer::rec(const char* op, const std::string& key, uint64_t off, uint64_t len,
                 uint64_t us, bool sampled) {
  if (sampled && sample_ > 1 &&
      n_.fetch_add(1, std::memory_order_relaxed) % static_cast<uint64_t>(sample_) != 0)
    return;
  const uint64_t h = std::hash<std::string>{}(key);
  char line[192];
  // `w` is this thread's worker slot. Recorded on every line because a replay
  // reconstructs per-worker timelines, and the gap between one worker's
  // completion and its next issue is that worker's compute.
  int n = std::snprintf(line, sizeof line,
                        "{\"t\":%llu,\"w\":%u,\"op\":\"%s\",\"k\":\"%016llx\","
                        "\"off\":%llu,\"len\":%llu,\"us\":%llu}\n",
                        static_cast<unsigned long long>(wallUs()), slot(), op,
                        static_cast<unsigned long long>(h),
                        static_cast<unsigned long long>(off),
                        static_cast<unsigned long long>(len),
                        static_cast<unsigned long long>(us));
  if (n <= 0 || n >= static_cast<int>(sizeof line))
    return;

  std::lock_guard<std::mutex> g(mu_);
  if (fd_ < 0) {
    fd_ = io_.open(path_, O_WRONLY | O_CREAT, 0600);
    if (fd_ < 0)
      return; // trace loss is acceptable; job health is not
    // Announce the sampling rate FIRST. Without it a consumer cannot tell a
    // complete trace from a 1-in-64 one, and a timeline reconstructed from a
    // sampled trace is nonsense that looks plausible: the 63 missing
    // operations become "compute gaps" and the run reads as if it had slack.
    char hdr[96];
    const int hn = std::snprintf(hdr, sizeof hdr, "{\"op\":\"meta\",\"sample\":%d}\n",
                                 sample_ < 1 ? 1 : sample_);
    if (hn > 0 && io_.pwriteFull(fd_, hdr, static_cast<size_t>(hn), off_) == hn)
      off_ += static_cast<uint64_t>(hn);
  }
  if (legend_.insert(h).second) {
    const std::string leg = "{\"op\":\"key\",\"k\":\"" +
                            [&] {
                              char hx[20];
                              std::snprintf(hx, sizeof hx, "%016llx",
                                            static_cast<unsigned long long>(h));
                              return std::string(hx);
                            }() +
                            "\",\"url\":\"" + jsonEscape(key) + "\"}\n";
    if (io_.pwriteFull(fd_, leg.data(), leg.size(), off_) ==
        static_cast<int64_t>(leg.size()))
      off_ += leg.size();
  }
  if (io_.pwriteFull(fd_, line, static_cast<uint64_t>(n), off_) == static_cast<int64_t>(n))
    off_ += static_cast<uint64_t>(n);
}

} // namespace ucache
