#include "xxh3.h"

#include <cstring>

namespace ucache {
namespace {

// Algorithm constants. The primes and the 192-byte default secret are facts of
// the format — any other values produce a hash ROOT will not accept.
constexpr uint32_t kPrime32_1 = 0x9E3779B1u;
constexpr uint32_t kPrime32_2 = 0x85EBCA77u;
constexpr uint32_t kPrime32_3 = 0xC2B2AE3Du;
constexpr uint64_t kPrime64_1 = 0x9E3779B185EBCA87ull;
constexpr uint64_t kPrime64_2 = 0xC2B2AE3D27D4EB4Full;
constexpr uint64_t kPrime64_3 = 0x165667B19E3779F9ull;
constexpr uint64_t kPrime64_4 = 0x85EBCA77C2B2AE63ull;
constexpr uint64_t kPrime64_5 = 0x27D4EB2F165667C5ull;
constexpr uint64_t kPrimeMx1 = 0x165667919E3779F9ull;
constexpr uint64_t kPrimeMx2 = 0x9FB21C651E98DF25ull;

constexpr size_t kSecretSize = 192;
constexpr size_t kStripeLen = 64;
constexpr size_t kSecretConsumeRate = 8;
constexpr size_t kAccNb = 8;
// Offsets into the secret used by the long-input path. Named because an
// off-by-one here still hashes, it just hashes to the wrong value.
constexpr size_t kSecretMergeAccsStart = 11;
constexpr size_t kSecretLastAccStart = 7;
constexpr size_t kMidsizeStartOffset = 3;
constexpr size_t kMidsizeLastOffset = 17;
constexpr size_t kSecretSizeMin = 136;

const uint8_t kSecret[kSecretSize] = {
    0xb8, 0xfe, 0x6c, 0x39, 0x23, 0xa4, 0x4b, 0xbe, 0x7c, 0x01, 0x81, 0x2c, 0xf7, 0x21, 0xad, 0x1c,
    0xde, 0xd4, 0x6d, 0xe9, 0x83, 0x90, 0x97, 0xdb, 0x72, 0x40, 0xa4, 0xa4, 0xb7, 0xb3, 0x67, 0x1f,
    0xcb, 0x79, 0xe6, 0x4e, 0xcc, 0xc0, 0xe5, 0x78, 0x82, 0x5a, 0xd0, 0x7d, 0xcc, 0xff, 0x72, 0x21,
    0xb8, 0x08, 0x46, 0x74, 0xf7, 0x43, 0x24, 0x8e, 0xe0, 0x35, 0x90, 0xe6, 0x81, 0x3a, 0x26, 0x4c,
    0x3c, 0x28, 0x52, 0xbb, 0x91, 0xc3, 0x00, 0xcb, 0x88, 0xd0, 0x65, 0x8b, 0x1b, 0x53, 0x2e, 0xa3,
    0x71, 0x64, 0x48, 0x97, 0xa2, 0x0d, 0xf9, 0x4e, 0x38, 0x19, 0xef, 0x46, 0xa9, 0xde, 0xac, 0xd8,
    0xa8, 0xfa, 0x76, 0x3f, 0xe3, 0x9c, 0x34, 0x3f, 0xf9, 0xdc, 0xbb, 0xc7, 0xc7, 0x0b, 0x4f, 0x1d,
    0x8a, 0x51, 0xe0, 0x4b, 0xcd, 0xb4, 0x59, 0x31, 0xc8, 0x9f, 0x7e, 0xc9, 0xd9, 0x78, 0x73, 0x64,
    0xea, 0xc5, 0xac, 0x83, 0x34, 0xd3, 0xeb, 0xc3, 0xc5, 0x81, 0xa0, 0xff, 0xfa, 0x13, 0x63, 0xeb,
    0x17, 0x0d, 0xdd, 0x51, 0xb7, 0xf0, 0xda, 0x49, 0xd3, 0x16, 0x55, 0x26, 0x29, 0xd4, 0x68, 0x9e,
    0x2b, 0x16, 0xbe, 0x58, 0x7d, 0x47, 0xa1, 0xfc, 0x8f, 0xf8, 0xb8, 0xd1, 0x7a, 0xd0, 0x31, 0xce,
    0x45, 0xcb, 0x3a, 0x8f, 0x95, 0x16, 0x04, 0x28, 0xaf, 0xd7, 0xfb, 0xca, 0xbb, 0x4b, 0x40, 0x7e,
};

// Loads are little-endian and alignment-agnostic; memcpy compiles to a single
// instruction on both architectures we build for.
inline uint32_t rd32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, sizeof(v));
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
  v = __builtin_bswap32(v);
#endif
  return v;
}

