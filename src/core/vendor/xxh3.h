// XXH3-64, the one hash the RNTuple container format uses — for envelope
// checksums and for per-page checksums. Original implementation for
// xrd-ucache, written to the published algorithm; no third-party code is
// copied into this tree.
//
// Scope is deliberately narrow: one-shot, unseeded, default secret. That is
// exactly what ROOT calls, and dropping the seed removes the custom-secret
// derivation along with every code path that could get it wrong. There is no
// streaming interface and no XXH32/XXH64 — add them only if something needs
// them.
//
// Correctness is pinned by tests against values ROOT itself wrote, not by
// self-consistency: a hash that is wrong but stable round-trips perfectly
// through our own reader and is rejected only by ROOT.
//
// Thread-safety: pure and reentrant. No state, no lazy initialization.
#pragma once

#include <cstddef>
#include <cstdint>

namespace ucache {

// XXH3_64bits(data, len) with the default secret and no seed.
uint64_t xxh3_64(const void* data, size_t len);

} // namespace ucache
