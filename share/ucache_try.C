// See uCache work on a small analysis: the dimuon mass spectrum of a CMS
// NanoAOD file, read from the server and from the cache.
//
//   root -l -b -q ucache_try.C
//   root -l -b -q 'ucache_try.C("root://host//path/other_nanoaod.root")'
//
// The analysis selects events with exactly two muons of opposite charge,
// computes their invariant mass from six muon branches, and saves the mass
// spectrum as ucache_try_dimuon.png. Six of a NanoAOD file's roughly 1500
// branches, as a typical analysis reads: uCache reads a job that takes most
// of a large file straight from the server, without caching it.
//
// The file is first removed from the cache (`ucache rm`), then analysed three
// times, each in a fresh ROOT process so that none inherits another's
// connection or compiled code: with uCache, which fetches the data and keeps
// it; with uCache switched off (UCACHE_DISABLE=1), straight from the server;
// and with uCache again, from the cache. Each line of the report says how much
// came from the server (`ucache stats`), so it shows rather than assumes where
// the data came from. With no arguments it reads a public CMS open-data file.

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

#include <chrono>
#include <cmath>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

namespace {
struct Pass {
  bool ok;
  double seconds, pairs, mean;
};

const char* kPlot = "ucache_try_dimuon.png";

// The analysis. Returns its wall time and a fingerprint of its result.
Pass analyse(const char* url, bool plot) {
  const auto t0 = std::chrono::steady_clock::now();
  ROOT::RDataFrame df("Events", url);
  // 300 bins, evenly spaced in log(mass), from 0.25 to 300 GeV.
  std::vector<double> edges;
  for (int i = 0; i <= 300; ++i)
    edges.push_back(0.25 * std::pow(300 / 0.25, i / 300.0));
  auto h = df.Filter("nMuon == 2", "two muons")
               .Filter("Muon_charge[0] != Muon_charge[1]", "opposite charge")
               .Define("mass", "ROOT::VecOps::InvariantMass(Muon_pt, Muon_eta, Muon_phi, Muon_mass)")
               .Histo1D({"mass", ";m_{#mu#mu} (GeV);events", 300, edges.data()}, "mass");
  const double pairs = h->GetEntries(); // runs the event loop
  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
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
    t.DrawLatex(0.15, 0.85, "#bf{CMS open data} Run2016H SingleMuon, read through uCache");
    t.DrawLatex(0.15, 0.80, "two muons of opposite charge");
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
  return {true, seconds, pairs, h->GetMean()};
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

// One analysis in a fresh ROOT process, with `env` in front of it; `child` 2
// also saves the plot.
Pass analyseInChild(const char* env, const char* macro, const char* url, int child) {
  const TString cmd = TString::Format("%s '%s/root.exe' -l -b -q '%s(\"%s\",%d)'", env,
                                      TROOT::GetBinDir().Data(), macro, url, child);
  std::istringstream out(gSystem->GetFromPipe(cmd).Data());
  for (std::string line; std::getline(out, line);) {
    Pass p{true, 0, 0, 0};
    if (std::sscanf(line.c_str(), "UCACHE_TRY %lf %lf %lf", &p.seconds, &p.pairs, &p.mean) == 3)
      return p;
  }
  return {false, 0, 0, 0};
}
} // namespace

void ucache_try(const char* url = "root://eospublic.cern.ch//eos/opendata/cms/Run2016H/SingleMuon/"
                                  "NANOAOD/UL2016_MiniAODv2_NanoAODv9-v1/130000/"
                                  "5CA4CE73-629C-2F48-939C-4274B369F112.root",
                int child = 0) {
  gErrorIgnoreLevel = kWarning + 1; // CMS files carry metadata classes ROOT warns it cannot read
  if (child) {
    const Pass p = analyse(url, child == 2);
    std::printf("UCACHE_TRY %.6f %.0f %.17g\n", p.seconds, p.pairs, p.mean);
    return;
  }
  std::printf("uCache try: dimuon mass spectrum of\n  %s\n\n", url);
  const bool haveCli = fetchedBytes() >= 0;
  if (haveCli)
    gSystem->Exec(TString::Format("ucache rm '%s' >/dev/null 2>&1", url)); // start from nothing
  else
    std::printf("  `ucache` is not on PATH: the file is not removed from the cache first,\n"
                "  and the report cannot say where the data came from.\n\n");
  std::printf("  analysis 1 of 3 ...\n");
  const double f0 = fetchedBytes();
  const Pass fill = analyseInChild("", __FILE__, url, 1);
  const double f1 = fetchedBytes();
  std::printf("  analysis 2 of 3 ...\n");
  const Pass direct = analyseInChild("UCACHE_DISABLE=1", __FILE__, url, 1);
  std::printf("  analysis 3 of 3 ...\n");
  const double f2 = fetchedBytes();
  const Pass warm = analyseInChild("", __FILE__, url, 2);
  const double f3 = fetchedBytes();
  if (!fill.ok || !direct.ok || !warm.ok) {
    std::printf("\n  An analysis failed; its messages are above.\n");
    return;
  }
  auto fromServer = [&](double before, double after) {
    return haveCli ? TString::Format("%7.1f MB from the server", (after - before) / 1e6)
                   : TString("");
  };

  const bool same = fill.pairs == warm.pairs && fill.mean == warm.mean &&
                    direct.pairs == warm.pairs && direct.mean == warm.mean;
  const double gain = warm.seconds > 0 ? direct.seconds / warm.seconds : 0;
  std::printf("\n  1. with uCache:   %6.1f s   %s\n", fill.seconds, fromServer(f0, f1).Data());
  std::printf("  2. uCache off:    %6.1f s     all of it from the server\n", direct.seconds);
  std::printf("  3. with uCache:   %6.1f s   %s   %.1fx faster than 2\n", warm.seconds,
              fromServer(f2, f3).Data(), gain);
  std::printf("  Same result every time: %s (%.0f muon pairs)\n", same ? "yes" : "NO", warm.pairs);
  std::printf("  Mass plot: %s\n", kPlot);
  if (gain < 1.2)
    std::printf("\n  No gain. If uCache is on (`ucache doctor`), reading is not what this job\n"
                "  waits for here: the server is close, or the job computes more than it reads.\n");
}