inline uint64_t rd64(const uint8_t* p) {
  uint64_t v;
  std::memcpy(&v, p, sizeof(v));
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
  v = __builtin_bswap64(v);
#endif
  return v;
}

inline uint64_t swap64(uint64_t v) { return __builtin_bswap64(v); }

inline uint64_t rotl64(uint64_t v, int r) { return (v << r) | (v >> (64 - r)); }

// Folded 64x64 -> 128 multiply: the two halves xored together.
inline uint64_t mul128Fold64(uint64_t a, uint64_t b) {
#if defined(__SIZEOF_INT128__)
  __uint128_t product = (__uint128_t)a * (__uint128_t)b;
  return (uint64_t)product ^ (uint64_t)(product >> 64);
#else
  const uint64_t lo_lo = (a & 0xFFFFFFFFull) * (b & 0xFFFFFFFFull);
  const uint64_t hi_lo = (a >> 32) * (b & 0xFFFFFFFFull);
  const uint64_t lo_hi = (a & 0xFFFFFFFFull) * (b >> 32);
  const uint64_t hi_hi = (a >> 32) * (b >> 32);
  const uint64_t cross = (lo_lo >> 32) + (hi_lo & 0xFFFFFFFFull) + lo_hi;
  const uint64_t upper = (hi_lo >> 32) + (cross >> 32) + hi_hi;
  const uint64_t lower = (cross << 32) | (lo_lo & 0xFFFFFFFFull);
  return lower ^ upper;
#endif
}

inline uint64_t xorshift64(uint64_t v, int shift) { return v ^ (v >> shift); }

inline uint64_t avalanche(uint64_t h) {
  h = xorshift64(h, 37);
  h *= kPrimeMx1;
  h = xorshift64(h, 32);
  return h;
}

// The older XXH64 finalizer, still used by the 0- and 1-to-3-byte cases.
inline uint64_t avalanche64(uint64_t h) {
  h ^= h >> 33;
  h *= kPrime64_2;
  h ^= h >> 29;
  h *= kPrime64_3;
  h ^= h >> 32;
  return h;
}

inline uint64_t rrmxmx(uint64_t h, uint64_t len) {
  h ^= rotl64(h, 49) ^ rotl64(h, 24);
  h *= kPrimeMx2;
  h ^= (h >> 35) + len;
  h *= kPrimeMx2;
  return xorshift64(h, 28);
}

// Unseeded throughout: seed 0 collapses the "+ seed" / "- seed" terms, which
// is why none of them appear below.
inline uint64_t mix16B(const uint8_t* in, const uint8_t* secret) {
  return mul128Fold64(rd64(in) ^ rd64(secret), rd64(in + 8) ^ rd64(secret + 8));
}

uint64_t len0to16(const uint8_t* in, size_t len) {
  if (len > 8) {
    const uint64_t lo = rd64(in) ^ (rd64(kSecret + 24) ^ rd64(kSecret + 32));
    const uint64_t hi = rd64(in + len - 8) ^ (rd64(kSecret + 40) ^ rd64(kSecret + 48));
    const uint64_t acc = len + swap64(lo) + hi + mul128Fold64(lo, hi);
    return avalanche(acc);
  }
  if (len >= 4) {
    const uint64_t in1 = rd32(in);
    const uint64_t in2 = rd32(in + len - 4);
    const uint64_t bitflip = rd64(kSecret + 8) ^ rd64(kSecret + 16);
    return rrmxmx((in2 + (in1 << 32)) ^ bitflip, len);
  }
  if (len != 0) {
    const uint32_t combined = ((uint32_t)in[0] << 16) | ((uint32_t)in[len >> 1] << 24) |
                              ((uint32_t)in[len - 1] << 0) | ((uint32_t)len << 8);
    return avalanche64((uint64_t)combined ^ (rd32(kSecret) ^ rd32(kSecret + 4)));
  }
  return avalanche64(rd64(kSecret + 56) ^ rd64(kSecret + 64));
}

