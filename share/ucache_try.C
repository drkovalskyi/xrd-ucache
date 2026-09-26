// See uCache work: read the same data from the server and from the cache, and
// compare.
//
//   root -l -b -q ucache_try.C
//   root -l -b -q 'ucache_try.C("root://host//path/file.root", "Events", "Muon_pt")'
//
// The file is first removed from the cache (`ucache rm`), then read three
// times, each in a fresh ROOT process so that none inherits another's
// connection or compiled code: with uCache, which fetches it and keeps it;
// with uCache switched off (UCACHE_DISABLE=1), straight from the server; and
// with uCache again, from the cache. Each line of the report says how much
// came from the server (`ucache stats`), so it shows rather than assumes where
// the data came from. With no arguments it reads one branch of a public CMS
// open-data file. One branch on purpose: uCache reads a job that takes most of
// a large file straight from the server, without caching it.

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

// Bytes uCache has fetched from servers so far, from `ucache stats`; -1 when
// the `ucache` command is not on PATH.
double fetchedBytes() {
  std::istringstream out(gSystem->GetFromPipe("ucache stats 2>/dev/null").Data());
  for (std::string line; std::getline(out, line);) {
    double v = 0;
    if (std::sscanf(line.c_str(), " origin_bytes %lf", &v) == 1)
      return v;
  }
  return -1;
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
  const bool haveCli = fetchedBytes() >= 0;
  if (haveCli)
    gSystem->Exec(TString::Format("ucache rm '%s' >/dev/null 2>&1", url)); // start from nothing
  else
    std::printf("  `ucache` is not on PATH: the file is not removed from the cache first,\n"
                "  and the report cannot say where the data came from.\n\n");
  std::printf("  read 1 of 3 ...\n");
  const double f0 = fetchedBytes();
  const Pass fill = readInChild("", __FILE__, url, tree, branch);
  const double f1 = fetchedBytes();
  std::printf("  read 2 of 3 ...\n");
  const Pass direct = readInChild("UCACHE_DISABLE=1", __FILE__, url, tree, branch);
  std::printf("  read 3 of 3 ...\n");
  const double f2 = fetchedBytes();
  const Pass warm = readInChild("", __FILE__, url, tree, branch);
  const double f3 = fetchedBytes();
  auto fromServer = [&](double before, double after) {
    return haveCli ? TString::Format("%6.1f MB from the server", (after - before) / 1e6)
                   : TString("");
  };
  if (!fill.ok || !direct.ok || !warm.ok) {
    std::printf("\n  A read failed; its messages are above.\n");
    return;
  }

  const bool same = fill.entries == warm.entries && fill.mean == warm.mean &&
                    direct.entries == warm.entries && direct.mean == warm.mean;
  const double gain = warm.seconds > 0 ? direct.seconds / warm.seconds : 0;
  std::printf("\n  1. with uCache:   %6.1f s   %s\n", fill.seconds, fromServer(f0, f1).Data());
  std::printf("  2. uCache off:    %6.1f s   all of it from the server\n", direct.seconds);
  std::printf("  3. with uCache:   %6.1f s   %s   %.1fx faster than 2\n", warm.seconds,
              fromServer(f2, f3).Data(), gain);
  std::printf("  Same result every time: %s (%.0f values, mean %g)\n", same ? "yes" : "NO",
              warm.entries, warm.mean);
  if (gain < 1.2)
    std::printf("\n  No gain. If uCache is on (`ucache doctor`), reading is not what this job\n"
                "  waits for here: the server is close, or the job computes more than it reads.\n");
}
