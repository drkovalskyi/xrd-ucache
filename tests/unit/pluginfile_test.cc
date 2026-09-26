// Which plugin file an XRootD client opens for a conf's `lib =` — the rule
// `doctor` mirrors (PluginFile.h), checked against the cases the client's own
// loader distinguishes.
#include "PluginFile.h"

#include <gtest/gtest.h>

#include <set>
#include <string>

using namespace ucache;

namespace {
auto inDir(std::set<std::string> files) {
  return [files = std::move(files)](const std::string& f) { return files.count(f) > 0; };
}
} // namespace

TEST(PluginFile, VersionGoesBeforeTheLastDotOfTheFileName) {
  EXPECT_EQ(versionedPluginName("/usr/lib64/libXrdClUCache.so", 5), "/usr/lib64/libXrdClUCache-5.so");
  EXPECT_EQ(versionedPluginName("/usr/lib64/libXrdClUCache.so", 6), "/usr/lib64/libXrdClUCache-6.so");
  EXPECT_EQ(versionedPluginName("libXrdClUCache.so", 5), "libXrdClUCache-5.so");
  // A dot in a directory name is not the extension.
  EXPECT_EQ(versionedPluginName("/opt/x.y/libXrdClUCache", 6), "/opt/x.y/libXrdClUCache-6");
  // A name that already carries a version gets a second one, as the client
  // does: it then opens the name as written, whatever its own major.
  EXPECT_EQ(versionedPluginName("/l/libXrdClUCache-5.so", 6), "/l/libXrdClUCache-5-6.so");
}

TEST(PluginFile, EachClientOpensItsOwnBuild) {
  const auto installed = inDir({"/usr/lib64/libXrdClUCache-5.so", "/usr/lib64/libXrdClUCache-6.so"});
  const std::string lib = "/usr/lib64/libXrdClUCache.so";
  EXPECT_EQ(pluginFileFor(lib, 5, installed), "/usr/lib64/libXrdClUCache-5.so");
  EXPECT_EQ(pluginFileFor(lib, 6, installed), "/usr/lib64/libXrdClUCache-6.so");
  // A major with no build of its own finds nothing: the plain file is not there.
  EXPECT_EQ(pluginFileFor(lib, 7, installed), "");
}

TEST(PluginFile, PlainNameIsTheFallbackOnly) {
  const std::string lib = "/b/libXrdClUCache.so";
  // A build tree: the per-major build plus the plain name pointing at it.
  const auto tree = inDir({"/b/libXrdClUCache-5.so", lib});
  EXPECT_EQ(pluginFileFor(lib, 5, tree), "/b/libXrdClUCache-5.so");
  EXPECT_EQ(pluginFileFor(lib, 6, tree), lib); // what a 6.x client would then refuse
  // An old install with only the plain file keeps working for everyone.
  EXPECT_EQ(pluginFileFor(lib, 5, inDir({lib})), lib);
  EXPECT_EQ(pluginFileFor(lib, 5, inDir({})), "");
  EXPECT_EQ(pluginFileFor("", 5, tree), "");
}

TEST(PluginFile, NamingOneBuildPinsIt) {
  const auto installed = inDir({"/l/libXrdClUCache-5.so", "/l/libXrdClUCache-6.so"});
  EXPECT_EQ(pluginFileFor("/l/libXrdClUCache-5.so", 6, installed), "/l/libXrdClUCache-5.so");
}

TEST(PluginFile, PlainNameDropsOnlyAMajorSuffix) {
  EXPECT_EQ(plainPluginName("/l/libXrdClUCache-5.so"), "/l/libXrdClUCache.so");
  EXPECT_EQ(plainPluginName("/l/libXrdClUCache-16.so"), "/l/libXrdClUCache.so");
  EXPECT_EQ(plainPluginName("/l/libXrdClUCache.so"), "/l/libXrdClUCache.so");
  EXPECT_EQ(plainPluginName("/l/libXrdClUCache-x.so"), "/l/libXrdClUCache-x.so");
  EXPECT_EQ(plainPluginName("/l/libXrdClUCache-.so"), "/l/libXrdClUCache-.so");
  EXPECT_EQ(plainPluginName("/a-5/libXrdClUCache.so"), "/a-5/libXrdClUCache.so");
}

TEST(PluginFile, BuildsBesideAreListedByMajor) {
  const auto b = pluginBuildsBeside("/l/libXrdClUCache.so",
                                    {".", "..", "libXrdClUCache-6.so", "libXrdClUCache.so",
                                     "libXrdClUCache-5.so", "libXrdClUCache-x.so",
                                     "libXrdClUCache-5.so.bak", "libXrdClRecorder-5.so",
                                     "libXrdClUCache-.so"});
  ASSERT_EQ(b.size(), 2u);
  EXPECT_EQ(b[0], (std::pair<int, std::string>{5, "/l/libXrdClUCache-5.so"}));
  EXPECT_EQ(b[1], (std::pair<int, std::string>{6, "/l/libXrdClUCache-6.so"}));
}

TEST(PluginFile, RecognisesItsOwnName) {
  EXPECT_TRUE(isUCachePluginName("/usr/lib64/libXrdClUCache.so"));
  EXPECT_TRUE(isUCachePluginName("libXrdClUCache-6.so"));
  EXPECT_FALSE(isUCachePluginName("/usr/lib64/libXrdClRecorder-5.so"));
  EXPECT_FALSE(isUCachePluginName("/opt/libXrdClUCache/libOther.so"));
  EXPECT_FALSE(isUCachePluginName(""));
}
