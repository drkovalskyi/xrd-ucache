// Open-retry policy: decide whether a failed
// inner open is a TRANSIENT failure worth retrying, and how long to back off.
//
// Deliberately XrdCl-free — the caller passes XRootDStatus.code / .errNo as
// plain ints — so this TU compiles into the core unit-test binary without
// XrdCl. The numeric error codes it matches are the pinned XRootD 5.8.3
// ABI/wire values (verified from the XRootD source — the raw-errNo
// comparison is deliberate). UCacheFile.cc static_asserts they still equal the
// XrdCl headers, so any ABI drift breaks the build rather than silently
// misclassifying.
//
// Thread-safety: isRetryableOpen / backoffBoundMs are pure; backoffMs draws
// from a thread_local RNG.
#pragma once

#include "Config.h"

#include <cstdint>

namespace ucache {

// True iff an open that failed with XRootDStatus (statusCode, errNo) should be
// retried: a conservative allowlist of transient transport failures and the
// transient/load subset of server errors. Genuine errors (NotFound,
// NotAuthorized, bad args, ...) return false so they fail fast instead of
// stalling N x backoff.
bool isRetryableOpen(int statusCode, int errNo);

// Deterministic per-attempt backoff CEILING in ms: min(max_ms, base_ms *
// 2^(attempt-1)), attempt 1-based. Monotone non-decreasing in attempt and
// capped at max_ms. Exposed (separately from the jittered draw) so the ceiling
// is unit-testable without randomness.
uint64_t backoffBoundMs(int attempt, const Config& cfg);

// Full-jitter backoff: a uniform random draw in [0, backoffBoundMs(attempt)].
// Full jitter (not fixed/lockstep backoff) decorrelates concurrent retries so
// they do not thundering-herd an already-loaded server.
uint64_t backoffMs(int attempt, const Config& cfg);

} // namespace ucache
