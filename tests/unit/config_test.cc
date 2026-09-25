#include "Config.h"
#include "Log.h"

#include "TestUtil.h"
#include <cstdlib>
#include <fstream>
#include <gtest/gtest.h>
#include <iterator>
#include <map>
#include <sys/stat.h>

using namespace ucache;

namespace {
struct EnvGuard {
  // Tests are hermetic by construction: the legacy config.toml layer
  // is retired, and the state layer only loads from an explicitly-set cache
  // dir. Clear every UCACHE_* twin on teardown.
  ~EnvGuard() {
    for (const auto& k : Config::knownKeys())
      ::unsetenv(k.envName);
    for (const auto& r : Config::retiredKeys())
      if (r.envName)
        ::unsetenv(r.envName);
  }
};
} // namespace

TEST(Config, Defaults) {
  EnvGuard g;
  Config c = Config::fromEnv();
  EXPECT_EQ(c.pageSize, 4096u); // measured default
  EXPECT_EQ(c.maxBytes, 0u);
  EXPECT_DOUBLE_EQ(c.highWater, 0.90);
  EXPECT_DOUBLE_EQ(c.lowWater, 0.75);
  EXPECT_EQ(c.validate, ValidateMode::kSize);
  EXPECT_EQ(c.fsync, FsyncMode::kOff);
  EXPECT_EQ(c.maxErrors, 5);
  EXPECT_EQ(c.metaFlushSeconds, 30);
  EXPECT_EQ(c.revalidateSeconds, 604800); // 7-day freshness window
  EXPECT_FALSE(c.recompress); // recompression is a single opt-in switch
  EXPECT_EQ(c.recompressCodecs, (std::vector<std::string>{"lzma", "zlib"}));
  EXPECT_EQ(c.recompressReclaim, Config::Reclaim::kSuperseded); // default reclaim mode
  EXPECT_TRUE(c.transpose);
  EXPECT_TRUE(c.announce); // servers are told uCache is in the path, by default
  EXPECT_FALSE(c.disable);
  EXPECT_FALSE(c.recompressKeepOriginals); // a converted basket is not also kept
  EXPECT_TRUE(c.cacheDir.empty()); // deliberately NO default cache dir
  EXPECT_TRUE(c.sources.empty());  // nothing explicitly set anywhere
}

