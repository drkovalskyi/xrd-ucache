// ucache-netbench: origin random-read baseline — the network-side
// companion of `ucache bench`. Measures what a remote source (EOS, any
// xrootd endpoint) delivers for basket-sized random reads at 1..N parallel
// streams, so a cache location's numbers can be compared against THIS
// machine's actual origin instead of a hard-coded reference. RAW numbers,
// same output discipline as `ucache bench` (human table + one JSON line).
//
// The probe DISABLES the ucache plugin for its own connections (else it
// would measure the cache it is meant to baseline); --through-cache keeps
// the plugin engaged for an end-to-end comparison run.
//
// Standalone binary on purpose: the `ucache` CLI links no XrootD; this tool
// is built and shipped next to the plugin, which already requires XrdCl.
#include <XrdCl/XrdClFile.hh>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

double nowS() {
  return std::chrono::duration<double>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

struct StreamResult {
  int n = 0;
  double iops = 0, mbps = 0;
  uint64_t p50 = 0, p95 = 0, p99 = 0; // µs
};

uint64_t pct(std::vector<uint32_t>& us, double p) {
  if (us.empty())
    return 0;
  size_t i = std::min(us.size() - 1, static_cast<size_t>(us.size() * p));
  return us[i];
}

void usage() {
  std::fputs(
      "usage: ucache-netbench <root://host//path/file> [options]\n"
      "  --block SZKB      read size in KiB (default 4 — matches `ucache bench`'s\n"
      "                    small-read size for direct comparison; 16 = basket)\n"
      "  --seconds S       per-configuration duration (default 5)\n"
      "  --streams LIST    comma list of parallel-stream counts (default 1,4,16)\n"
      "  --through-cache   keep the ucache plugin engaged (default: disabled,\n"
      "                    so the ORIGIN is measured, not the cache)\n"
      "  --seed N          fixed offset sequence (default: fresh offsets every\n"
      "                    run/config, so FST warming is not overstated)\n"
      "Measures random reads at each stream count; prints a table and one\n"
      "ucache-netbench-json line for collection. Compare against `ucache\n"
      "bench`: a cache location must beat these numbers to be worth having.\n",
      stderr);
}

} // namespace

