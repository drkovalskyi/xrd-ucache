// Measurement holdout: deciding which files are served straight from the
// origin so a run can measure what the cache is worth, from inside itself.
//
// WHY A FILE AND NOT A READ. A file processed end-to-end on one worker slot
// has an honest wall span — waits and the route's own CPU together — and spans
// compose where per-read service times do not. Splicing origin reads into an
// otherwise-cached stream measures blocked time, which overlaps compute and
// says nothing about the wall.
//
// WHY HASHED AND NOT RANDOM. The decision must be identical for every process
// and every thread that opens the same file in the same window, or one file
// would be half-cached and half-relayed and its span would measure neither
// route. Hashing the key gives that for free, with no shared state.
//
// Thread-safety: pure function of its arguments.
#pragma once

#include <cstdint>
#include <string>

namespace ucache {

// True when this key is held out for measurement in the window containing
// `nowS`. `permille` is 0..1000 (0 = never). `rotateSeconds` = 0 pins the
// selection forever; otherwise the window index joins the hash, so the held-out
// set changes each period and coverage accumulates instead of the same files
// paying every time.
bool holdoutSelected(const std::string& key, int permille, uint64_t rotateSeconds,
                     uint64_t nowS);

} // namespace ucache