TEST(Config, ParsesEverything) {
  EnvGuard g;
  ::setenv("UCACHE_DIR", "/x/y", 1);
  ::setenv("UCACHE_PAGE_SIZE", "16k", 1);
  ::setenv("UCACHE_MAX_BYTES", "1000000", 1);
  ::setenv("UCACHE_HIGH_WATER", "0.8", 1);
  ::setenv("UCACHE_LOW_WATER", "0.5", 1);
  ::setenv("UCACHE_VALIDATE", "size+mtime", 1);
  ::setenv("UCACHE_FSYNC", "data", 1);
  ::setenv("UCACHE_THREADS", "4", 1);
  ::setenv("UCACHE_MAX_ERRORS", "9", 1);
  ::setenv("UCACHE_META_FLUSH_S", "7", 1);
  ::setenv("UCACHE_REVALIDATE_S", "0", 1); // 0 must override the 7-day default
  ::setenv("UCACHE_DISABLE", "1", 1);
  ::setenv("UCACHE_TRANSPOSE", "off", 1);
  ::setenv("UCACHE_ANNOUNCE", "off", 1);
  ::setenv("UCACHE_RECOMPRESS", "on", 1);
  ::setenv("UCACHE_RECOMPRESS_RECLAIM", "full", 1);
  ::setenv("UCACHE_RECOMPRESS_KEEP_ORIGINALS", "on", 1);
  ::setenv("UCACHE_KEEP_CGI", "a,b", 1);
  ::setenv("UCACHE_ALLOW", "*.cern.ch", 1);
  ::setenv("UCACHE_DENY", "bad.host", 1);
  Config c = Config::fromEnv();
  EXPECT_EQ(c.cacheDir, "/x/y");
  EXPECT_EQ(c.pageSize, 16384u);
  EXPECT_EQ(c.maxBytes, 1000000u);
  EXPECT_DOUBLE_EQ(c.highWater, 0.8);
  EXPECT_DOUBLE_EQ(c.lowWater, 0.5);
  EXPECT_EQ(c.validate, ValidateMode::kSizeMtime);
  EXPECT_EQ(c.fsync, FsyncMode::kData);
  EXPECT_EQ(c.threads, 4);
  EXPECT_EQ(c.maxErrors, 9);
  EXPECT_EQ(c.metaFlushSeconds, 7);
  EXPECT_EQ(c.revalidateSeconds, 0);
  EXPECT_TRUE(c.disable);
  EXPECT_FALSE(c.transpose);
  EXPECT_FALSE(c.announce);
  EXPECT_EQ(c.valueOf("announce"), "off"); // reported by `ucache settings`
  EXPECT_TRUE(c.recompress);
  EXPECT_EQ(c.recompressReclaim, Config::Reclaim::kFull);
  EXPECT_EQ(c.valueOf("recompress_reclaim"), "full");
  EXPECT_TRUE(c.recompressKeepOriginals);
  EXPECT_EQ(c.valueOf("recompress_keep_originals"), "on");
  EXPECT_EQ(c.keepCgi, (std::vector<std::string>{"a", "b"}));
  EXPECT_EQ(c.allowHosts, (std::vector<std::string>{"*.cern.ch"}));
  EXPECT_EQ(c.denyHosts, (std::vector<std::string>{"bad.host"}));
  EXPECT_EQ(c.sources.at("revalidate_seconds"), "env");
}

// ucache settings live in the XrdCl plugin conf. Layering:
// defaults < plugin conf < <cacheDir>/state < UCACHE_* env.
TEST(Config, PluginConfMapLayer) {
  EnvGuard g;
  std::map<std::string, std::string> conf = {
      {"url", "*"}, // XrdCl's keys: silently ignored
      {"lib", "/x/libXrdClUCache.so"},
      {"enable", "true"},
      {"dir", "/conf/cache"}, // ours
      {"revalidate_seconds", "42"},
      {"max_bytes", "1000000"},
      {"recompress", "on"},
      {"recompress_codecs", "lzma,zlib"},
      {"recompress_min_share", "25"}, // retired key: warned + ignored
      {"recompress_reclaim", "everything"}, // bad value: warned + ignored
  };
  Config c = Config::fromEnv(&conf);
  EXPECT_EQ(c.cacheDir, "/conf/cache");
  EXPECT_EQ(c.revalidateSeconds, 42);
  EXPECT_EQ(c.maxBytes, 1000000u);
  EXPECT_FALSE(c.budgetAuto); // explicit positive cap in the conf
  EXPECT_TRUE(c.recompress);
  EXPECT_EQ(c.recompressCodecs, (std::vector<std::string>{"lzma", "zlib"}));
  EXPECT_EQ(c.sources.at("dir"), "conf");
  EXPECT_EQ(c.sources.count("recompress_min_share"), 0u); // unknown keys get no source
  EXPECT_EQ(c.recompressReclaim, Config::Reclaim::kSuperseded); // bad value kept default
  EXPECT_EQ(c.sources.count("recompress_reclaim"), 0u);

  // env overrides the plugin conf
  ::setenv("UCACHE_DIR", "/env/cache", 1);
  ::setenv("UCACHE_REVALIDATE_S", "7", 1);
  c = Config::fromEnv(&conf);
  EXPECT_EQ(c.cacheDir, "/env/cache");
  EXPECT_EQ(c.revalidateSeconds, 7);
  EXPECT_EQ(c.sources.at("dir"), "env");
}

