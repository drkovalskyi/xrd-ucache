// See uCache work on a small analysis: the dimuon mass spectrum of 100 CMS
// NanoAOD files, read cold and warm, first through the byte cache and then
// through recompressed replicas.
//
//   root -l -b -q ucache_try.C
//   root -l -b -q 'ucache_try.C("root://host//dir1,root://host//dir2", 50)'
//
// The analysis selects events with exactly two muons of opposite charge,
// computes their invariant mass from six muon branches, and saves the mass
// spectrum as ucache_try_dimuon.png. Six of a NanoAOD file's roughly 1300
// branches, as a typical analysis reads: uCache reads a job that takes most
// of a large file straight from the server, without caching it. It runs on
// all cores (ROOT::EnableImplicitMT). The files are the first N `.root` files
// of the given directories, in order.
//
// Four passes, each in a fresh ROOT process:
//   1. cold, byte cache  -- the files removed from the cache first, so every
//                           byte comes from the server and is kept
//   2. warm, byte cache  -- served from what pass 1 kept
//   3. cold, replica     -- removed again; with `recompress = on`, each file is
//                           converted as it is read into a form faster to decode
//   4. warm, replica     -- served from the replicas
// Each pass times its event loop only -- opening the files, reading and
// computing -- not ROOT starting up or compiling: the analysis is written with
// typed functions, which ROOT compiles when it loads this macro. Each line says
// how much came from the server (`ucache stats`), and the histogram's bin
// counts must be the same in every pass.

#include <ROOT/RDataFrame.hxx>
#include <ROOT/RVec.hxx>
#include <TCanvas.h>
#include <TError.h>
#include <TH1D.h>
#include <TLatex.h>
#include <TROOT.h>
#include <TString.h>
#include <TStyle.h>
#include <TSystem.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {
const char* kPlot = "ucache_try_dimuon.png";
const char* kDirs =
    "root://eospublic.cern.ch//eos/opendata/cms/Run2016H/SingleMuon/NANOAOD/"
    "UL2016_MiniAODv2_NanoAODv9-v1/130000,"
    "root://eospublic.cern.ch//eos/opendata/cms/Run2016H/SingleMuon/NANOAOD/"
    "UL2016_MiniAODv2_NanoAODv9-v1/70000,"
    "root://eospublic.cern.ch//eos/opendata/cms/Run2016G/SingleMuon/NANOAOD/"
    "UL2016_MiniAODv2_NanoAODv9-v1/70000";

struct Pass {
  bool ok;
  double seconds, pairs, fingerprint;
};

// The first `n` .root files of the comma-separated directories, in order.
std::vector<std::string> listFiles(const char* dirs, int n) {
  std::vector<std::string> files;
  std::istringstream in(dirs);
  for (std::string dir; std::getline(in, dir, ',') && static_cast<int>(files.size()) < n;) {
    std::vector<std::string> here;
    if (void* d = gSystem->OpenDirectory(dir.c_str())) {
      while (const char* e = gSystem->GetDirEntry(d))
        if (TString(e).EndsWith(".root"))
          here.push_back(dir + "/" + e);
      gSystem->FreeDirectory(d);
    }
    std::sort(here.begin(), here.end());
    for (const auto& f : here)
      if (static_cast<int>(files.size()) < n)
        files.push_back(f);
  }
  return files;
}

