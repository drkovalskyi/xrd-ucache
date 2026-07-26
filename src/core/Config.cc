#include "Config.h"

#include "Log.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace ucache {
namespace {

const char* env(const char* name) { return ::getenv(name); }

std::vector<std::string> splitCommas(const std::string& s) {
  std::vector<std::string> out;
  std::istringstream in(s);
  std::string item;
  while (std::getline(in, item, ','))
    if (!item.empty())
      out.push_back(item);
  return out;
}

std::string trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos)
    return "";
  return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

std::string unquote(const std::string& s) {
  if (s.size() >= 2 && (s.front() == '"' || s.front() == '\'') && s.back() == s.front())
    return s.substr(1, s.size() - 2);
  return s;
}

// Cut a config line at the first '#' that is OUTSIDE quotes, so a '#' inside a
// value (e.g. a path "/data/run#42") is preserved rather than truncated.
std::string stripComment(const std::string& line) {
  char q = 0;
  for (size_t i = 0; i < line.size(); ++i) {
    char c = line[i];
    if (q) {
      if (c == q)
        q = 0;
    } else if (c == '"' || c == '\'') {
      q = c;
    } else if (c == '#') {
      return line.substr(0, i);
    }
  }
  return line;
}

// Parse a byte size with optional k/m/g/t suffix (like page_size). Returns false
// (leaving out untouched) on garbage — so a typo can't silently reinterpret the
// value or disable the auto disk-floor.
bool parseBytes(const char* v, uint64_t& out) {
  if (!v || *v < '0' || *v > '9')
    return false; // no leading digit
  char* end = nullptr;
  uint64_t n = ::strtoull(v, &end, 10);
  uint64_t mult = 1;
  switch (end && *end ? *end : '\0') {
    case 'k': case 'K': mult = 1ull << 10; break;
    case 'm': case 'M': mult = 1ull << 20; break;
    case 'g': case 'G': mult = 1ull << 30; break;
    case 't': case 'T': mult = 1ull << 40; break;
    default: break;
  }
  out = n * mult;
  return true;
}

// Shared value interpreters so every layer (conf, state, env) agrees exactly.
void setPageSize(Config& c, const char* v) {
  uint32_t p = static_cast<uint32_t>(::strtoul(v, nullptr, 10));
  if (const char* s = v + ::strspn(v, "0123456789"); *s == 'k' || *s == 'K')
    p *= 1024;
  else if (*s == 'm' || *s == 'M')
    p *= 1024 * 1024;
  if (Config::validPageSize(p))
    c.pageSize = p;
  else
    UCACHE_WARN("page_size=%s invalid (need pow2 in [4KiB,1MiB]); using %u", v, c.pageSize);
}
void setValidate(Config& c, const std::string& s) {
  if (s == "none")
    c.validate = ValidateMode::kNone;
  else if (s == "size")
    c.validate = ValidateMode::kSize;
  else if (s == "size+mtime")
    c.validate = ValidateMode::kSizeMtime;
  else if (s == "cksum")
    c.validate = ValidateMode::kCksum;
  else
    UCACHE_WARN("validate=%s unknown; using 'size'", s.c_str());
}
void setFsync(Config& c, const std::string& s) {
  if (s == "off")
    c.fsync = FsyncMode::kOff;
  else if (s == "data")
    c.fsync = FsyncMode::kData;
  else if (s == "all")
    c.fsync = FsyncMode::kAll;
  else
    UCACHE_WARN("fsync=%s unknown; using 'off'", s.c_str());
}
bool truthy(const std::string& v) { return v == "1" || v == "true" || v == "on"; }
bool falsy(const std::string& v) { return v == "0" || v == "off" || v == "false"; }