uint64_t len17to128(const uint8_t* in, size_t len) {
  uint64_t acc = len * kPrime64_1;
  if (len > 32) {
    if (len > 64) {
      if (len > 96) {
        acc += mix16B(in + 48, kSecret + 96);
        acc += mix16B(in + len - 64, kSecret + 112);
      }
      acc += mix16B(in + 32, kSecret + 64);
      acc += mix16B(in + len - 48, kSecret + 80);
    }
    acc += mix16B(in + 16, kSecret + 32);
    acc += mix16B(in + len - 32, kSecret + 48);
  }
  acc += mix16B(in, kSecret);
  acc += mix16B(in + len - 16, kSecret + 16);
  return avalanche(acc);
}

uint64_t len129to240(const uint8_t* in, size_t len) {
  uint64_t acc = len * kPrime64_1;
  const size_t rounds = len / 16;
  for (size_t i = 0; i < 8; ++i) acc += mix16B(in + 16 * i, kSecret + 16 * i);
  acc = avalanche(acc);
  for (size_t i = 8; i < rounds; ++i)
    acc += mix16B(in + 16 * i, kSecret + 16 * (i - 8) + kMidsizeStartOffset);
  acc += mix16B(in + len - 16, kSecret + kSecretSizeMin - kMidsizeLastOffset);
  return avalanche(acc);
}

void accumulate512(uint64_t* acc, const uint8_t* in, const uint8_t* secret) {
  for (size_t i = 0; i < kAccNb; ++i) {
    const uint64_t data = rd64(in + 8 * i);
    const uint64_t key = data ^ rd64(secret + 8 * i);
    acc[i ^ 1] += data; // adjacent lanes are swapped on purpose
    acc[i] += (key & 0xFFFFFFFFull) * (key >> 32);
  }
}

void scramble(uint64_t* acc, const uint8_t* secret) {
  for (size_t i = 0; i < kAccNb; ++i) {
    acc[i] = (xorshift64(acc[i], 47) ^ rd64(secret + 8 * i)) * kPrime32_1;
  }
}

uint64_t mergeAccs(const uint64_t* acc, const uint8_t* secret, uint64_t start) {
  uint64_t result = start;
  for (size_t i = 0; i < 4; ++i) {
    result += mul128Fold64(acc[2 * i] ^ rd64(secret + 16 * i), acc[2 * i + 1] ^ rd64(secret + 16 * i + 8));
  }
  return avalanche(result);
}

uint64_t hashLong(const uint8_t* in, size_t len) {
  uint64_t acc[kAccNb] = {kPrime32_3, kPrime64_1, kPrime64_2, kPrime64_3,
                          kPrime64_4, kPrime32_2, kPrime64_5, kPrime32_1};

  const size_t stripesPerBlock = (kSecretSize - kStripeLen) / kSecretConsumeRate;
  const size_t blockLen = kStripeLen * stripesPerBlock;
  const size_t blocks = (len - 1) / blockLen;

  for (size_t n = 0; n < blocks; ++n) {
    for (size_t s = 0; s < stripesPerBlock; ++s) {
      accumulate512(acc, in + n * blockLen + s * kStripeLen, kSecret + s * kSecretConsumeRate);
    }
    scramble(acc, kSecret + kSecretSize - kStripeLen);
  }

  // Trailing partial block, then the final stripe, which always covers the
  // last 64 bytes even if that re-reads bytes already consumed.
  const size_t stripes = ((len - 1) - blockLen * blocks) / kStripeLen;
  for (size_t s = 0; s < stripes; ++s) {
    accumulate512(acc, in + blocks * blockLen + s * kStripeLen, kSecret + s * kSecretConsumeRate);
  }
  accumulate512(acc, in + len - kStripeLen, kSecret + kSecretSize - kStripeLen - kSecretLastAccStart);

  return mergeAccs(acc, kSecret + kSecretMergeAccsStart, (uint64_t)len * kPrime64_1);
}

} // namespace

uint64_t xxh3_64(const void* data, size_t len) {
  const uint8_t* in = static_cast<const uint8_t*>(data);
  if (len <= 16) return len0to16(in, len);
  if (len <= 128) return len17to128(in, len);
  if (len <= 240) return len129to240(in, len);
  return hashLong(in, len);
}

} // namespace ucache