// The analysis. Returns the time of its event loop and a fingerprint of the
// histogram's bin counts, which do not depend on how threads split the work.
Pass analyse(const std::vector<std::string>& files, bool plot) {
  using ROOT::RVecF;
  using ROOT::RVecI;
  ROOT::EnableImplicitMT();
  ROOT::RDataFrame df("Events", files);
  // 300 bins, evenly spaced in log(mass), from 0.25 to 300 GeV.
  std::vector<double> edges;
  for (int i = 0; i <= 300; ++i)
    edges.push_back(0.25 * std::pow(300 / 0.25, i / 300.0));
  auto h = df.Filter([](unsigned n) { return n == 2; }, {"nMuon"}, "two muons")
               .Filter([](const RVecI& q) { return q[0] != q[1]; }, {"Muon_charge"},
                       "opposite charge")
               .Define("mass",
                       [](const RVecF& pt, const RVecF& eta, const RVecF& phi, const RVecF& m) {
                         return ROOT::VecOps::InvariantMass(pt, eta, phi, m);
                       },
                       {"Muon_pt", "Muon_eta", "Muon_phi", "Muon_mass"})
               .Histo1D<float>({"mass", ";m_{#mu#mu} (GeV);events", 300, edges.data()}, "mass");
  const auto t0 = std::chrono::steady_clock::now();
  const double pairs = h->GetEntries(); // runs the event loop
  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  double fingerprint = 0;
  for (int i = 0; i <= h->GetNbinsX() + 1; ++i)
    fingerprint += (i + 1) * h->GetBinContent(i);
  if (plot) {
    gStyle->SetOptStat(0);
    TCanvas c("c", "", 900, 600);
    c.SetLogx();
    c.SetLogy();
    h->SetLineColor(kAzure + 3);
    h->SetLineWidth(2);
    h->SetMaximum(20 * h->GetMaximum()); // room for the labels below the header
    h->Draw("hist");
    TLatex t;
    t.SetNDC();
    t.SetTextFont(42);
    t.SetTextSize(0.04);
    t.DrawLatex(0.15, 0.85,
                TString::Format("#bf{CMS open data} Run2016 SingleMuon, %zu files", files.size()));
    t.DrawLatex(0.15, 0.80, "two muons of opposite charge, read through uCache");
    // The resonances, labelled just above their peaks.
    TLatex peak;
    peak.SetTextFont(42);
    peak.SetTextSize(0.035);
    peak.SetTextAlign(21);
    const std::pair<double, const char*> resonances[] = {
        {1.019, "#phi"}, {3.097, "J/#psi"}, {3.686, "#psi'"}, {9.46, "#Upsilon"}, {91.19, "Z"}};
    for (const auto& [m, name] : resonances)
      peak.DrawLatex(m, 1.4 * h->GetBinContent(h->FindBin(m)), name);
    c.SaveAs(kPlot);
  }
  return {true, seconds, pairs, fingerprint};
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

// Remove the files from the cache, byte copies and replicas alike.
void removeFromCache(const std::vector<std::string>& files) {
  TString cmd = "ucache rm";
  for (const auto& f : files)
    cmd += TString::Format(" '%s'", f.c_str());
  gSystem->Exec(cmd + " >/dev/null 2>&1");
}

// One pass in a fresh ROOT process, with `env` in front of it; `child` 2 also
// saves the plot.
Pass passInChild(const char* env, const char* macro, const char* dirs, int n, int child) {
  const TString cmd = TString::Format("%s '%s/root.exe' -l -b -q '%s(\"%s\",%d,%d)'", env,
                                      TROOT::GetBinDir().Data(), macro, dirs, n, child);
  std::istringstream out(gSystem->GetFromPipe(cmd).Data());
  for (std::string line; std::getline(out, line);) {
    Pass p{true, 0, 0, 0};
    if (std::sscanf(line.c_str(), "UCACHE_TRY %lf %lf %lf", &p.seconds, &p.pairs, &p.fingerprint) == 3)
      return p;
  }
  return {false, 0, 0, 0};
}
} // namespace

void ucache_try(const char* dirs = kDirs, int nfiles = 100, int child = 0) {
  gErrorIgnoreLevel = kWarning + 1; // CMS files carry metadata classes ROOT warns it cannot read
  const std::vector<std::string> files = listFiles(dirs, nfiles);
  if (child) {
    const Pass p = analyse(files, child == 2);
    std::printf("UCACHE_TRY %.6f %.0f %.17g\n", p.seconds, p.pairs, p.fingerprint);
    return;
  }
  if (files.empty()) {
    std::printf("uCache try: no .root files found in %s\n", dirs);
    return;
  }
  ROOT::EnableImplicitMT();
  std::printf("uCache try: dimuon mass spectrum of %zu files, %u threads\n", files.size(),
              ROOT::GetThreadPoolSize());
  const bool haveCli = fetchedBytes() >= 0;
  if (!haveCli)
    std::printf("  `ucache` is not on PATH: the files cannot be removed from the cache\n"
                "  before the cold passes, and the report cannot say where data came from.\n");

  struct Run {
    const char* label;
    const char* env;
    bool cold;
    Pass pass;
    double fetched;
  } runs[] = {{"cold, byte cache", "UCACHE_RECOMPRESS=off", true, {}, 0},
              {"warm, byte cache", "UCACHE_RECOMPRESS=off", false, {}, 0},
              {"cold, replica   ", "UCACHE_RECOMPRESS=on", true, {}, 0},
              {"warm, replica   ", "UCACHE_RECOMPRESS=on", false, {}, 0}};
  for (int k = 0; k < 4; ++k) {
    std::printf("  pass %d of 4: %s ...\n", k + 1, runs[k].label);
    if (runs[k].cold && haveCli)
      removeFromCache(files);
    const double before = fetchedBytes();
    runs[k].pass = passInChild(runs[k].env, __FILE__, dirs, nfiles, k == 3 ? 2 : 1);
    runs[k].fetched = fetchedBytes() - before;
    if (!runs[k].pass.ok) {
      std::printf("\n  Pass %d failed; its messages are above.\n", k + 1);
      return;
    }
  }

  bool same = true;
  for (const auto& r : runs)
    same = same && r.pass.pairs == runs[0].pass.pairs &&
           r.pass.fingerprint == runs[0].pass.fingerprint;
  std::printf("\n  event-loop time (ROOT start-up and compiling not counted):\n");
  for (int k = 0; k < 4; ++k) {
    const auto& r = runs[k];
    TString from = haveCli ? TString::Format("%6.2f GB from the server", r.fetched / 1e9) : TString("");
    TString rel = k == 0 ? TString("")
                         : TString::Format("   %.1fx faster than 1", runs[0].pass.seconds / r.pass.seconds);
    std::printf("  %d. %s %7.1f s   %s%s\n", k + 1, r.label, r.pass.seconds, from.Data(), rel.Data());
  }
  std::printf("  Same result every time: %s (%.0f muon pairs)\n", same ? "yes" : "NO",
              runs[0].pass.pairs);
  std::printf("  Mass plot: %s\n", kPlot);
}