// The state file (CLI-written CURRENT values) sits between the conf
// (the user's defaults) and the env (per-job override).
TEST(Config, StateLayerBetweenConfAndEnv) {
  EnvGuard g;
  test::TempDir td;
  const std::string cache = td.path() + "/cache";
  ::mkdir(cache.c_str(), 0755);
  const std::string conf = td.path() + "/ucache.conf";
  std::ofstream(conf) << "url = *\nlib = /x/libXrdClUCache.so\nenable = true\n"
                      << "dir = " << cache << "\nrevalidate_seconds = 1\nrecompress = off\n";
  std::ofstream(Config::statePath(cache)) << "# managed\nrevalidate_seconds = 2\n"
                                          << "recompress = on\n";
  Config c = Config::fromEnv(conf);
  EXPECT_EQ(c.revalidateSeconds, 2); // state wins over conf
  EXPECT_TRUE(c.recompress);
  EXPECT_EQ(c.sources.at("revalidate_seconds"), "state");
  EXPECT_EQ(c.sources.at("recompress"), "state");
  EXPECT_EQ(c.sources.at("dir"), "conf");

  ::setenv("UCACHE_REVALIDATE_S", "3", 1); // env wins over state
  c = Config::fromEnv(conf);
  EXPECT_EQ(c.revalidateSeconds, 3);
  EXPECT_EQ(c.sources.at("revalidate_seconds"), "env");
  EXPECT_TRUE(c.recompress); // untouched state key still applies
}

// `dir` in the state file would be circular (the file lives inside the cache
// dir) — it must be refused, loudly ignored.
TEST(Config, StateCannotRelocateCacheDir) {
  EnvGuard g;
  test::TempDir td;
  const std::string cache = td.path() + "/cache";
  ::mkdir(cache.c_str(), 0755);
  const std::string conf = td.path() + "/ucache.conf";
  std::ofstream(conf) << "dir = " << cache << "\n";
  std::ofstream(Config::statePath(cache)) << "dir = /evil/elsewhere\nthreads = 3\n";
  Config c = Config::fromEnv(conf);
  EXPECT_EQ(c.cacheDir, cache); // unmoved
  EXPECT_EQ(c.threads, 3);      // the rest of the state still applies
}

// Bootstrap order: UCACHE_DIR alone (no conf) must still locate the state
// file, and env stays the highest layer for every other key.
TEST(Config, StateDiscoveredViaEnvDir) {
  EnvGuard g;
  test::TempDir td;
  std::ofstream(Config::statePath(td.path())) << "recompress = on\nthreads = 5\n";
  ::setenv("UCACHE_DIR", td.path().c_str(), 1);
  ::setenv("UCACHE_THREADS", "7", 1);
  Config c = Config::fromEnv();
  EXPECT_EQ(c.cacheDir, td.path());
  EXPECT_TRUE(c.recompress);            // from state, found via env dir
  EXPECT_EQ(c.threads, 7);              // env beats state
  EXPECT_EQ(c.sources.at("recompress"), "state");
  EXPECT_EQ(c.sources.at("threads"), "env");
}

TEST(Config, RejectsBadValues) {
  EnvGuard g;
  ::setenv("UCACHE_PAGE_SIZE", "12345", 1); // not a power of two
  ::setenv("UCACHE_HIGH_WATER", "0.5", 1);
  ::setenv("UCACHE_LOW_WATER", "0.9", 1); // low > high
  Config c = Config::fromEnv();
  EXPECT_EQ(c.pageSize, 4096u);
  EXPECT_DOUBLE_EQ(c.highWater, 0.90);
  EXPECT_DOUBLE_EQ(c.lowWater, 0.75);
  ::setenv("UCACHE_PAGE_SIZE", "2048", 1); // below 4 KiB floor
  c = Config::fromEnv();
  EXPECT_EQ(c.pageSize, 4096u);
  ::setenv("UCACHE_PAGE_SIZE", "2m", 1); // above 1 MiB ceiling
  c = Config::fromEnv();
  EXPECT_EQ(c.pageSize, 4096u);
  ::setenv("UCACHE_VALIDATE", "bogus", 1);
  c = Config::fromEnv();
  EXPECT_EQ(c.validate, ValidateMode::kSize);
}