// Apply one settings key (shared by the plugin conf, the state file and the
// env layer — every layer parses identically). `src` labels warnings;
// `pluginConf` additionally accepts XrdCl's own keys (url/lib/enable) as
// silent no-ops. Sets explicitMax when a positive byte cap is given, so
// budgetAuto is not turned back on. Returns true iff the key is a recognized
// ucache setting (callers record provenance on true).
bool applyKey(Config& c, const std::string& k, const std::string& v, bool& explicitMax,
              const char* src, bool pluginConf) {
  if (k == "dir")
    c.cacheDir = v;
  else if (k == "page_size")
    setPageSize(c, v.c_str());
  else if (k == "max_bytes") {
    uint64_t mb;
    if (parseBytes(v.c_str(), mb)) {
      c.maxBytes = mb;
      if (mb > 0)
        explicitMax = true; // explicit 0 => no byte cap; auto floor still governs
    } else
      UCACHE_WARN("%s: max_bytes=%s invalid; ignored", src, v.c_str());
  } else if (k == "min_free_bytes") {
    if (!parseBytes(v.c_str(), c.minFreeBytes))
      UCACHE_WARN("%s: min_free_bytes=%s invalid; ignored", src, v.c_str());
  } else if (k == "disable")
    c.disable = truthy(v);
  else if (k == "high_water")
    c.highWater = ::atof(v.c_str());
  else if (k == "low_water")
    c.lowWater = ::atof(v.c_str());
  else if (k == "evict_check_seconds")
    c.evictCheckSeconds = ::atoi(v.c_str());
  else if (k == "validate")
    setValidate(c, v);
  else if (k == "fsync")
    setFsync(c, v);
  else if (k == "threads")
    c.threads = ::atoi(v.c_str());
  else if (k == "max_errors")
    c.maxErrors = ::atoi(v.c_str());
  else if (k == "meta_flush_seconds")
    c.metaFlushSeconds = ::atoi(v.c_str());
  else if (k == "fill_buffer_mb")
    c.fillBufferMb = ::atoi(v.c_str());
  else if (k == "fill_buffer_total_mb")
    c.fillBufferTotalMb = ::atoi(v.c_str());
  else if (k == "revalidate_seconds")
    c.revalidateSeconds = ::atoi(v.c_str());
  else if (k == "open_retries")
    c.openRetries = ::atoi(v.c_str());
  else if (k == "open_retry_base_ms")
    c.openRetryBaseMs = ::atoi(v.c_str());
  else if (k == "open_retry_max_ms")
    c.openRetryMaxMs = ::atoi(v.c_str());
  else if (k == "transpose")
    c.transpose = !falsy(v); // replica-tier kill switch
  else if (k == "recompress")
    c.recompress = truthy(v);
  else if (k == "recompress_codecs")
    c.recompressCodecs = splitCommas(v);
  else if (k == "recompress_drain_jobs") {
    // 0 (or unset) = auto: min(cores/2, threads/2) of the reading process.
    long n = std::strtol(v.c_str(), nullptr, 10);
    if (n < 0) {
      UCACHE_WARN("%s: recompress_drain_jobs '%s' must be >= 0 (0 = auto); ignored", src,
                  v.c_str());
      return false;
    }
    c.recompressDrainJobs = static_cast<int>(n);
  }
  else if (k == "recompress_reclaim") {
    if (v == "superseded")
      c.recompressReclaim = Config::Reclaim::kSuperseded;
    else if (v == "full")
      c.recompressReclaim = Config::Reclaim::kFull;
    else {
      UCACHE_WARN("%s: recompress_reclaim '%s' unknown (superseded|full); ignored", src,
                  v.c_str());
      return false;
    }
  } else if (k == "trace") {
    if (v == "io")
      c.trace = "io";
    else if (v == "off" || falsy(v))
      c.trace.clear();
    else {
      UCACHE_WARN("%s: trace '%s' unknown (io|off); ignored", src, v.c_str());
      return false;
    }
  } else if (k == "trace_sample")
    c.traceSample = ::atoi(v.c_str());
  else if (k == "keep_cgi")
    c.keepCgi = splitCommas(v);
  else if (k == "allow")
    c.allowHosts = splitCommas(v);
  else if (k == "deny")
    c.denyHosts = splitCommas(v);
  else if (k == "log")
    Log::configure(v.c_str());
  else if (pluginConf && (k == "url" || k == "lib" || k == "enable"))
    return false; // XrdCl's own plugin keys — not ours, no warning
  else {
    UCACHE_WARN("%s: unknown key '%s' ignored", src, k.c_str());
    return false;
  }
  return true;
}

// Minimal flat `key = value` file: `#` comments, optional quotes;
// no sections/arrays. Absent file = no-op. Parses the XrdCl plugin conf
// (tag "conf", pluginConf=true) and the CLI-managed state file (tag "state",
// where `dir` is refused — the state file lives inside the cache dir, so it
// cannot relocate it).
void loadKeyValueFile(Config& c, const std::string& path, bool& explicitMax, const char* tag) {
  std::ifstream in(path);
  if (!in)
    return;
  const bool pluginConf = ::strcmp(tag, "conf") == 0;
  const char* src = pluginConf ? "ucache.conf" : tag;
  std::string line;
  while (std::getline(in, line)) {
    line = stripComment(line);
    auto eq = line.find('=');
    if (eq == std::string::npos)
      continue;
    std::string k = trim(line.substr(0, eq));
    std::string v = unquote(trim(line.substr(eq + 1)));
    if (k.empty() || v.empty())
      continue;
    if (!pluginConf && k == "dir") {
      UCACHE_WARN("state: 'dir' cannot be set in the state file; ignored");
      continue;
    }
    if (applyKey(c, k, v, explicitMax, src, pluginConf))
      c.sources[k] = tag;
  }
}

