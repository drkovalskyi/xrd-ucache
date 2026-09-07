// The identity uCache presents to servers at login. The composition rules are
// pure, so they are pinned here; that the strings actually reach the wire is a
// separate, environment-bound check.
#include "Announce.h"

#include <gtest/gtest.h>
#include <string>

using namespace ucache;

TEST(Announce, NamesUCacheAndKeepsTheHostProgram) {
  const Announcement a = buildAnnouncement("root.exe", "1.0.0");
  EXPECT_EQ(a.appName, "ucache");
  EXPECT_EQ(a.monInfo, "ucache/1.0.0 (root.exe)");
}

TEST(Announce, NoHostProgramLeavesJustTheVersion) {
  const Announcement a = buildAnnouncement("", "1.2.3");
  EXPECT_EQ(a.appName, "ucache");
  EXPECT_EQ(a.monInfo, "ucache/1.2.3");
}

// Naming ourselves twice says nothing.
TEST(Announce, HostProgramAlreadyNamedUCacheIsNotRepeated) {
  EXPECT_EQ(buildAnnouncement("ucache", "1.0.0").monInfo, "ucache/1.0.0");
}

// The separators of the login string. A program name is data, and data must
// not be able to invent a field: were these to survive, a program called
// `x&xrd.appname=y` would rewrite the very field beside it.
TEST(Announce, FieldSeparatorsInTheHostProgramAreNeutralized) {
  const Announcement a = buildAnnouncement("x&xrd.appname=y", "1.0.0");
  EXPECT_EQ(a.monInfo, "ucache/1.0.0 (x_xrd.appname_y)");
  EXPECT_EQ(a.monInfo.find('&'), std::string::npos);
  EXPECT_EQ(a.monInfo.find('='), std::string::npos);
}

// These strings are read back in server logs, monitoring records and
// dashboards; a newline or a control byte in any of those is somebody else's
// bug report.
TEST(Announce, ControlCharactersAndNonAsciiAreNeutralized) {
  const Announcement a = buildAnnouncement(std::string("a\nb\tc\x7f") + "\xc3\xa9", "1.0.0");
  EXPECT_EQ(a.monInfo, "ucache/1.0.0 (a_b_c___)");
}

// The whole login carries every field in about a kilobyte, so one field cannot
// be allowed to crowd out the ones beside it.
TEST(Announce, AnAbsurdHostProgramNameIsTruncated) {
  const Announcement a = buildAnnouncement(std::string(4096, 'x'), "1.0.0");
  EXPECT_EQ(a.monInfo, "ucache/1.0.0 (" + std::string(kAnnounceHostAppMax, 'x') + ")");
  EXPECT_LT(a.monInfo.size(), 128u);
}

// The name is a metric label in at least one collector, so it stays short and
// free of anything that would need quoting.
TEST(Announce, TheAnnouncedNameIsAPlainShortToken) {
  const std::string name = buildAnnouncement("root.exe", "1.0.0").appName;
  EXPECT_EQ(name, kAnnounceAppName);
  EXPECT_LT(name.size(), 16u);
  for (const char c : name)
    EXPECT_TRUE((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) << "not a plain token: " << name;
}
