// The XrdCl timeout parameter type, which differs across client major versions.
//
// XRootD 5 declares every FilePlugIn method's timeout as uint16_t; XRootD 6
// widened it to time_t. An override whose signature does not match the base
// class exactly overrides nothing, so hardcoding either type makes the plugin
// silently unimplemented against the other client — the compiler reports one
// error per method and none of them names the cause.
//
// Selected from the client's own version header rather than a build option, so
// a build cannot disagree with the headers it is compiled against.
//
// Use this alias for the zero we pass to the SYNCHRONOUS File::Open too, not a
// bare literal: v6 overloads Open as both sync (url, flags, mode, timeout) and
// async (url, flags, mode, handler, timeout = 0), so a four-argument call is
// ambiguous when the fourth argument converts equally well to a pointer and to
// the timeout — which is what `static_cast<uint16_t>(0)` did. Cast to this
// alias and the sync overload is an exact match, which wins outright.
#pragma once

#include <XrdVersion.hh>

#include <cstdint>
#include <ctime>

namespace ucache {

// XrdVNUMBER is xyyzz — major, minor, bugfix — so 60000 is the 6.0.0 boundary.
// XrdVNUMUNK (1000000) marks an unreleased client and compares above it, which
// is the right side to land on: unreleased code is newer than 6.0.0.
#if defined(XrdVNUMBER) && XrdVNUMBER >= 60000
using XrdTimeout = time_t;
#else
using XrdTimeout = uint16_t;
#endif

}  // namespace ucache
