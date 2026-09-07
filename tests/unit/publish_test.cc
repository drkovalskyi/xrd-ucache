// The publish module's contract with the report service: the hashes, the
// path normalization and the field-level redaction must produce the same bytes
// as the service's own reference implementation, or records published from the
// CLI and records pasted into the web page would land as different disks and
// different machines. The expected strings below were produced by that
// reference implementation with the test identity the service's own tests use.
#include "Publish.h"

#include "TestUtil.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <ctime>
#include <fstream>

#include <sys/stat.h>
#include <unistd.h>

using namespace ucache;

namespace {

const char* kTestIdentity = "ucache-id:aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa:0123456789abcdef0123456789abcdef";
const char* kSalt = "0123456789abcdef0123456789abcdef";

// A raw record as `ucache bench` prints it: every field the redaction has an
// opinion about is present, with values that would be a leak if they went out.
const char* kRawBench =
    R"({"schema":1,"host":"node-1.example.org","path":"/scratch/user//cache/./","fs":"xfs",)"
    R"("mode":"O_DIRECT","file_mb":1024,"randr1_iops":7494,"randr1_us_p50":133,"randr16_iops":73863,)"
    R"("seq_read_mbps":512.3,"time":"2026-09-05T10:00:00Z","version":"1.0.0",)"
    R"("cmd":"/opt/ucache/bin/ucache bench --size 64g --phase-seconds 60 --streams 1,16,32 --log /home/u/x.txt /scratch/user/cache",)"
    R"("kernel":"5.14.0-503.34.1.el9_5.x86_64","mount":"/scratch/","mount_fstype":"xfs",)"
    R"("mount_source":"nfs-server.example.org:/export/scratch","mount_opts":"rw,relatime",)"
    R"("mount_super_opts":"rw,seclabel","dev":"8:17","dev_name":"sdb1","dev_model":"Samsung SSD 870",)"
    R"("dev_rotational":0,"std_qds":[1,16,32],"cachepath_fill_curve":[1.5,2.0,2.5]})";

struct ScopedEnv {
  std::string name, old;
  bool had;
  ScopedEnv(const char* n, const std::string& value) : name(n) {
    const char* o = ::getenv(n);
    had = o != nullptr;
    if (had)
      old = o;
    ::setenv(n, value.c_str(), 1);
  }
  ~ScopedEnv() {
    if (had)
      ::setenv(name.c_str(), old.c_str(), 1);
    else
      ::unsetenv(name.c_str());
  }
};

} // namespace

