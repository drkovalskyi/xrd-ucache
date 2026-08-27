#include "Holdout.h"

namespace ucache {

BypassPhase bypassPhase(uint64_t nowS, uint64_t windowSeconds, int dutyPermille) {
  BypassPhase p;
  if (windowSeconds == 0 || dutyPermille <= 0)
    return p;
  if (dutyPermille > 1000)
    dutyPermille = 1000;
  // A cycle is sized so that ONE window of length `windowSeconds` is the bypass
  // share of it: duty 200 per mille and a 120 s window give a 600 s cycle whose
  // first 120 s bypass. Sizing the window rather than the cycle is deliberate --
  // the window length is what has to clear the transient, and it must not shrink
  // when someone lowers the duty.
  const uint64_t cycle = windowSeconds * 1000ull / static_cast<uint64_t>(dutyPermille);
  if (cycle == 0)
    return p;
  p.window = nowS / windowSeconds;
  p.bypass = (nowS % cycle) < windowSeconds;
  return p;
}

} // namespace ucache
