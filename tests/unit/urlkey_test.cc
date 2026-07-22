#include "UrlKey.h"

#include <gtest/gtest.h>

using namespace ucache;

TEST(UrlKey, NormalizesCaseSlashesAndPort) {
  auto k = UrlKey::parse("root://EOSpublic.CERN.ch//eos//opendata/./cms/file.root");
  ASSERT_TRUE(k);
  EXPECT_EQ(k->key, "root://eospublic.cern.ch:1094/eos/opendata/cms/file.root");
  EXPECT_EQ(k->host, "eospublic.cern.ch");
  EXPECT_EQ(k->scheme, "root");
  EXPECT_EQ(k->hashHex.size(), 64u);
}

TEST(UrlKey, ExplicitPortPreserved) {
  auto k = UrlKey::parse("root://h.example:2094//a/b");
  ASSERT_TRUE(k);
  EXPECT_EQ(k->key, "root://h.example:2094/a/b");
}

TEST(UrlKey, DotDotResolution) {
  auto k = UrlKey::parse("root://h//a/b/../c/../../d");
  ASSERT_TRUE(k);
  EXPECT_EQ(k->key, "root://h:1094/d");
  k = UrlKey::parse("root://h//../..//x");
  ASSERT_TRUE(k);
  EXPECT_EQ(k->key, "root://h:1094/x");
}

TEST(UrlKey, StripsAllCgiByDefault) {
  auto k = UrlKey::parse("root://h//f.root?authz=SECRET&xrd.wantprot=unix");
  ASSERT_TRUE(k);
  EXPECT_EQ(k->key, "root://h:1094/f.root");
  EXPECT_EQ(k->key.find("SECRET"), std::string::npos);
  // Same file with different tokens => same cache object.
  auto k2 = UrlKey::parse("root://h//f.root?authz=OTHER");
  ASSERT_TRUE(k2);
  EXPECT_EQ(k->hashHex, k2->hashHex);
}

TEST(UrlKey, KeepCgiWhitelistSortedCanonical) {
  std::vector<std::string> keep = {"zkey", "akey"};
  auto k = UrlKey::parse("root://h//f?zkey=1&authz=SECRET&akey=2", keep);
  ASSERT_TRUE(k);
  EXPECT_EQ(k->key, "root://h:1094/f?akey=2&zkey=1");
  // Order-insensitive: same params, different order => same key.
  auto k2 = UrlKey::parse("root://h//f?akey=2&zkey=1&authz=X", keep);
  ASSERT_TRUE(k2);
  EXPECT_EQ(k->hashHex, k2->hashHex);
}

TEST(UrlKey, UserinfoDropped) {
  auto k = UrlKey::parse("root://user@h//f");
  ASSERT_TRUE(k);
  EXPECT_EQ(k->key, "root://h:1094/f");
}

TEST(UrlKey, Ipv6) {
  auto k = UrlKey::parse("root://[::1]:2094//f");
  ASSERT_TRUE(k);
  EXPECT_EQ(k->key, "root://[::1]:2094/f");
  EXPECT_EQ(k->host, "[::1]");
}

TEST(UrlKey, ParseFailures) {
  EXPECT_FALSE(UrlKey::parse("not a url"));
  EXPECT_FALSE(UrlKey::parse("root://"));
  EXPECT_FALSE(UrlKey::parse("root://hostonly"));
  EXPECT_FALSE(UrlKey::parse("://h//f"));
  EXPECT_FALSE(UrlKey::parse("root://[::1//f"));
}

TEST(UrlKey, ObjectPaths) {
  auto k = UrlKey::parse("root://h//f");
  ASSERT_TRUE(k);
  std::string shard = k->hashHex.substr(0, 2);
  EXPECT_EQ(k->objectDir("/c"), "/c/objects/" + shard);
  EXPECT_EQ(k->dataPath("/c"), "/c/objects/" + shard + "/" + k->hashHex + ".data");
  EXPECT_EQ(k->metaPath("/c"), "/c/objects/" + shard + "/" + k->hashHex + ".meta");
}

TEST(UrlKey, HostGlobs) {
  EXPECT_TRUE(hostMatchesAny({"*.cern.ch"}, "eospublic.cern.ch"));
  EXPECT_TRUE(hostMatchesAny({"exact.host"}, "EXACT.host"));
  EXPECT_FALSE(hostMatchesAny({"*.cern.ch"}, "cern.ch"));
  EXPECT_FALSE(hostMatchesAny({}, "anything"));
  EXPECT_TRUE(hostMatchesAny({"a", "b", "eos*"}, "eospublic.cern.ch"));
}

// The plugin's own cache-fill opens strip client routing directives:
// XrdAdaptor's tried=/triedrc= exclusion list forced the
// redirector onto unreachable data servers. Everything else must survive
// byte-for-byte (auth tokens in particular).
TEST(UrlKey, StripCgiParams) {
  const std::vector<std::string> kRouting = {"tried", "triedrc"};
  EXPECT_EQ(stripCgiParams("root://h//f.root", kRouting), "root://h//f.root");
  EXPECT_EQ(stripCgiParams("root://h//f.root?tried=a,b&triedrc=resel", kRouting),
            "root://h//f.root");
  EXPECT_EQ(stripCgiParams("root://h//f.root?authz=TOK&tried=a&xrd.wantprot=unix", kRouting),
            "root://h//f.root?authz=TOK&xrd.wantprot=unix");
  EXPECT_EQ(stripCgiParams("root://h//f.root?tried=a&authz=TOK", kRouting),
            "root://h//f.root?authz=TOK");
  // 'triedx' is NOT 'tried': exact key match only.
  EXPECT_EQ(stripCgiParams("root://h//f.root?triedx=1", kRouting),
            "root://h//f.root?triedx=1");
}
