// Open-retry gate: the open-retry policy is pure and XrdCl-free, so
// it is proven here in the core unit-test binary. Truth-table for the
// classifier + bounds for the backoff ceiling and the jittered draw.
//
// The numeric codes below ARE the contract with XrdCl 5.8.3 (verified from
// source). The plugin (UCacheFile.cc) static_asserts they
// still match the XrdCl headers, so this test and the wire stay in lockstep.
#include "OpenRetry.h"

#include "Config.h"

#include <cstdint>
#include <gtest/gtest.h>

using namespace ucache;

namespace {
// XRootD status `code` values (XrdCl/XrdClStatus.hh).
constexpr int kErrSocketError = 102;
constexpr int kErrSocketTimeout = 103;
constexpr int kErrConnectionError = 108;
constexpr int kErrOperationExpired = 206;
constexpr int kErrThresholdExceeded = 208;
constexpr int kErrErrorResponse = 400;
// XRootD server error wire codes (XProtocol/XProtocol.hh).
constexpr int kXR_ArgInvalid = 3000;
constexpr int kXR_ArgMissing = 3001;
constexpr int kXR_ArgTooLong = 3002;
constexpr int kXR_FileLocked = 3003;
constexpr int kXR_FSError = 3005;
constexpr int kXR_InvalidRequest = 3006;
constexpr int kXR_IOError = 3007;
constexpr int kXR_NoSpace = 3009;
constexpr int kXR_NotAuthorized = 3010;
constexpr int kXR_NotFound = 3011;
constexpr int kXR_ServerError = 3012;
constexpr int kXR_Unsupported = 3013;
constexpr int kXR_noserver = 3014;
constexpr int kXR_isDirectory = 3016;
constexpr int kXR_Cancelled = 3017;
constexpr int kXR_ItExists = 3018;
constexpr int kXR_Overloaded = 3024;

Config retryCfg() {
  Config c;
  c.openRetryBaseMs = 200;
  c.openRetryMaxMs = 5000;
  return c;
}
} // namespace

// --- classifier truth-table -------------------------------------------------

TEST(OpenRetry, TransportFailuresRetryRegardlessOfErrno) {
  EXPECT_TRUE(isRetryableOpen(kErrConnectionError, 0));
  EXPECT_TRUE(isRetryableOpen(kErrSocketError, 0));
  EXPECT_TRUE(isRetryableOpen(kErrSocketTimeout, 0));
  EXPECT_TRUE(isRetryableOpen(kErrOperationExpired, 0));
  EXPECT_TRUE(isRetryableOpen(kErrThresholdExceeded, 0));
  // errNo is irrelevant for transport-layer codes.
  EXPECT_TRUE(isRetryableOpen(kErrConnectionError, kXR_NotFound));
}

TEST(OpenRetry, TransientServerErrorsRetry) {
  EXPECT_TRUE(isRetryableOpen(kErrErrorResponse, kXR_ItExists)); // the MIT failure
  EXPECT_TRUE(isRetryableOpen(kErrErrorResponse, kXR_Overloaded));
  EXPECT_TRUE(isRetryableOpen(kErrErrorResponse, kXR_ServerError));
  EXPECT_TRUE(isRetryableOpen(kErrErrorResponse, kXR_FSError));
  EXPECT_TRUE(isRetryableOpen(kErrErrorResponse, kXR_IOError));
  EXPECT_TRUE(isRetryableOpen(kErrErrorResponse, kXR_noserver));
  EXPECT_TRUE(isRetryableOpen(kErrErrorResponse, kXR_Cancelled));
  EXPECT_TRUE(isRetryableOpen(kErrErrorResponse, kXR_FileLocked));
}

TEST(OpenRetry, GenuineServerErrorsNeverRetry) {
  EXPECT_FALSE(isRetryableOpen(kErrErrorResponse, kXR_NotFound));
  EXPECT_FALSE(isRetryableOpen(kErrErrorResponse, kXR_NotAuthorized));
  EXPECT_FALSE(isRetryableOpen(kErrErrorResponse, kXR_Unsupported));
  EXPECT_FALSE(isRetryableOpen(kErrErrorResponse, kXR_ArgInvalid));
  EXPECT_FALSE(isRetryableOpen(kErrErrorResponse, kXR_ArgMissing));
  EXPECT_FALSE(isRetryableOpen(kErrErrorResponse, kXR_ArgTooLong));
  EXPECT_FALSE(isRetryableOpen(kErrErrorResponse, kXR_isDirectory));
  EXPECT_FALSE(isRetryableOpen(kErrErrorResponse, kXR_NoSpace));
  EXPECT_FALSE(isRetryableOpen(kErrErrorResponse, kXR_InvalidRequest));
}

TEST(OpenRetry, UnknownStatusNeverRetries) {
  EXPECT_FALSE(isRetryableOpen(0, 0));            // stOK / no error
  EXPECT_FALSE(isRetryableOpen(101, 0));          // an unlisted transport code
  EXPECT_FALSE(isRetryableOpen(kErrErrorResponse, 9999)); // unknown server errNo
}

// --- backoff ----------------------------------------------------------------

TEST(OpenRetry, BackoffBoundMonotoneAndCapped) {
  Config c = retryCfg();
  EXPECT_EQ(backoffBoundMs(1, c), 200u);
  EXPECT_EQ(backoffBoundMs(2, c), 400u);
  EXPECT_EQ(backoffBoundMs(3, c), 800u);
  EXPECT_EQ(backoffBoundMs(4, c), 1600u);
  EXPECT_EQ(backoffBoundMs(5, c), 3200u);
  EXPECT_EQ(backoffBoundMs(6, c), 5000u); // 6400 -> capped
  uint64_t prev = 0;
  for (int a = 1; a <= 20; ++a) {
    uint64_t b = backoffBoundMs(a, c);
    EXPECT_GE(b, prev) << "attempt " << a;   // monotone non-decreasing
    EXPECT_LE(b, 5000u) << "attempt " << a;  // capped
    prev = b;
  }
}

TEST(OpenRetry, BackoffDrawsWithinBound) {
  Config c = retryCfg();
  for (int a = 1; a <= 8; ++a) {
    uint64_t bound = backoffBoundMs(a, c);
    for (int i = 0; i < 2000; ++i)
      EXPECT_LE(backoffMs(a, c), bound) << "attempt " << a;
  }
}

TEST(OpenRetry, BackoffZeroConfigMeansNoSleep) {
  Config c;
  c.openRetryBaseMs = 0;
  c.openRetryMaxMs = 0;
  EXPECT_EQ(backoffBoundMs(3, c), 0u);
  EXPECT_EQ(backoffMs(3, c), 0u);
}

TEST(OpenRetry, BackoffBaseAboveCapClampsToCap) {
  Config c;
  c.openRetryBaseMs = 9000; // base already exceeds the cap
  c.openRetryMaxMs = 5000;
  EXPECT_EQ(backoffBoundMs(1, c), 5000u);
  EXPECT_EQ(backoffBoundMs(4, c), 5000u);
}