TEST(Config, ConfFileLayeredUnderEnv) {
  EnvGuard g;
  test::TempDir td;
  std::string path = td.path() + "/ucache.conf";
  {
    std::ofstream f(path);
    f << "# ucache conf\n"
      << "dir = \"/from/file\"\n"
      << "page_size = 16k\n"
      << "max_bytes = 5000000\n"
      << "min_free_bytes = 2000000\n"
      << "validate = size+mtime\n";
  }
  // File values applied; an explicit max_bytes turns budgetAuto OFF.
  Config c = Config::fromEnv(path);
  EXPECT_EQ(c.cacheDir, "/from/file");
  EXPECT_EQ(c.pageSize, 16384u);
  EXPECT_EQ(c.maxBytes, 5000000u);
  EXPECT_EQ(c.minFreeBytes, 2000000u);
  EXPECT_EQ(c.validate, ValidateMode::kSizeMtime);
  EXPECT_FALSE(c.budgetAuto);

  // Env overrides the file for the keys it sets; the rest stay from the file.
  ::setenv("UCACHE_DIR", "/from/env", 1);
  c = Config::fromEnv(path);
  EXPECT_EQ(c.cacheDir, "/from/env"); // env wins
  EXPECT_EQ(c.maxBytes, 5000000u);    // still from file
}

TEST(Config, BudgetAutoWhenNoExplicitCap) {
  EnvGuard g; // no conf, no UCACHE_MAX_BYTES
  Config c = Config::fromEnv();
  EXPECT_EQ(c.maxBytes, 0u);
  EXPECT_TRUE(c.budgetAuto); // floor-governed default
}

TEST(Config, ByteSuffixesZeroAndGarbage) {
  EnvGuard g;
  ::setenv("UCACHE_MAX_BYTES", "2m", 1);
  ::setenv("UCACHE_MIN_FREE_BYTES", "3g", 1);
  Config c = Config::fromEnv();
  EXPECT_EQ(c.maxBytes, 2u << 20);
  EXPECT_EQ(c.minFreeBytes, 3ull << 30);
  EXPECT_FALSE(c.budgetAuto); // positive cap => hard-cap mode
  // Explicit 0 => no byte cap; floor still governs (budgetAuto stays on).
  ::setenv("UCACHE_MAX_BYTES", "0", 1);
  c = Config::fromEnv();
  EXPECT_EQ(c.maxBytes, 0u);
  EXPECT_TRUE(c.budgetAuto);
  // Garbage => ignored; must NOT silently disable the auto floor.
  ::setenv("UCACHE_MAX_BYTES", "notanumber", 1);
  c = Config::fromEnv();
  EXPECT_TRUE(c.budgetAuto);
}

TEST(Config, FileDisableAndHashInsideQuotedValue) {
  EnvGuard g;
  test::TempDir td;
  std::string path = td.path() + "/ucache.conf";
  {
    std::ofstream f(path);
    f << "disable = true\n";
    f << "dir = \"/data/run#42/cache\"\n"; // '#' inside quotes must survive
  }
  Config c = Config::fromEnv(path);
  EXPECT_TRUE(c.disable);
  EXPECT_EQ(c.cacheDir, "/data/run#42/cache");
}

TEST(Config, StateSettableVocabulary) {
  EXPECT_TRUE(Config::stateSettable("recompress"));
  EXPECT_TRUE(Config::stateSettable("revalidate_seconds"));
  EXPECT_FALSE(Config::stateSettable("dir"));           // lives outside the state file
  EXPECT_FALSE(Config::stateSettable("no_such_key"));   // typo protection
  EXPECT_FALSE(Config::stateSettable("recompress_min_share")); // retired key
}