// Env layer (highest): every UCACHE_* twin routes through the SAME applyKey
// as the files, so a value means the same thing wherever it is set.
void applyEnvLayer(Config& c, bool& explicitMax) {
  for (const auto& spec : Config::knownKeys()) {
    const char* v = env(spec.envName);
    if (!v || !*v) // empty = unset (keep the file/state value)
      continue;
    if (applyKey(c, spec.key, v, explicitMax, spec.envName, /*pluginConf=*/false))
      c.sources[spec.key] = "env";
  }
}

// Final validation + derived flags, applied once after all layers.
void clampAndFinish(Config& c, bool explicitMax) {
  if (c.evictCheckSeconds < 0)
    c.evictCheckSeconds = 10;
  if (c.lowWater <= 0 || c.highWater > 1.0 || c.lowWater >= c.highWater) {
    UCACHE_WARN("invalid watermarks (%.2f/%.2f); using 0.90/0.75", c.highWater, c.lowWater);
    c.highWater = 0.90;
    c.lowWater = 0.75;
  }
  // budgetAuto iff NO explicit (positive) byte cap from any layer: resolve a
  // free-disk floor in CacheStore.
  c.budgetAuto = !explicitMax;
}

// Layers above the conf: state file (found under the EFFECTIVE cache dir —
// env UCACHE_DIR wins over the conf for locating it, matching the final
// precedence; no dir anywhere => no state layer), then env.
void finishLoad(Config& c, bool& explicitMax) {
  std::string dir = c.cacheDir;
  if (const char* v = env("UCACHE_DIR"); v && *v)
    dir = v;
  if (!dir.empty())
    loadKeyValueFile(c, Config::statePath(dir), explicitMax, "state");
  applyEnvLayer(c, explicitMax);
  clampAndFinish(c, explicitMax);
}

} // namespace

Config Config::fromEnv() {
  return fromEnv(static_cast<const std::map<std::string, std::string>*>(nullptr));
}

Config Config::fromEnv(const std::string& pluginConfPath) {
  Config c;
  bool explicitMax = false;
  if (!pluginConfPath.empty())
    loadKeyValueFile(c, pluginConfPath, explicitMax, "conf");
  finishLoad(c, explicitMax);
  return c;
}

Config Config::fromEnv(const std::map<std::string, std::string>* pluginConf) {
  Config c;
  bool explicitMax = false;

  // The XrdCl plugin conf — the single user-facing DEFAULTS file.
  // XrdCl hands the conf's key/value map to XrdClGetPlugIn; url/lib/enable
  // are XrdCl's own keys.
  if (pluginConf)
    for (const auto& [k, v] : *pluginConf) {
      if (k.empty() || v.empty())
        continue;
      if (applyKey(c, k, v, explicitMax, "ucache.conf", /*pluginConf=*/true))
        c.sources[k] = "conf";
    }

  finishLoad(c, explicitMax);
  return c;
}

