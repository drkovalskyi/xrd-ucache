#include "vendor/xxh3.h"

#include "TestUtil.h"
#include <gtest/gtest.h>

#include <vector>

using namespace ucache;

namespace {

// A byte ramp, so the vectors below can be regenerated from any reference
// implementation without shipping an input file.
std::vector<uint8_t> ramp(size_t n) {
  std::vector<uint8_t> b(n);
  for (size_t i = 0; i < n; ++i) b[i] = (uint8_t)(i & 0xFF);
  return b;
}

struct Vector {
  size_t len;
  uint64_t digest;
};

// Known answers from the reference implementation. The lengths are chosen to
// straddle every boundary in the algorithm — 16/17 (short to medium), 128/129
// and 240/241 (the two medium regimes), and 1024/1025, where the long path
// starts a second block and scrambles the accumulators for the first time.
const Vector kVectors[] = {
    {0, 0x2d06800538d394c2ull},    {1, 0xc44bdff4074eecdbull},
    {2, 0xd6645fc3051a9457ull},    {3, 0x5f4299fc161c9cbbull},
    {4, 0x60dab036a58211f2ull},    {5, 0xb075753a84ca0fbeull},
    {8, 0x3a1c2d7c85af88f8ull},    {9, 0xe9612598145bb9dcull},
    {12, 0x5ace6a511c10894bull},   {16, 0x8355e3a6f61770dbull},
    {17, 0x9ef341a99de37328ull},   {31, 0x4f36db8e4df378fdull},
    {32, 0x3523581fe96e4c05ull},   {33, 0xe68c56ba88991e58ull},
    {64, 0x6187eb9089b0ed55ull},   {65, 0x6928c76ce90422d0ull},
    {96, 0x278a3e12ea046dfbull},   {97, 0xe7220282dc4e14f4ull},
    {128, 0x85c6174c7ff4c46bull},  {129, 0xec7642b431ba3e5aull},
    {160, 0x5bea9075ec9401b8ull},  {240, 0x375a384d957fe865ull},
    {241, 0x02e8cd95421c6d02ull},  {256, 0x9408a4433b952d71ull},
    {1023, 0x26a0a01d1c925c06ull}, {1024, 0xa870f92984398d22ull},
    {1025, 0x78c86e91ee939852ull}, {2048, 0xdd420471ff96bd00ull},
    {4096, 0xeb4b7c3707879151ull}, {5000, 0x1b74bda2c82a8c7aull},
};

} // namespace

TEST(Xxh3, KnownAnswers) {
  const auto buf = ramp(8192);
  for (const auto& v : kVectors) {
    EXPECT_EQ(xxh3_64(buf.data(), v.len), v.digest) << "length " << v.len;
  }
}

TEST(Xxh3, EmptyInputIsNotZero) {
  // A zero here would be the signature of a stub that never ran.
  EXPECT_NE(xxh3_64("", 0), 0ull);
  EXPECT_EQ(xxh3_64(nullptr, 0), kVectors[0].digest);
}

TEST(Xxh3, SensitiveToEveryByteAndToLength) {
  // Negative control for the known answers: if the hash ignored input, or
  // truncated it, the vectors above would still pass while the format broke.
  auto buf = ramp(4096);
  const uint64_t base = xxh3_64(buf.data(), buf.size());
  for (size_t i : {size_t(0), size_t(1), size_t(63), size_t(64), size_t(1023), size_t(2048),
                   size_t(4094), size_t(4095)}) {
    auto flipped = buf;
    flipped[i] ^= 0x01;
    EXPECT_NE(xxh3_64(flipped.data(), flipped.size()), base) << "byte " << i << " ignored";
  }
  EXPECT_NE(xxh3_64(buf.data(), buf.size() - 1), base);
}

TEST(Xxh3, LengthIsPartOfTheHash) {
  // Zero-filled buffers of different lengths must not collide: the page and
  // envelope checksums rely on length being mixed in, not just content.
  std::vector<uint8_t> zeros(300, 0);
  EXPECT_NE(xxh3_64(zeros.data(), 8), xxh3_64(zeros.data(), 16));
  EXPECT_NE(xxh3_64(zeros.data(), 64), xxh3_64(zeros.data(), 128));
  EXPECT_NE(xxh3_64(zeros.data(), 129), xxh3_64(zeros.data(), 240));
}

TEST(Xxh3, UnalignedInputMatchesAligned) {
  // Pages are hashed straight out of a read buffer at arbitrary offsets.
  auto buf = ramp(2048);
  for (size_t shift : {size_t(1), size_t(3), size_t(7)}) {
    std::vector<uint8_t> shifted(buf.begin(), buf.end());
    shifted.insert(shifted.begin(), shift, 0xAA);
    EXPECT_EQ(xxh3_64(shifted.data() + shift, buf.size()), xxh3_64(buf.data(), buf.size()));
  }
}
