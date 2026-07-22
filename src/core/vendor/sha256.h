// Vendored SHA-256 (FIPS 180-4) — public-domain-style original implementation
// for xrd-ucache object naming (objects/<aa>/<sha256(key)>).
// Not used for security; collision resistance for cache keying only.
//
// Thread-safety: pure functions, no global state.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace ucache {

std::array<uint8_t, 32> sha256(const void* data, size_t len);

// Lowercase hex of sha256(s).
std::string sha256Hex(const std::string& s);

} // namespace ucache
