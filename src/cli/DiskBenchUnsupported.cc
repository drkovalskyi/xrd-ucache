// Stand-in for the storage self-test on platforms that cannot run it.
//
// The measurement is built out of interfaces that only Linux offers, and not
// incidentally: O_DIRECT is what keeps the page cache out of a disk number,
// /proc/diskstats is what separates device operations from the syscalls that
// issued them, and /proc/self/mountinfo plus /sys/block are what let a record
// name the device it describes. Elsewhere the nearest equivalents are weaker in
// kind, not just in detail — a caching hint instead of a guarantee, no
// device-level counters at all.
//
// The published JSON keys are parsed by other tools and compared across
// machines, so emitting them from a different, quieter measurement would put
// numbers that are not comparable under names that say they are. This declines
// instead, and says which part of the tool is missing. Nothing else in the CLI
// depends on the platform.
#include "DiskBench.h"

#include <cstdio>

namespace ucache {

int runDiskBench(const std::vector<std::string>& /*paths*/, const DiskBenchOpts& /*opts*/) {
  std::fputs("bench: the storage self-test is not available on this platform.\n"
             "       It measures with O_DIRECT and reads per-device counters, which this\n"
             "       system does not provide; a substitute measurement would print the same\n"
             "       field names for numbers that cannot be compared with the ones this\n"
             "       command exists to produce.\n"
             "       Every other subcommand works normally. To size a cache here, measure a\n"
             "       real workload: run your analysis twice and compare `ucache stats`.\n",
             stderr);
  return 2;
}

} // namespace ucache
