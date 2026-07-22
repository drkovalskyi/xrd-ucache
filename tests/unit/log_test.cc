#include "Log.h"

#include "TestUtil.h"
#include <fstream>
#include <gtest/gtest.h>

using namespace ucache;
using test::TempDir;

TEST(Log, LevelsAndFileOutput) {
  TempDir td;
  std::string path = td.path() + "/log.txt";
  Log::configure("info:" + path);
  EXPECT_TRUE(Log::enabled(LogLevel::kError));
  EXPECT_TRUE(Log::enabled(LogLevel::kWarn));
  EXPECT_TRUE(Log::enabled(LogLevel::kInfo));
  EXPECT_FALSE(Log::enabled(LogLevel::kDebug));
  UCACHE_ERROR("err %d", 1);
  UCACHE_WARN("warn %s", "two");
  UCACHE_INFO("info");
  UCACHE_DEBUG("suppressed");

  Log::configure("debug:" + path);
  EXPECT_TRUE(Log::enabled(LogLevel::kDebug));
  UCACHE_DEBUG("now visible");
  Log::configure("error:" + path);
  EXPECT_FALSE(Log::enabled(LogLevel::kWarn));
  // Oversized message is clamped, not overflowed.
  std::string big(5000, 'x');
  UCACHE_ERROR("%s", big.c_str());

  std::ifstream in(path);
  std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  EXPECT_NE(all.find("ERROR] err 1"), std::string::npos);
  EXPECT_NE(all.find("WARN] warn two"), std::string::npos);
  EXPECT_NE(all.find("INFO] info"), std::string::npos);
  EXPECT_EQ(all.find("suppressed"), std::string::npos);
  EXPECT_NE(all.find("now visible"), std::string::npos);
  // Reset level for the rest of the test binary (fd stays on the temp file;
  // writes to an unlinked fd are harmless — documented leak in Log.cc).
  Log::configure("warn");
}