TEST(Publish, IdentityParsesAndRoundTrips) {
  Identity id;
  ASSERT_TRUE(parseIdentity(kTestIdentity, id));
  EXPECT_EQ(id.owner, "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
  EXPECT_EQ(id.salt, kSalt);
  EXPECT_EQ(id.text(), kTestIdentity);
  EXPECT_TRUE(parseIdentity(std::string("  ") + kTestIdentity + "\n", id)); // whitespace tolerated
  EXPECT_FALSE(parseIdentity("ucache-id:not-a-uuid:0123456789abcdef0123456789abcdef", id));
  EXPECT_FALSE(parseIdentity("ucache-id:aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa:short", id));
  EXPECT_FALSE(parseIdentity("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa:0123456789abcdef0123456789abcdef", id));
  EXPECT_FALSE(parseIdentity("ucache-id:AAAAAAAA-aaaa-4aaa-8aaa-aaaaaaaaaaaa:0123456789abcdef0123456789abcdef", id));
}

TEST(Publish, NewIdentityIsWellFormedAndRandom) {
  const Identity a = newIdentity(), b = newIdentity();
  Identity parsed;
  ASSERT_TRUE(parseIdentity(a.text(), parsed));
  EXPECT_TRUE(looksLikeUuid(a.owner));
  EXPECT_EQ(a.owner[14], '4'); // version nibble
  EXPECT_TRUE(a.owner[19] == '8' || a.owner[19] == '9' || a.owner[19] == 'a' || a.owner[19] == 'b');
  EXPECT_EQ(a.salt.size(), 32u);
  EXPECT_NE(a.owner, b.owner);
  EXPECT_NE(a.salt, b.salt);
}

TEST(Publish, HashesMatchTheServiceReference) {
  // Reference values from the service's redact module with this salt.
  EXPECT_EQ(machineHash(kSalt, "Node-1.example.org "), "472f25c59a7204767d5d866e7aac359d");
  EXPECT_EQ(locationHash(kSalt, "node-1.example.org", "/scratch/user//cache/./"),
            "385c4438c397c96755cb418f2a154bfe");
  EXPECT_EQ(volumeHash(kSalt, "node-1.example.org", "/scratch/"), "00e74f7f596ef35a4b4e4484cbbbe581");
  EXPECT_EQ(locationHash(kSalt, "h", "a/../b/c"), "e2de35c323590abd11598362ed4802f8");
  // The three facts the model rests on: host is part of a location, a volume
  // is not a location, and case/whitespace in the hostname do not matter.
  EXPECT_NE(locationHash(kSalt, "node-a", "/data/x/cache"), locationHash(kSalt, "node-b", "/data/x/cache"));
  EXPECT_NE(volumeHash(kSalt, "node-a", "/data"), locationHash(kSalt, "node-a", "/data"));
  EXPECT_EQ(machineHash(kSalt, " Node-A.example.org "), machineHash(kSalt, "node-a.example.org"));
  EXPECT_NE(locationHash(kSalt, "node-a", "/data/x/cache"),
            locationHash("ffffffffffffffffffffffffffffffff", "node-a", "/data/x/cache"));
  // The host is lowercased and trimmed for EVERY id, not only the machine's:
  // a mixed-case hostname (macOS) must hash like the web page hashes it.
  EXPECT_EQ(locationHash(kSalt, " Node-1.EXAMPLE.org ", "/scratch/user//cache/./"),
            "385c4438c397c96755cb418f2a154bfe");
  EXPECT_EQ(volumeHash(kSalt, "NODE-1.example.ORG", "/scratch/"), "00e74f7f596ef35a4b4e4484cbbbe581");
}

TEST(Publish, LabelProblem) {
  EXPECT_EQ(labelProblem("scratch SSD"), "");
  EXPECT_EQ(labelProblem("   "), "");
  EXPECT_EQ(labelProblem(std::string(80, 'x')), "");
  EXPECT_NE(labelProblem(std::string(81, 'x')), "");
  EXPECT_NE(labelProblem("--url"), "");
  EXPECT_NE(labelProblem("cache on /home/alice"), "");
  EXPECT_NE(labelProblem("~/cache"), "");
  EXPECT_NE(labelProblem("nfs1:/export"), "");
  EXPECT_NE(labelProblem("alice@node"), "");
  EXPECT_NE(labelProblem("C:\\cache"), "");
}

TEST(Publish, NormPathIsPythonsNormpath) {
  EXPECT_EQ(normPath("/a//b/./c/"), "/a/b/c");
  EXPECT_EQ(normPath("a/../b"), "b");
  EXPECT_EQ(normPath("/../x"), "/x");
  EXPECT_EQ(normPath(""), ".");
  EXPECT_EQ(normPath("."), ".");
  EXPECT_EQ(normPath("/"), "/");
  EXPECT_EQ(normPath("a/b/../../.."), "..");
  EXPECT_EQ(normPath("//x"), "//x");
  EXPECT_EQ(normPath("///x"), "/x");
  EXPECT_EQ(normPath("/scratch/"), "/scratch");
}

TEST(Publish, NormalizeCmdKeepsParametersAndLosesPlaces) {
  EXPECT_EQ(normalizeCmd("ucache bench --size 64g --phase-seconds 60 --streams 1,16,32 --log /home/u/x.txt /data/cache"),
            "ucache bench --size 64g --phase-seconds 60 --streams 1,16,32 <path>");
  EXPECT_EQ(normalizeCmd("/opt/ucache/bin/ucache bench --log=~/b.txt --size=2g . --threads 8"),
            "ucache bench --size=2g . --threads 8");
  EXPECT_EQ(normalizeCmd("python3 bench.py --out=/tmp/x"), "<exe> bench.py --out=<path>");
  EXPECT_EQ(normalizeCmd("ucache bench --threads 32 --fill writers=4,block=48k ~/cache"),
            "ucache bench --threads 32 --fill writers=4,block=48k <path>");
  // The two cases the service's own tests pin.
  EXPECT_EQ(normalizeCmd("/usr/local/bin/ucache bench --size 64g --phase-seconds 60 --streams 1,16,32 "
                         "/data/x/cache --log /x/y.txt"),
            "ucache bench --size 64g --phase-seconds 60 --streams 1,16,32 <path>");
  EXPECT_EQ(normalizeCmd("ucache bench --log=/tmp/l.txt --cache-path=/data/c ~/cache"),
            "ucache bench --cache-path=<path> <path>");
  EXPECT_EQ(normalizeCmd(""), "");
}

TEST(Publish, RegistrableDomainAndUrlCoarsening) {
  EXPECT_EQ(registrableDomain("eoscms.cern.ch"), "cern.ch");
  EXPECT_EQ(registrableDomain("xrootd-cms.infn.it"), "infn.it");
  EXPECT_EQ(registrableDomain("cmsxrootd.fnal.gov"), "fnal.gov");
  EXPECT_EQ(registrableDomain("red-gridftp.unl.edu"), "unl.edu");
  EXPECT_EQ(registrableDomain("x.y.ac.uk"), "y.ac.uk");
  EXPECT_EQ(registrableDomain("a.b.co.jp"), "b.co.jp");
  EXPECT_EQ(registrableDomain("f.q.example.com."), "example.com");
  EXPECT_EQ(registrableDomain("localhost"), "localhost");
  EXPECT_EQ(registrableDomain("127.0.0.1"), "unknown");
  EXPECT_EQ(registrableDomain("::1"), "unknown");
  EXPECT_EQ(registrableDomain(""), "unknown");
  EXPECT_EQ(coarsenRootUrl("root://eoscms.cern.ch:1094//eos/cms/store/x.root"), "root://cern.ch");
  EXPECT_EQ(coarsenRootUrl("root://xrootd-cms.infn.it//store/y"), "root://infn.it");
  EXPECT_EQ(coarsenRootUrl("root://eos.site.ac.uk:1094//eos/x/y.root"), "root://site.ac.uk");
  EXPECT_EQ(coarsenRootUrl("root://user@[::1]:1094//x"), "root://unknown");
  EXPECT_EQ(coarsenRootUrl("eoscms.cern.ch"), "root://cern.ch");           // bare host: root assumed
  EXPECT_EQ(coarsenRootUrl("//eoscms.cern.ch/x"), "root://cern.ch");
  EXPECT_EQ(coarsenRootUrl("eoscms.cern.ch//eos/x"), "root://unknown");    // no authority: a path
  EXPECT_EQ(coarsenRootUrl("/data/user/file.root"), "root://unknown");      // a local path names a place
  EXPECT_EQ(coarsenRootUrl("https://cmsxrootd.fnal.gov/x"), "https://fnal.gov");
}

TEST(Publish, DatesOnly) {
  // UTC by construction: a local-time implementation would pass on a UTC
  // runner and fail east of it, so the test pins a far-east zone.
  ScopedEnv tz("TZ", "Asia/Tokyo");
  ::tzset();
  EXPECT_EQ(dateOnlyUtc(1725494400), "2024-09-05");
  EXPECT_EQ(dateOnlyUtc(1757030399), "2025-09-04");
  EXPECT_EQ(dateOnlyUtc(1788000727), "2026-08-29");
}

TEST(Publish, DatesRefuseATimestampTheyCannotFormat) {
  // Reachable, not hypothetical: a run-file name is parsed into a uint64 with
  // no range check, so a corrupt stats directory reaches this. Both the
  // library calls can fail here, and glibc leaves the buffer unterminated when
  // strftime overflows — returning it read past the array and put stack bytes
  // in the payload. An empty answer is the only honest one.
  EXPECT_EQ(dateOnlyUtc(32000000000000000ull), "");   // a ten-digit year: strftime overflows
  EXPECT_EQ(dateOnlyUtc(999999999999999999ull), "");  // gmtime_r fails outright
  EXPECT_EQ(dateOnlyUtc(9223372036854775807ull), "");
  EXPECT_EQ(dateOnlyUtc(1788000727), "2026-08-29");   // and the ordinary case still works
}

TEST(Publish, TextUnderAnUnknownKeyNeverLeaves) {
  // The record grows as the tool learns to measure more, and a new field
  // arrives already published. `cachepath_error` carried the benchmarked
  // directory and the pid in its text and was on no drop list. The rule is
  // therefore an allowlist over STRINGS: numbers pass, unvouched text does not.
  Json r = Json::object();
  r.set("host", Json::string("node-1.example.org"));
  r.set("randr1_iops", Json::integer(7494));
  r.set("dev_name", Json::string("dm-0"));
  r.set("dev_model", Json::string("Samsung SSD 870"));
  r.set("mount_fstype", Json::string("xfs"));
  r.set("cachepath_error", Json::string("mkdir /scratch/u/cache/.ucache-bench.4242/ucache-path: No space left on device"));
  r.set("dev_dm_name", Json::string("almalinux_node1-home"));
  r.set("cachepath_stalls", Json::integer(3));
  const Json out = redactBench(r, nullptr, "");
  const std::string text = out.dump();
  EXPECT_FALSE(out.has("cachepath_error")) << text;
  EXPECT_FALSE(out.has("dev_dm_name")) << text;   // the LVM name is <distro>_<hostname>
  EXPECT_EQ(text.find("/scratch"), std::string::npos) << text;
  EXPECT_EQ(text.find("almalinux_node1"), std::string::npos) << text;
  // and the numbers and the vouched-for text are all still there
  EXPECT_EQ(out.num("randr1_iops"), 7494);
  EXPECT_EQ(out.num("cachepath_stalls"), 3);
  EXPECT_EQ(out.str("dev_name"), "dm-0");
  EXPECT_EQ(out.str("dev_model"), "Samsung SSD 870");
  EXPECT_EQ(out.str("mount_fstype"), "xfs");
  EXPECT_EQ(out.str("host"), "");
}

TEST(Publish, ANumericArrayUnderAnUnknownKeyStillPasses) {
  Json r = Json::object();
  Json arr;
  ASSERT_TRUE(Json::parse("[1.5,2.0,2.5]", arr));
  r.set("cachepath_fill_curve", arr);
  Json strs;
  ASSERT_TRUE(Json::parse(R"(["/scratch/u","/eos/x"])", strs));
  r.set("some_future_paths", strs);
  const Json out = redactBench(r, nullptr, "");
  EXPECT_TRUE(out.has("cachepath_fill_curve")); // numbers are not a leak
  EXPECT_FALSE(out.has("some_future_paths"));   // text nested in an array is
  EXPECT_EQ(out.dump().find("/scratch"), std::string::npos);
}

TEST(Publish, JsonRoundTripKeepsNumberTextAndOrder) {
  Json j;
  std::string err;
  ASSERT_TRUE(Json::parse(kRawBench, j, &err)) << err;
  ASSERT_TRUE(j.isObject());
  EXPECT_EQ(j.obj.front().first, "schema");
  EXPECT_EQ(j.str("host"), "node-1.example.org");
  EXPECT_EQ(j.get("seq_read_mbps")->s, "512.3"); // text preserved, not re-rounded
  EXPECT_DOUBLE_EQ(j.num("randr1_iops"), 7494.0);
  EXPECT_EQ(j.dump(), std::string(kRawBench));
  // escapes, unicode, nesting, whitespace
  Json k;
  ASSERT_TRUE(Json::parse(R"( {"a": "x\"y\\z\né😀", "b": [1, -2.5e3, true, null, {}], "c": {}} )", k, &err))
      << err;
  EXPECT_EQ(k.str("a"), "x\"y\\z\n\xC3\xA9\xF0\x9F\x98\x80");
  EXPECT_EQ(k.get("b")->arr.size(), 5u);
  EXPECT_EQ(k.get("b")->arr[1].s, "-2.5e3");
  EXPECT_EQ(k.dump(), "{\"a\":\"x\\\"y\\\\z\\n\xC3\xA9\xF0\x9F\x98\x80\",\"b\":[1,-2.5e3,true,null,{}],\"c\":{}}");
  Json bad;
  EXPECT_FALSE(Json::parse("{\"a\":}", bad, &err));
  EXPECT_FALSE(Json::parse("{\"a\":1} x", bad, &err));
  EXPECT_FALSE(Json::parse("", bad, &err));
  // pretty form parses back to the same value
  Json again;
  ASSERT_TRUE(Json::parse(j.dump(2), again, &err)) << err;
  EXPECT_EQ(again.dump(), j.dump());
}

TEST(Publish, JsonUnicodeEscapesNumbersAndHostileInput) {
  Json k;
  std::string err;
  ASSERT_TRUE(Json::parse(R"("é€😀")", k, &err)) << err;
  EXPECT_EQ(k.s, "\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80");
  ASSERT_TRUE(Json::parse(R"("\ud83dA")", k, &err)) << err; // broken pair: U+FFFD, then the A survives
  EXPECT_EQ(k.s, "\xEF\xBF\xBD" "A");
  ASSERT_TRUE(Json::parse(R"("\ude00x")", k, &err)) << err;     // a low surrogate on its own
  EXPECT_EQ(k.s, "\xEF\xBF\xBDx");
  ASSERT_TRUE(Json::parse(R"("\ud83d")", k, &err)) << err;      // a high surrogate at the end
  EXPECT_EQ(k.s, "\xEF\xBF\xBD");
  // Nesting is bounded: a hostile record-store line fails, it does not
  // overflow the stack.
  EXPECT_FALSE(Json::parse(std::string(200000, '['), k, &err));
  EXPECT_NE(err.find("nesting"), std::string::npos);
  EXPECT_TRUE(Json::parse(std::string(60, '[') + std::string(60, ']'), k, &err)) << err;
  // The number grammar is JSON's, not strtod's.
  EXPECT_FALSE(Json::parse("01", k, &err));
  EXPECT_FALSE(Json::parse("1.", k, &err));
  EXPECT_FALSE(Json::parse("-", k, &err));
  EXPECT_FALSE(Json::parse(".5", k, &err));
  EXPECT_FALSE(Json::parse("1e", k, &err));
  ASSERT_TRUE(Json::parse("-0.5e-3", k, &err));
  EXPECT_EQ(k.s, "-0.5e-3");
  ASSERT_TRUE(Json::parse("0", k, &err));
  // The pretty printer closes a mixed array on its own line, and it parses back.
  Json m;
  ASSERT_TRUE(Json::parse("[1,{\"a\":2}]", m, &err));
  EXPECT_EQ(m.dump(1), "[\n 1,\n {\n  \"a\": 2\n }\n]");
  Json back;
  ASSERT_TRUE(Json::parse(m.dump(1), back, &err)) << err;
  EXPECT_EQ(back.dump(), m.dump());
}

TEST(Publish, RedactBenchDropsBlanksHashesAndNormalizes) {
  Json raw;
  ASSERT_TRUE(Json::parse(kRawBench, raw));
  Identity id;
  ASSERT_TRUE(parseIdentity(kTestIdentity, id));
  const Json r = redactBench(raw, &id, "00000000-0000-4000-8000-000000000001");
  // Named here, not iterated from the array: shortening kBenchDropKeys must fail this.
  for (const char* k : {"path", "mount", "mount_source", "mount_opts", "mount_super_opts"})
    EXPECT_FALSE(r.has(k)) << k;
  EXPECT_EQ(r.str("dev_name"), "sdb1"); // the block device is hardware, not a place
  EXPECT_EQ(r.str("host"), "");
  EXPECT_EQ(r.str("cmd"), "ucache bench --size 64g --phase-seconds 60 --streams 1,16,32 <path>");
  EXPECT_EQ(r.str("location"), "385c4438c397c96755cb418f2a154bfe"); // host + normpath(path)
  EXPECT_EQ(r.str("volume"), "00e74f7f596ef35a4b4e4484cbbbe581");   // host + normpath(mount)
  EXPECT_EQ(r.str("install_id"), "00000000-0000-4000-8000-000000000001");
  // What is kept is kept verbatim, and in its original order.
  EXPECT_EQ(r.str("mount_fstype"), "xfs");
  EXPECT_EQ(r.str("dev_model"), "Samsung SSD 870");
  EXPECT_EQ(r.get("randr16_iops")->s, "73863");
  EXPECT_EQ(r.obj.front().first, "schema");
  const std::string text = r.dump();
  for (const char* leak : {"/scratch", "/home/", "example.org", "nfs-server", "--log", "relatime", "seclabel"})
    EXPECT_EQ(text.find(leak), std::string::npos) << leak;
  // Without an identity: no hashes, still no leaks.
  const Json anon = redactBench(raw, nullptr);
  EXPECT_FALSE(anon.has("location"));
  EXPECT_FALSE(anon.has("volume"));
  EXPECT_FALSE(anon.has("install_id"));
  EXPECT_EQ(anon.str("host"), "");
  EXPECT_EQ(anon.dump().find("/scratch"), std::string::npos);
}

TEST(Publish, RedactNetbench) {
  Json raw;
  ASSERT_TRUE(Json::parse(R"({"schema":1,"host":"node-1.example.org","url":"root://eoscms.cern.ch:1094//eos/cms/store/x.root",)"
                          R"("block_kb":4,"seconds":10.0,"mode":"origin","file_mb":2048,)"
                          R"("streams":[{"n":1,"iops":147,"mbps":0.6,"p50_us":6100,"p95_us":25000,"p99_us":45000}]})",
                          raw));
  const Json r = redactNetbench(raw);
  EXPECT_EQ(r.str("host"), "");
  EXPECT_EQ(r.str("url"), "root://cern.ch");
  EXPECT_EQ(r.str("mode"), "origin");
  EXPECT_EQ(r.get("streams")->arr.size(), 1u);
  EXPECT_EQ(r.get("streams")->arr[0].get("iops")->s, "147");
}

TEST(Publish, MachineBlockHasHardwareAndHashedId) {
  Identity id;
  ASSERT_TRUE(parseIdentity(kTestIdentity, id));
  ASSERT_FALSE(hostName().empty()); // the "never appears" check below would be vacuous otherwise
  const Json m = machineBlock(&id, "v5.8.3", "1.0.0");
  EXPECT_EQ(m.str("id"), machineHash(kSalt, hostName()));
  EXPECT_EQ(m.str("id").size(), 32u);
  EXPECT_FALSE(m.str("os").empty());
  EXPECT_FALSE(m.str("arch").empty());
  EXPECT_FALSE(m.str("kernel").empty());
  EXPECT_GT(m.num("ncpu"), 0);
  EXPECT_GT(m.num("mem_gb"), 0);
  EXPECT_EQ(m.str("xrootd_client"), "v5.8.3");
  EXPECT_EQ(m.str("ucache_version"), "1.0.0");
  EXPECT_EQ(m.dump().find(hostName()), std::string::npos); // the hostname itself never appears
  const Json anon = machineBlock(nullptr, "", "1.0.0");
  EXPECT_FALSE(anon.has("id"));
  EXPECT_FALSE(anon.has("xrootd_client"));
}

TEST(Publish, PayloadAndPlaceholders) {
  Identity id;
  ASSERT_TRUE(parseIdentity(kTestIdentity, id));
  Json raw;
  ASSERT_TRUE(Json::parse(kRawBench, raw));
  PayloadParts parts;
  parts.identity = id;
  parts.installId = "12345678-1234-4123-8123-123456789abc";
  parts.location = locationHash(kSalt, "node-1.example.org", "/scratch/user/cache");
  parts.volume = volumeHash(kSalt, "node-1.example.org", "/scratch");
  parts.label = "SSD scratch";
  parts.machine = machineBlock(&id, "", "1.0.0");
  parts.bench.push_back(redactBench(raw, &id, parts.installId));
  parts.ucacheVersion = "1.0.0";
  const Json p = buildPayload(parts);
  EXPECT_EQ(p.get("schema")->s, "1");
  EXPECT_EQ(p.str("kind"), "publish");
  EXPECT_EQ(p.str("owner_id"), id.owner);
  EXPECT_EQ(p.str("install_id"), parts.installId);
  EXPECT_EQ(p.str("label"), "SSD scratch");
  EXPECT_EQ(p.get("bench")->arr.size(), 1u);
  EXPECT_EQ(p.get("netbench")->arr.size(), 0u);
  EXPECT_FALSE(p.has("history"));
  const Json d = withPlaceholderIds(p);
  EXPECT_EQ(d.str("owner_id"), "00000000-0000-4000-8000-000000000000");
  EXPECT_EQ(d.str("install_id"), "00000000-0000-4000-8000-000000000001");
  EXPECT_EQ(d.str("location"), std::string(32, '1'));
  EXPECT_EQ(d.str("volume"), std::string(32, '2'));
  EXPECT_EQ(d.get("machine")->str("id"), std::string(32, '0'));
  EXPECT_EQ(d.get("bench")->arr[0].str("location"), std::string(32, '1'));
  EXPECT_EQ(d.get("bench")->arr[0].str("install_id"), "00000000-0000-4000-8000-000000000001");
  // the placeholders replaced every real identifier
  const std::string text = d.dump();
  EXPECT_EQ(text.find(id.owner), std::string::npos);
  EXPECT_EQ(text.find(parts.installId), std::string::npos);
  EXPECT_EQ(text.find(parts.location), std::string::npos);
  // the original is untouched
  EXPECT_EQ(p.str("owner_id"), id.owner);
  const std::string desc = describePayload(p);
  EXPECT_NE(desc.find("1 benchmark record"), std::string::npos);
  EXPECT_NE(desc.find("SSD scratch"), std::string::npos);
}

TEST(Publish, IdentityFileAndRecordStoreUnderAPrivateHome) {
  test::TempDir home;
  ScopedEnv h("HOME", home.path());
  ScopedEnv xc("XDG_CONFIG_HOME", home.path() + "/cfg");
  ScopedEnv xd("XDG_DATA_HOME", home.path() + "/data");
  ScopedEnv f1("UCACHE_IDENTITY_FILE", "");
  ScopedEnv f2("UCACHE_RECORDS_FILE", "");
  ::unsetenv("UCACHE_IDENTITY_FILE");
  ::unsetenv("UCACHE_RECORDS_FILE");

  EXPECT_EQ(identityPath(), home.path() + "/cfg/ucache/identity");
  EXPECT_EQ(recordsPath(), home.path() + "/data/ucache/records.jsonl");
  EXPECT_FALSE(loadIdentity().has_value());
  EXPECT_FALSE(ensureIdentity(false).has_value()); // asked not to create
  bool created = false;
  auto id = ensureIdentity(true, &created);
  ASSERT_TRUE(id.has_value());
  EXPECT_TRUE(created);
  struct ::stat st;
  ASSERT_EQ(::stat(identityPath().c_str(), &st), 0);
  EXPECT_EQ(st.st_mode & 0777, 0600u);
  auto again = ensureIdentity(true, &created);
  ASSERT_TRUE(again.has_value());
  EXPECT_FALSE(created);
  EXPECT_EQ(again->text(), id->text());
  // --set installs a given string
  Identity given;
  ASSERT_TRUE(parseIdentity(kTestIdentity, given));
  EXPECT_EQ(saveIdentity(given), 0);
  EXPECT_EQ(loadIdentity()->text(), kTestIdentity);

  // records: raw lines in, parsed records out, labels and marks alongside
  EXPECT_EQ(appendRecordLine(std::string("ucache-bench-json: ") + kRawBench), 0);
  EXPECT_EQ(appendRecordLine("ucache-netbench-json: {\"schema\":1,\"host\":\"node-1.example.org\",\"url\":\"root://a.b//c\",\"mode\":\"origin\",\"streams\":[]}"), 0);
  EXPECT_EQ(appendRecordLine("not a record line"), 0);
  EXPECT_EQ(rememberLabel("Node-1.example.org", "/scratch/user/cache/", "SSD scratch"), 0);
  EXPECT_EQ(markPublished("bench", "https://example.invalid/s/abc"), 0);
  ASSERT_EQ(::stat(recordsPath().c_str(), &st), 0);
  EXPECT_EQ(st.st_mode & 0777, 0600u);
  const auto recs = loadRecords();
  ASSERT_EQ(recs.size(), 4u);
  EXPECT_EQ(recs[0].kind, "bench");
  EXPECT_EQ(recs[0].body.str("path"), "/scratch/user//cache/./");
  EXPECT_EQ(recs[1].kind, "netbench");
  EXPECT_EQ(recs[2].kind, "label");
  EXPECT_EQ(recs[3].kind, "published");
  EXPECT_EQ(knownLabel(recs, "node-1.example.org", "/scratch/user//cache/./"), "SSD scratch");
  EXPECT_EQ(knownLabel(recs, "node-2.example.org", "/scratch/user/cache"), "");
}

TEST(Publish, InstallIdIsMintedOnceAndSurvivesReuse) {
  test::TempDir dir;
  EXPECT_EQ(installId(dir.path() + "/does-not-exist", true), "");
  EXPECT_EQ(installId(dir.path(), false), ""); // not created unless asked
  bool fresh = false;
  const std::string a = installId(dir.path(), true, &fresh);
  EXPECT_TRUE(looksLikeUuid(a));
  EXPECT_FALSE(fresh); // written, so not fresh on the next call either
  const std::string b = installId(dir.path(), true, &fresh);
  EXPECT_EQ(a, b);
  EXPECT_FALSE(fresh);
  EXPECT_EQ(installId(dir.path(), false), a);
  std::ifstream f(dir.path() + "/install-id");
  std::string onDisk;
  std::getline(f, onDisk);
  EXPECT_EQ(onDisk, a);
  // a garbage file is replaced, not trusted
  std::ofstream(dir.path() + "/install-id") << "garbage\n";
  const std::string c = installId(dir.path(), true, &fresh);
  EXPECT_TRUE(looksLikeUuid(c));
  EXPECT_NE(c, a);
}

TEST(Publish, AnInstanceIdIsNeverReplacedByARace) {
  // Two publishes starting at once both find no id and both try to write one.
  // A rename-over would give one cache directory two instance ids and two
  // pages; the loser adopts the winner's file instead.
  test::TempDir dir;
  const std::string mine = installId(dir.path(), true);
  ASSERT_TRUE(looksLikeUuid(mine));
  // stand in for the other process having won the race: the file is already
  // there when this call goes to create it
  EXPECT_EQ(installId(dir.path(), true), mine);
  // present but unreadable: minting a replacement would orphan the page this
  // cache already publishes to, so it publishes unlinked and says so
  ASSERT_EQ(::chmod((dir.path() + "/install-id").c_str(), 0), 0);
  const std::string none = installId(dir.path(), true);
  ASSERT_EQ(::chmod((dir.path() + "/install-id").c_str(), 0644), 0);
  if (::geteuid() != 0) { // root reads it anyway
    EXPECT_EQ(none, "");
    EXPECT_EQ(installId(dir.path(), true), mine); // and the file is untouched
  }
}

TEST(Publish, ASecondEnsureIdentityAdoptsTheFileRatherThanMintingAgain) {
  // The invariant, not the race: two calls in one process see each other's
  // file, so the second adopts it. This passes on the code that had the race
  // too — an interleaving between the read and the write cannot be produced
  // sequentially — so the race itself is gated where it can be: six
  // concurrent publishes in the integration gate, which showed six owner
  // pages before the fix. Kept because the adopt path is what that fix
  // relies on, and a change to it should be seen here first.
  test::TempDir dir;
  ScopedEnv f("UCACHE_IDENTITY_FILE", dir.path() + "/identity");
  bool created = false;
  const auto first = ensureIdentity(true, &created);
  ASSERT_TRUE(first.has_value());
  EXPECT_TRUE(created);
  // a second publish that also found no identity a moment ago
  created = true;
  const auto second = ensureIdentity(true, &created);
  ASSERT_TRUE(second.has_value());
  EXPECT_FALSE(created);
  EXPECT_EQ(second->text(), first->text()); // same owner AND same salt
}

TEST(Publish, ServiceUrlPrecedence) {
  ScopedEnv e("UCACHE_PUBLISH_URL", "");
  ::unsetenv("UCACHE_PUBLISH_URL");
  EXPECT_EQ(serviceUrl(""), kDefaultServiceUrl);
  EXPECT_EQ(serviceUrl("http://127.0.0.1:8080/"), "http://127.0.0.1:8080");
  ::setenv("UCACHE_PUBLISH_URL", "https://test.example/", 1);
  EXPECT_EQ(serviceUrl(""), "https://test.example");
  EXPECT_EQ(serviceUrl("http://flag"), "http://flag"); // the flag wins over the environment
}