const std::vector<Config::KeyInfo>& Config::knownKeys() {
  static const std::vector<KeyInfo> kKeys = {
      {"dir", "UCACHE_DIR"},
      {"page_size", "UCACHE_PAGE_SIZE"},
      {"max_bytes", "UCACHE_MAX_BYTES"},
      {"min_free_bytes", "UCACHE_MIN_FREE_BYTES"},
      {"evict_check_seconds", "UCACHE_EVICT_CHECK_S"},
      {"high_water", "UCACHE_HIGH_WATER"},
      {"low_water", "UCACHE_LOW_WATER"},
      {"validate", "UCACHE_VALIDATE"},
      {"fsync", "UCACHE_FSYNC"},
      {"threads", "UCACHE_THREADS"},
      {"max_errors", "UCACHE_MAX_ERRORS"},
      {"meta_flush_seconds", "UCACHE_META_FLUSH_S"},
      {"fill_buffer_mb", "UCACHE_FILL_BUFFER_MB"},
      {"fill_buffer_total_mb", "UCACHE_FILL_BUFFER_TOTAL_MB"},
      {"revalidate_seconds", "UCACHE_REVALIDATE_S"},
      {"open_retries", "UCACHE_OPEN_RETRIES"},
      {"open_retry_base_ms", "UCACHE_OPEN_RETRY_BASE_MS"},
      {"open_retry_max_ms", "UCACHE_OPEN_RETRY_MAX_MS"},
      {"disable", "UCACHE_DISABLE"},
      {"transpose", "UCACHE_TRANSPOSE"},
      {"recompress", "UCACHE_RECOMPRESS"},
      {"recompress_codecs", "UCACHE_RECOMPRESS_CODECS"},
      {"recompress_reclaim", "UCACHE_RECOMPRESS_RECLAIM"},
      {"recompress_drain_jobs", "UCACHE_RECOMPRESS_DRAIN_JOBS"},
      {"trace", "UCACHE_TRACE"},
      {"trace_sample", "UCACHE_TRACE_SAMPLE"},
      {"keep_cgi", "UCACHE_KEEP_CGI"},
      {"allow", "UCACHE_ALLOW"},
      {"deny", "UCACHE_DENY"},
      {"log", "UCACHE_LOG"},
  };
  return kKeys;
}

bool Config::stateSettable(const std::string& key) {
  if (key == "dir")
    return false; // the state file lives inside the cache dir
  for (const auto& spec : knownKeys())
    if (key == spec.key)
      return true;
  return false;
}

std::string Config::valueOf(const std::string& key) const {
  auto onoff = [](bool x) { return std::string(x ? "on" : "off"); };
  auto join = [](const std::vector<std::string>& v) {
    std::string s;
    for (const auto& e : v) {
      if (!s.empty())
        s += ",";
      s += e;
    }
    return s.empty() ? std::string("-") : s;
  };
  if (key == "dir")
    return cacheDir.empty() ? "(unset)" : cacheDir;
  if (key == "page_size")
    return std::to_string(pageSize);
  if (key == "max_bytes")
    return std::to_string(maxBytes);
  if (key == "min_free_bytes")
    return minFreeBytes ? std::to_string(minFreeBytes) : std::string(budgetAuto ? "0 (auto)" : "0");
  if (key == "evict_check_seconds")
    return std::to_string(evictCheckSeconds);
  if (key == "high_water" || key == "low_water") {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.2f", key == "high_water" ? highWater : lowWater);
    return buf;
  }
  if (key == "validate")
    switch (validate) {
      case ValidateMode::kNone: return "none";
      case ValidateMode::kSize: return "size";
      case ValidateMode::kSizeMtime: return "size+mtime";
      case ValidateMode::kCksum: return "cksum";
    }
  if (key == "fsync")
    switch (fsync) {
      case FsyncMode::kOff: return "off";
      case FsyncMode::kData: return "data";
      case FsyncMode::kAll: return "all";
    }
  if (key == "threads")
    return std::to_string(threads);
  if (key == "max_errors")
    return std::to_string(maxErrors);
  if (key == "fill_buffer_mb")
    return std::to_string(fillBufferMb);
  if (key == "fill_buffer_total_mb")
    return std::to_string(fillBufferTotalMb);
  if (key == "meta_flush_seconds")
    return std::to_string(metaFlushSeconds);
  if (key == "revalidate_seconds")
    return std::to_string(revalidateSeconds);
  if (key == "open_retries")
    return std::to_string(openRetries);
  if (key == "open_retry_base_ms")
    return std::to_string(openRetryBaseMs);
  if (key == "open_retry_max_ms")
    return std::to_string(openRetryMaxMs);
  if (key == "disable")
    return onoff(disable);
  if (key == "transpose")
    return onoff(transpose);
  if (key == "recompress")
    return onoff(recompress);
  if (key == "recompress_drain_jobs")
    return std::to_string(recompressDrainJobs);
  if (key == "recompress_codecs")
    return join(recompressCodecs);
  if (key == "recompress_reclaim")
    return recompressReclaim == Reclaim::kFull ? "full" : "superseded";
  if (key == "trace")
    return trace.empty() ? "off" : trace;
  if (key == "trace_sample")
    return std::to_string(traceSample);
  if (key == "keep_cgi")
    return join(keepCgi);
  if (key == "allow")
    return join(allowHosts);
  if (key == "deny")
    return join(denyHosts);
  if (key == "log")
    return "-"; // write-only: configures the logger, not stored
  return "?";
}

} // namespace ucache