int main(int argc, char** argv) {
  std::string url;
  uint64_t blockKb = 4;
  double seconds = 5.0;
  std::vector<int> streams = {1, 4, 16};
  bool throughCache = false;
  // Fresh offsets every invocation and every stream-config by default: fixed
  // seeds made later configs (and repeat runs) re-read offsets earlier ones
  // had just warmed on the FST, poisoning the medians (found on a production
  // cache: cold 3.3 GB file, 16-stream p50 = 20 us). --seed pins
  // the sequence when reproducibility matters more than cold fidelity.
  uint64_t seedBase = static_cast<uint64_t>(::time(nullptr)) * 2654435761ull;

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto val = [&](const char* what) -> const char* {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "netbench: %s needs a value\n", what);
        return nullptr;
      }
      return argv[++i];
    };
    if (a == "--block") {
      const char* v = val("--block");
      if (!v || (blockKb = std::strtoull(v, nullptr, 10)) == 0 || blockKb > 65536)
        return 2;
    } else if (a == "--seconds") {
      const char* v = val("--seconds");
      if (!v || (seconds = std::strtod(v, nullptr)) <= 0 || seconds > 300)
        return 2;
    } else if (a == "--streams") {
      const char* v = val("--streams");
      if (!v)
        return 2;
      streams.clear();
      for (const char* p = v; *p;) {
        int n = std::atoi(p);
        if (n <= 0 || n > 256)
          return 2;
        streams.push_back(n);
        while (*p && *p != ',')
          ++p;
        if (*p == ',')
          ++p;
      }
      if (streams.empty())
        return 2;
    } else if (a == "--seed") {
      const char* v = val("--seed");
      if (!v)
        return 2;
      seedBase = std::strtoull(v, nullptr, 10);
    } else if (a == "--through-cache") {
      throughCache = true;
    } else if (a == "--help" || a == "-h") {
      usage();
      return 0;
    } else if (a[0] == '-') {
      std::fprintf(stderr, "netbench: unknown option '%s'\n", a.c_str());
      return 2;
    } else {
      if (!url.empty()) {
        std::fprintf(stderr,
                     "netbench: two file arguments ('%s' and '%s') — is there a\n"
                     "space inside the root:// URL? It must be ONE argument:\n"
                     "  root://host//path/file.root\n",
                     url.c_str(), a.c_str());
        return 2;
      }
      url = a;
    }
  }
  if (url.empty()) {
    usage();
    return 2;
  }
  // A protocol-less argument is a LOCAL path to XrdCl (e.g. an EOS FUSE
  // mount) — a legitimate thing to measure, but it must never masquerade as
  // an origin baseline (bitten twice in the field by URL-splitting typos).
  const bool localPath = url.find("://") == std::string::npos;
  if (localPath)
    std::fprintf(stderr, "netbench: NOTE — '%s' has no protocol: measuring the\n"
                         "LOCAL path (FUSE mount?), not a remote origin\n",
                 url.c_str());
  // Must happen before ANY XrdCl activity: plugin selection is per-process.
  if (!throughCache)
    ::setenv("UCACHE_DISABLE", "1", 1);

  const uint64_t block = blockKb * 1024;

  // Probe the file size (and fail fast on auth/route problems).
  uint64_t fsize = 0;
  {
    XrdCl::File f;
    auto st = f.Open(url, XrdCl::OpenFlags::Read);
    if (!st.IsOK()) {
      std::fprintf(stderr, "netbench: open failed: %s\n", st.ToString().c_str());
      return 1;
    }
    XrdCl::StatInfo* info = nullptr;
    if (f.Stat(false, info).IsOK() && info)
      fsize = info->GetSize();
    delete info;
    auto cs = f.Close();
    (void)cs;
  }
  if (fsize < block * 4) {
    std::fprintf(stderr, "netbench: file too small (%llu bytes)\n",
                 static_cast<unsigned long long>(fsize));
    return 1;
  }

  char host[256] = "?";
  ::gethostname(host, sizeof host - 1);
  std::printf("=== ucache-netbench: %s ===\n", url.c_str());
  std::printf("  client %s | file %.1f MB | block %llu KiB | %s | %.0fs/config\n\n",
              host, fsize / 1e6, static_cast<unsigned long long>(blockKb),
              localPath      ? "LOCAL PATH (fuse?)"
              : throughCache ? "THROUGH CACHE"
                             : "plugin disabled (origin)",
              seconds);
  std::printf("%8s  %10s  %10s  %9s  %9s  %9s\n", "streams", "IOPS", "MB/s",
              "p50 ms", "p95 ms", "p99 ms");

  std::vector<StreamResult> results;
  int cfgIdx = 0;
  for (int n : streams) {
    // Pre-open one handle per stream (serialized: opens are not the subject).
    std::vector<std::unique_ptr<XrdCl::File>> files;
    bool ok = true;
    for (int t = 0; t < n; ++t) {
      auto f = std::make_unique<XrdCl::File>();
      if (!f->Open(url, XrdCl::OpenFlags::Read).IsOK()) {
        ok = false;
        break;
      }
      files.push_back(std::move(f));
    }
    if (!ok) {
      std::fprintf(stderr, "netbench: open failed at %d streams\n", n);
      break;
    }
    std::atomic<uint64_t> ops{0};
    std::mutex mu;
    std::vector<uint32_t> lat;
    double t0 = nowS(), deadline = t0 + seconds;
    std::vector<std::thread> pool;
    for (int t = 0; t < n; ++t)
      pool.emplace_back([&, t] {
        XrdCl::File* f = files[t].get();
        std::mt19937_64 rng(seedBase + 1000003ull * static_cast<uint64_t>(cfgIdx) + t);
        std::vector<char> buf(block);
        std::vector<uint32_t> mine;
        const uint64_t slots = (fsize - block) / block;
        while (nowS() < deadline) {
          uint64_t off = (rng() % slots) * block;
          uint32_t got = 0;
          double r0 = nowS();
          if (!f->Read(off, static_cast<uint32_t>(block), buf.data(), got).IsOK())
            break;
          if (mine.size() < 200000)
            mine.push_back(static_cast<uint32_t>((nowS() - r0) * 1e6));
          ops.fetch_add(1, std::memory_order_relaxed);
        }
        std::lock_guard<std::mutex> g(mu);
        lat.insert(lat.end(), mine.begin(), mine.end());
      });
    for (auto& t : pool)
      t.join();
    double elapsed = nowS() - t0;
    for (auto& f : files) {
      auto cs = f->Close();
      (void)cs;
    }

    std::sort(lat.begin(), lat.end());
    StreamResult r;
    r.n = n;
    r.iops = static_cast<double>(ops.load()) / elapsed;
    r.mbps = r.iops * static_cast<double>(block) / 1e6;
    r.p50 = pct(lat, 0.50);
    r.p95 = pct(lat, 0.95);
    r.p99 = pct(lat, 0.99);
    results.push_back(r);
    std::printf("%8d  %10.0f  %10.1f  %9.2f  %9.2f  %9.2f\n", r.n, r.iops, r.mbps,
                r.p50 / 1e3, r.p95 / 1e3, r.p99 / 1e3);
    ++cfgIdx;
  }

  std::printf("\nucache-netbench-json: {\"schema\":1,\"host\":\"%s\",\"url\":\"%s\","
              "\"block_kb\":%llu,\"seconds\":%.1f,\"mode\":\"%s\",\"file_mb\":%.0f,"
              "\"streams\":[",
              host, url.c_str(), static_cast<unsigned long long>(blockKb), seconds,
              localPath      ? "local-path"
              : throughCache ? "through-cache"
                             : "origin",
              fsize / 1e6);
  for (size_t i = 0; i < results.size(); ++i) {
    const auto& r = results[i];
    std::printf("%s{\"n\":%d,\"iops\":%.0f,\"mbps\":%.1f,\"p50_us\":%llu,"
                "\"p95_us\":%llu,\"p99_us\":%llu}",
                i ? "," : "", r.n, r.iops, r.mbps,
                static_cast<unsigned long long>(r.p50),
                static_cast<unsigned long long>(r.p95),
                static_cast<unsigned long long>(r.p99));
  }
  std::printf("]}\n");
  return results.empty() ? 1 : 0;
}
