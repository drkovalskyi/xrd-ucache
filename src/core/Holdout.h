// Measurement bypass: deciding WHEN the cache steps aside so a run can measure
// what it is worth, from inside itself.
//
// WHY TIME AND NOT FILES. Wall time is per-item cost divided by how much of that
// cost overlaps, and the two routes overlap differently -- measured, by more
// than they differ in cost -- measured, not assumed. A measurement therefore has
// to span a period in which the WHOLE application used one route;
// only then does its wall contain the overlap term. Splitting files between
// routes cannot do that at any fraction: each population is measured while the
// other route carries most of the load. Splitting TIME can.
//
// A file keeps the route it was opened with for its whole life -- switching
// mid-file would measure neither route -- so after each switch the population is
// mixed until the files in flight drain. Windows must be several times a file's
// span, and a consumer must discard files that straddle a boundary.
//
// Thread-safety: pure function of its arguments.
#pragma once

#include <cstdint>
#include <string>

namespace ucache {

// Which measurement window `nowS` falls in, and whether the cache steps aside
// during it. `windowSeconds` = 0 disables measurement entirely.
//
// The cycle is one bypass window followed by cache windows, repeating, with
// `dutyPermille` of each cycle spent bypassing. Deriving it from the wall clock
// (rather than from process start) means every process in a multi-process job
// switches together -- otherwise one process would be bypassing while another
// cached, and neither window would be pure.
struct BypassPhase {
  bool bypass = false;   // serve from the origin, do not cache
  uint64_t window = 0;   // window index, so records can be grouped
};
BypassPhase bypassPhase(uint64_t nowS, uint64_t windowSeconds, int dutyPermille);

} // namespace ucache
