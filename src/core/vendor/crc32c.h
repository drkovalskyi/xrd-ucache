// Vendored CRC-32C (Castagnoli, polynomial 0x1EDC6F41, reflected 0x82F63B78)
// — public-domain-style original implementation for xrd-ucache.
// Hardware path where the target has one (SSE4.2 on x86_64, the CRC32 extension
// on AArch64), software slice-by-8 fallback everywhere else.
//
// Thread-safety: all functions are pure/reentrant after the first call
// (one-time table/CPU-feature initialization is guarded).
#pragma once

#include <cstddef>
#include <cstdint>

namespace ucache {

// Incremental: pass the previous return value as `crc` to continue.
// Initial call uses crc = 0.
uint32_t crc32c(uint32_t crc, const void* data, size_t len);

inline uint32_t crc32c(const void* data, size_t len) { return crc32c(0, data, len); }

// Testing hooks: force one implementation (both must agree on the standard
// check vectors regardless of the host CPU).
namespace detail {
uint32_t crc32cSw(uint32_t crc, const void* data, size_t len);
uint32_t crc32cHw(uint32_t crc, const void* data, size_t len); // 0-filled stub if no hw path
bool crc32cHwAvailable();
} // namespace detail

} // namespace ucache
