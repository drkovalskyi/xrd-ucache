// See uCache work: read the same data from the server and from the cache, and
// compare.
//
//   root -l -b -q ucache_try.C
//   root -l -b -q 'ucache_try.C("root://host//path/file.root", "Events", "Muon_pt")'
//
// Three reads, each in a fresh ROOT process so that none inherits another's
// connection or compiled code: the first fills the cache; the second reads
// from the server with uCache switched off (UCACHE_DISABLE=1), after the first
// has warmed the server, so it is the fairest read the server can give; the
// third is served from the cache. With no arguments it reads one branch of a
// public CMS open-data file. One branch on purpose: uCache reads a job that
// takes most of a large file straight from the server, without caching it.

#include <ROOT/RDataFrame.hxx>
#include <TError.h>
#include <TH1D.h>
#include <TROOT.h>
#include <TString.h>
#include <TSystem.h>

#include <chrono>
#include <cstdio>
#include <sstream>
#include <string>

namespace {
struct Pass {
  bool ok;
  double seconds, entries, mean;
};

Pass readOnce(const char* url, const char* tree, const char* branch) {
  const auto t0 = std::chrono::steady_clock::now();
  ROOT::RDataFrame df(tree, url);
  auto h = df.Histo1D(branch);
  const double entries = h->GetEntries(); // runs the event loop
  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  return {true, seconds, entries, h->GetMean()};
}

// One read in a fresh ROOT process, with `env` in front of it.
Pass readInChild(const char* env, const char* macro, const char* url, const char* tree,
                 const char* branch) {
  const TString cmd = TString::Format("%s '%s/root.exe' -l -b -q '%s(\"%s\",\"%s\",\"%s\",1)'", env,
                                      TROOT::GetBinDir().Data(), macro, url, tree, branch);
  std::istringstream out(gSystem->GetFromPipe(cmd).Data());
  for (std::string line; std::getline(out, line);) {
    Pass p{true, 0, 0, 0};
    if (std::sscanf(line.c_str(), "UCACHE_TRY %lf %lf %lf", &p.seconds, &p.entries, &p.mean) == 3)
      return p;
  }
  return {false, 0, 0, 0};
}
} // namespace

void ucache_try(const char* url = "root://eospublic.cern.ch//eos/opendata/cms/Run2016H/SingleMuon/"
                                  "NANOAOD/UL2016_MiniAODv2_NanoAODv9-v1/130000/"
                                  "5CA4CE73-629C-2F48-939C-4274B369F112.root",
                const char* tree = "Events", const char* branch = "Muon_pt", int child = 0) {
  gErrorIgnoreLevel = kError; // CMS files carry metadata classes ROOT warns it cannot read
  if (child) {
    const Pass p = readOnce(url, tree, branch);
    std::printf("UCACHE_TRY %.6f %.0f %.17g\n", p.seconds, p.entries, p.mean);
    return;
  }
  std::printf("uCache try: branch %s of %s in\n  %s\n\n", branch, tree, url);
  std::printf("  read 1 of 3 ...\n");
  const Pass fill = readInChild("", __FILE__, url, tree, branch);
  std::printf("  read 2 of 3 ...\n");
  const Pass direct = readInChild("UCACHE_DISABLE=1", __FILE__, url, tree, branch);
  std::printf("  read 3 of 3 ...\n");
  const Pass warm = readInChild("", __FILE__, url, tree, branch);
  if (!fill.ok || !direct.ok || !warm.ok) {
    std::printf("\n  A read failed; its messages are above.\n");
    return;
  }

  const bool same = fill.entries == warm.entries && fill.mean == warm.mean &&
                    direct.entries == warm.entries && direct.mean == warm.mean;
  const double gain = warm.seconds > 0 ? direct.seconds / warm.seconds : 0;
  std::printf("\n  1. with uCache, first read (fetches, fills the cache): %6.1f s\n", fill.seconds);
  std::printf("  2. uCache off, straight from the server:               %6.1f s\n", direct.seconds);
  std::printf("  3. with uCache, from the cache:                        %6.1f s   %.1fx faster than 2\n",
              warm.seconds, gain);
  std::printf("\n  Read 2 came right after read 1, when the server had just served the same\n"
              "  data: the best case for reading without the cache. Read 1 is usually the\n"
              "  slowest: the server had not served it just before, and it also writes the cache.\n");
  std::printf("  Same result every time: %s (%.0f values, mean %g)\n", same ? "yes" : "NO",
              warm.entries, warm.mean);
  if (gain < 1.2)
    std::printf("\n  No gain. If uCache is on (`ucache doctor`), reading is not what this job\n"
                "  waits for here: the server is close, or the job computes more than it reads.\n");
}
