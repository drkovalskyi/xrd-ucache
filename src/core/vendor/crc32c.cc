#include "crc32c.h"

#include <mutex>

#if defined(__x86_64__)
#include <cpuid.h>
#include <nmmintrin.h>
#define UCACHE_CRC32C_HW 1
#endif

namespace ucache {
namespace {

// CRC-32C uses the standard convention: state is inverted on entry and exit,
// so incremental composition works with plain chaining.
uint32_t table[8][256];

void initTables() {
  const uint32_t poly = 0x82F63B78u; // reflected Castagnoli
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t c = i;
    for (int k = 0; k < 8; ++k)
      c = (c & 1) ? (poly ^ (c >> 1)) : (c >> 1);
    table[0][i] = c;
  }
  for (uint32_t i = 0; i < 256; ++i)
    for (int s = 1; s < 8; ++s)
      table[s][i] = table[0][table[s - 1][i] & 0xFF] ^ (table[s - 1][i] >> 8);
}

uint32_t swCrc(uint32_t crc, const uint8_t* p, size_t len) {
  crc = ~crc;
  while (len && (reinterpret_cast<uintptr_t>(p) & 7)) {
    crc = table[0][(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    --len;
  }
  while (len >= 8) {
    uint64_t v;
    __builtin_memcpy(&v, p, 8);
    v ^= crc; // little-endian host assumed (x86_64/aarch64 Linux)
    crc = table[7][v & 0xFF] ^ table[6][(v >> 8) & 0xFF] ^ table[5][(v >> 16) & 0xFF] ^
          table[4][(v >> 24) & 0xFF] ^ table[3][(v >> 32) & 0xFF] ^
          table[2][(v >> 40) & 0xFF] ^ table[1][(v >> 48) & 0xFF] ^ table[0][(v >> 56) & 0xFF];
    p += 8;
    len -= 8;
  }
  while (len--)
    crc = table[0][(crc ^ *p++) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

#ifdef UCACHE_CRC32C_HW
bool cpuHasSse42() {
  unsigned eax, ebx, ecx, edx;
  if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx))
    return false;
  return (ecx & bit_SSE4_2) != 0;
}

__attribute__((target("sse4.2"))) uint32_t hwCrc(uint32_t crc, const uint8_t* p, size_t len) {
  uint64_t c = ~crc;
  while (len && (reinterpret_cast<uintptr_t>(p) & 7)) {
    c = _mm_crc32_u8(static_cast<uint32_t>(c), *p++);
    --len;
  }
  while (len >= 8) {
    uint64_t v;
    __builtin_memcpy(&v, p, 8);
    c = _mm_crc32_u64(c, v);
    p += 8;
    len -= 8;
  }
  while (len--)
    c = _mm_crc32_u8(static_cast<uint32_t>(c), *p++);
  return ~static_cast<uint32_t>(c);
}
#endif

bool useHw = false;
std::once_flag initFlag;

} // namespace

uint32_t crc32c(uint32_t crc, const void* data, size_t len) {
  std::call_once(initFlag, [] {
    initTables();
#ifdef UCACHE_CRC32C_HW
    useHw = cpuHasSse42();
#endif
  });
  const auto* p = static_cast<const uint8_t*>(data);
#ifdef UCACHE_CRC32C_HW
  if (useHw)
    return hwCrc(crc, p, len);
#endif
  return swCrc(crc, p, len);
}

namespace detail {

uint32_t crc32cSw(uint32_t crc, const void* data, size_t len) {
  std::call_once(initFlag, [] {
    initTables();
#ifdef UCACHE_CRC32C_HW
    useHw = cpuHasSse42();
#endif
  });
  return swCrc(crc, static_cast<const uint8_t*>(data), len);
}

uint32_t crc32cHw(uint32_t crc, const void* data, size_t len) {
#ifdef UCACHE_CRC32C_HW
  if (cpuHasSse42())
    return hwCrc(crc, static_cast<const uint8_t*>(data), len);
#endif
  (void)crc;
  (void)data;
  (void)len;
  return 0;
}

bool crc32cHwAvailable() {
#ifdef UCACHE_CRC32C_HW
  return cpuHasSse42();
#else
  return false;
#endif
}

} // namespace detail

} // namespace ucache
