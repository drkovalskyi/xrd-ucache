// Envelope framing for the RNTuple parser. Hermetic: an envelope is a 64-bit
// word (16-bit type, 48-bit length), a payload, and a trailing xxh3-64, so a
// valid one can be built here without a ROOT file.
//
// The parser itself is checked against ROOT's descriptor on real files, which
// is the only way to catch a reader that is self-consistently wrong. These
// tests cover what that comparison cannot: what happens to a MALFORMED
// envelope, which no healthy file contains.
#include "RNTupleMeta.h"

#include <gtest/gtest.h>

#include <cstring>
#include <vector>

using namespace ucache::transpose;

namespace {

std::vector<uint8_t> makeEnvelope(uint16_t type, size_t payloadBytes) {
  const size_t n = 8 + payloadBytes + 8;
  std::vector<uint8_t> e(n, 0);
  const uint64_t word = (uint64_t)type | ((uint64_t)n << 16);
  for (size_t i = 0; i < 8; ++i) e[i] = (uint8_t)((word >> (8 * i)) & 0xFF);
  for (size_t i = 0; i < payloadBytes; ++i) e[8 + i] = (uint8_t)(i * 7 + 1);
  sealEnvelope(e.data(), e.size());
  return e;
}

} // namespace

TEST(RNTupleEnvelope, SealedEnvelopeVerifies) {
  std::string why;
  for (size_t payload : {size_t(0), size_t(1), size_t(64), size_t(4096)}) {
    auto e = makeEnvelope(kEnvelopeHeader, payload);
    EXPECT_TRUE(verifyEnvelope(e.data(), e.size(), kEnvelopeHeader, why))
        << "payload " << payload << ": " << why;
  }
}

TEST(RNTupleEnvelope, WrongTypeRejected) {
  auto e = makeEnvelope(kEnvelopeFooter, 128);
  std::string why;
  EXPECT_FALSE(verifyEnvelope(e.data(), e.size(), kEnvelopePageList, why));
  EXPECT_NE(why.find("type"), std::string::npos) << why;
  // ...and the same bytes verify against the type they actually carry, so the
  // rejection is about the type and not about the envelope being broken.
  EXPECT_TRUE(verifyEnvelope(e.data(), e.size(), kEnvelopeFooter, why)) << why;
}

TEST(RNTupleEnvelope, LengthFieldMustMatchTheBuffer) {
  // A declared length that disagrees with the bytes on hand is the shape a
  // truncated or over-long read takes; it must not be parsed on trust.
  auto e = makeEnvelope(kEnvelopeHeader, 128);
  std::string why;
  EXPECT_FALSE(verifyEnvelope(e.data(), e.size() - 1, kEnvelopeHeader, why));
  EXPECT_NE(why.find("length"), std::string::npos) << why;
}

TEST(RNTupleEnvelope, AnyFlippedByteFailsTheChecksum) {
  std::string why;
  for (size_t at : {size_t(0), size_t(8), size_t(100), size_t(135)}) {
    auto e = makeEnvelope(kEnvelopeHeader, 128);
    e[at] ^= 0x01;
    EXPECT_FALSE(verifyEnvelope(e.data(), e.size(), kEnvelopeHeader, why)) << "byte " << at;
  }
}

TEST(RNTupleEnvelope, ResealAfterEditing) {
  // The writer patches an envelope in place and re-seals it; that has to
  // produce something the reader accepts, or a rewritten file is unreadable.
  auto e = makeEnvelope(kEnvelopePageList, 256);
  e[100] ^= 0xFF;
  std::string why;
  ASSERT_FALSE(verifyEnvelope(e.data(), e.size(), kEnvelopePageList, why));
  sealEnvelope(e.data(), e.size());
  EXPECT_TRUE(verifyEnvelope(e.data(), e.size(), kEnvelopePageList, why)) << why;
}

TEST(RNTupleEnvelope, TooShortIsRejectedNotRead) {
  std::string why;
  std::vector<uint8_t> tiny(8, 0);
  EXPECT_FALSE(verifyEnvelope(tiny.data(), tiny.size(), kEnvelopeHeader, why));
}

TEST(RNTupleParse, RefusesNonRNTupleInputWithoutCrashing) {
  auto m = parseRNTuple("/nonexistent/path/file.root", "Events");
  EXPECT_FALSE(m.error.empty());
  EXPECT_TRUE(m.ranges.empty());
}