namespace {
std::string slurp(const std::string& path) {
  std::ifstream in(path);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
size_t countOf(const std::string& hay, const std::string& needle) {
  size_t n = 0;
  for (size_t at = hay.find(needle); at != std::string::npos; at = hay.find(needle, at + 1))
    ++n;
  return n;
}
} // namespace

// A setting that no longer exists is named, with its reason, wherever it is
// still set — never reported as a typo, never recorded as a source, never
// offered by `ucache set`.
TEST(Config, RetiredDrainJobsKeyIsNamed) {
  EnvGuard g;
  test::TempDir td;
  const std::string log = td.path() + "/log.txt";
  Log::configure("warn:" + log);
  const std::string cache = td.path() + "/cache";
  ::mkdir(cache.c_str(), 0755);
  std::ofstream(Config::statePath(cache)) << "recompress_drain_jobs = 4\n";
  std::map<std::string, std::string> conf = {{"dir", cache}, {"recompress_drain_jobs", "4"}};
  ::setenv("UCACHE_RECOMPRESS_DRAIN_JOBS", "4", 1);
  Config c = Config::fromEnv(&conf);
  Log::configure("warn");
  const std::string all = slurp(log);
  EXPECT_NE(all.find("ucache.conf: 'recompress_drain_jobs' is no longer a setting"),
            std::string::npos)
      << all;
  EXPECT_NE(all.find("state: 'recompress_drain_jobs' is no longer a setting"), std::string::npos);
  EXPECT_NE(all.find("`ucache unset recompress_drain_jobs` removes it"), std::string::npos);
  EXPECT_NE(all.find("background recompression worker"), std::string::npos); // the reason
  EXPECT_EQ(countOf(all, "UCACHE_RECOMPRESS_DRAIN_JOBS is no longer used"), 1u);
  EXPECT_EQ(all.find("unknown key"), std::string::npos); // named, not a typo
  EXPECT_EQ(c.sources.count("recompress_drain_jobs"), 0u);
  EXPECT_FALSE(Config::stateSettable("recompress_drain_jobs"));
  for (const auto& k : Config::knownKeys())
    EXPECT_STRNE(k.key, "recompress_drain_jobs");
  EXPECT_NE(Config::retiredReason("recompress_drain_jobs"), nullptr);
  EXPECT_EQ(Config::retiredReason("recompress"), nullptr); // a live key is not retired
}

// The helper override has no key at all: only the environment can carry it, so
// the environment layer names it — once per process, however often the
// settings are loaded.
TEST(Config, RetiredHelperEnvIsNamedOnce) {
  EnvGuard g;
  test::TempDir td;
  const std::string log = td.path() + "/log.txt";
  Log::configure("warn:" + log);
  ::setenv("UCACHE_RECOMPRESS_HELPER", "/x/ucache", 1);
  Config::fromEnv();
  Config::fromEnv();
  Log::configure("warn");
  const std::string all = slurp(log);
  EXPECT_EQ(countOf(all, "UCACHE_RECOMPRESS_HELPER is no longer used"), 1u) << all;
  EXPECT_NE(all.find("background recompression worker"), std::string::npos);
  EXPECT_NE(Config::retiredReason("UCACHE_RECOMPRESS_HELPER"), nullptr);
}

TEST(Config, ValidPageSize) {
  EXPECT_TRUE(Config::validPageSize(4096));
  EXPECT_TRUE(Config::validPageSize(1u << 20));
  EXPECT_FALSE(Config::validPageSize(0));
  EXPECT_FALSE(Config::validPageSize(4095));
  EXPECT_FALSE(Config::validPageSize(2048));
  EXPECT_FALSE(Config::validPageSize(2u << 20));
}
