// The copy detector: which handles belong to a copy. The names and strings are
// pinned directly; the stack check is exercised through stand-in libraries
// (copydetect_fake.cc) loaded by path, so each test decides when a library
// appears. That real copy tools are recognised is an environment-bound check.
#include "CopyDetect.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <gtest/gtest.h>
#include <iterator>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace ucache;

namespace {

struct FakeCall {
  void (*fn)(void*);
  void* arg;
};
using Entry = int (*)(FakeCall*);

// Never a real signal: tells "the callback did not run" from any answer.
const CopySignal kUnset = static_cast<CopySignal>(0xff);

void* openFake(const char* path) {
  void* h = ::dlopen(path, RTLD_NOW | RTLD_LOCAL);
  if (!h)
    ADD_FAILURE() << "cannot load " << path << ": " << ::dlerror();
  return h;
}
Entry entry(void* h, const char* name) {
  Entry e = h ? reinterpret_cast<Entry>(::dlsym(h, name)) : nullptr;
  if (!e)
    ADD_FAILURE() << "no " << name << " in the stand-in library";
  return e;
}

void observeStack(void* out) { *static_cast<CopySignal*>(out) = copierStackSignal(); }
void observeHandle(void* out) { *static_cast<CopySignal*>(out) = copierSignal(); }

// What the detector sees from inside `e`'s marked frame.
CopySignal seenFrom(Entry e, void (*observe)(void*) = observeStack) {
  CopySignal got = kUnset;
  FakeCall c{observe, &got};
  if (e)
    e(&c);
  return got;
}

// `left` more frames, then look. Work after each call keeps every frame.
struct Deep {
  int left;
  CopySignal* out;
};
__attribute__((noinline)) void descend(void* p) {
  auto* d = static_cast<Deep*>(p);
  if (d->left-- > 0) {
    descend(p);
    asm volatile("" ::: "memory");
    return;
  }
  *d->out = copierStackSignal();
}

const void* fakeAnchor(void* h) { return h ? ::dlsym(h, "ucache_fake_anchor") : nullptr; }

} // namespace

TEST(CopyDetect, ExecutableNames) {
  for (const char* n : {"xrdcp", "xrdcopy", "xrdfs", "xrdadler32", "edmCopyUtil"})
    EXPECT_TRUE(isCopyToolExecutable(n)) << n;
  for (const char* n : {"root.exe", "python3.12", "cmsRun", "ucache", "ucache-workload",
                        "xrdcp2", "myxrdcp", "", "XRDCP"})
    EXPECT_FALSE(isCopyToolExecutable(n)) << n;
  EXPECT_EQ(executableBaseName("/usr/bin/xrdcp"), "xrdcp");
  // What the kernel shows for a program replaced on disk while it runs.
  EXPECT_EQ(executableBaseName("/usr/bin/xrdcp (deleted)"), "xrdcp");
  EXPECT_EQ(executableBaseName("xrdfs"), "xrdfs");
  EXPECT_EQ(executableBaseName("/opt/x/bin/"), "");
#if defined(__linux__) || defined(__APPLE__)
  // The running program is found, and it is not a copy tool.
  EXPECT_EQ(hostExecutable(), "ucache-unit-tests");
#endif
  EXPECT_EQ(copierSignal(), CopySignal::kNone);
}

// hadd and ROOT's command-line tools read the origin's file: they merge, copy
// or report on it. Most of the tools are Python scripts, recognised by the
// script the interpreter runs.
TEST(CopyDetect, RootToolPrograms) {
  for (const char* n : {"hadd", "rootcp", "rootmv", "rooteventselector", "rootslimtree",
                        "rootls", "rootprint", "rootdrawtree", "rootbrowse", "rootrm",
                        "rootmkdir"})
    EXPECT_TRUE(isRootToolProgram(n)) << n;
  for (const char* n : {"root", "root.exe", "python3", "haddx", "rootcling", "", "HADD"})
    EXPECT_FALSE(isRootToolProgram(n)) << n;
  using V = std::vector<std::string>;
  EXPECT_EQ(scriptOfCommandLine(V{"python3", "/cvmfs/x/bin/rootcp", "a.root", "b.root"}), "rootcp");
  EXPECT_EQ(scriptOfCommandLine(V{"/usr/bin/python3.12", "-u", "-W", "ignore", "/x/rootls", "-t"}),
            "rootls");
  EXPECT_EQ(scriptOfCommandLine(V{"python3", "-c", "import rootcp"}), "");
  EXPECT_EQ(scriptOfCommandLine(V{"python3", "-m", "rootls"}), "");
  EXPECT_EQ(scriptOfCommandLine(V{"python3"}), "");
  EXPECT_EQ(scriptOfCommandLine(V{"hadd", "out.root", "rootcp"}), ""); // not an interpreter
  EXPECT_EQ(scriptOfCommandLine(V{}), "");
#if defined(__linux__) || defined(__APPLE__)
  EXPECT_EQ(hostScript(), ""); // the test binary is not an interpreter
#endif
  EXPECT_EQ(copyProgramSignal(), CopySignal::kNone);
}

// A typo in any of these switches detection off without a sound.
TEST(CopyDetect, SymbolListPinned) {
  const std::vector<std::string> engine(std::begin(kCopyEngineSymbols),
                                        std::end(kCopyEngineSymbols));
  EXPECT_EQ(engine, (std::vector<std::string>{
                        "_ZN5XrdCl14ClassicCopyJob3RunEPNS_19CopyProgressHandlerE",
                        "_ZN5XrdCl17ThirdPartyCopyJob3RunEPNS_19CopyProgressHandlerE",
                        "_ZN5XrdCl11CopyProcess3RunEPNS_19CopyProgressHandlerE",
                        "_ZN5XrdCl6XCpSrc3RunEPv",
                    }));
  EXPECT_STREQ(kRootCpSymbol, "_ZN5TFile2CpEPKcS1_bj");
  const std::vector<std::string> merge(std::begin(kRootMergeSymbols), std::end(kRootMergeSymbols));
  EXPECT_EQ(merge, (std::vector<std::string>{"_ZN11TFileMerger7AddFileEPKcb",
                                             "_ZN11TFileMerger15OpenExcessFilesEv"}));
  EXPECT_STREQ(kGfalXrootdPrefix, "libgfal_plugin_xrootd");
  EXPECT_EQ(kCopyStackDepth, 32);
}

TEST(CopyDetect, ObjectNames) {
  EXPECT_TRUE(isGfalXrootdObject("/usr/lib64/gfal2-plugins/libgfal_plugin_xrootd.so"));
  EXPECT_TRUE(isGfalXrootdObject("libgfal_plugin_xrootd.so"));
  EXPECT_FALSE(isGfalXrootdObject("/usr/lib64/libgfal2.so.2"));
  EXPECT_FALSE(isGfalXrootdObject("/usr/lib64/gfal2-plugins/libgfal_plugin_http.so"));
  EXPECT_TRUE(isRootIoObject("/opt/root/lib/libRIO.so"));
  EXPECT_TRUE(isRootIoObject("/opt/root/lib/libRIO.so.6.36"));
  EXPECT_TRUE(isRootIoObject("/opt/local/libexec/root6/lib/root/libRIO.dylib"));
  EXPECT_FALSE(isRootIoObject("/opt/root/lib/libRIOx.so"));
  EXPECT_FALSE(isRootIoObject("/opt/root/lib/libTree.so"));
}

// A frame inside the copy engine's Run marks a copy; the same library's other
// code does not, a direct call does not, and neither does the engine of a
// library the plugin is not bound to.
TEST(CopyDetect, CopyEngineFrameDetected) {
  void* h = openFake(UCACHE_COPYFAKE_XRDCL);
  ASSERT_NE(h, nullptr);
  copyDetectInit(fakeAnchor(h));
  EXPECT_EQ(seenFrom(entry(h, "ucache_fake_copy_job")), CopySignal::kCopyEngine);
  EXPECT_EQ(seenFrom(entry(h, "ucache_fake_copy_job"), observeHandle), CopySignal::kCopyEngine);
  EXPECT_EQ(seenFrom(entry(h, "ucache_fake_not_a_copy")), CopySignal::kNone);
  EXPECT_EQ(copierStackSignal(), CopySignal::kNone);
  copyDetectInit(nullptr); // no anchor: no library's copy engine counts
  EXPECT_EQ(seenFrom(entry(h, "ucache_fake_copy_job")), CopySignal::kNone);
}

TEST(CopyDetect, GfalRangeDetected) {
#if defined(__APPLE__)
  GTEST_SKIP() << "gfal2 is not built for this platform";
#else
  copyDetectInit(nullptr);
  void* h = openFake(UCACHE_COPYFAKE_GFAL);
  ASSERT_NE(h, nullptr);
  EXPECT_EQ(seenFrom(entry(h, "ucache_fake_gfal")), CopySignal::kGfal);
  EXPECT_EQ(copierStackSignal(), CopySignal::kNone);
#endif
}

// ROOT's I/O library is often loaded after the plugin starts (PyROOT loads it
// on first use): a library that appears later is found without a new init.
TEST(CopyDetect, LateLoadedLibrary) {
  void* x = openFake(UCACHE_COPYFAKE_XRDCL);
  ASSERT_NE(x, nullptr);
  copyDetectInit(fakeAnchor(x));
  EXPECT_EQ(copierStackSignal(), CopySignal::kNone); // the table is built without it
  void* r = openFake(UCACHE_COPYFAKE_RIO);
  ASSERT_NE(r, nullptr);
  EXPECT_EQ(seenFrom(entry(r, "ucache_fake_root_cp")), CopySignal::kRootCp);
  EXPECT_EQ(seenFrom(entry(r, "ucache_fake_merger_add")), CopySignal::kMerge);
  // ... and what was found before is still found.
  EXPECT_EQ(seenFrom(entry(x, "ucache_fake_copy_job")), CopySignal::kCopyEngine);
}

// The walk stops at kCopyStackDepth frames, on purpose: a match deeper than
// that is not seen. A shallow one through the same helper is.
TEST(CopyDetect, DepthBound) {
  void* h = openFake(UCACHE_COPYFAKE_XRDCL);
  ASSERT_NE(h, nullptr);
  copyDetectInit(fakeAnchor(h));
  Entry e = entry(h, "ucache_fake_copy_job");
  ASSERT_NE(e, nullptr);
  for (const auto& [frames, want] : {std::pair<int, CopySignal>{4, CopySignal::kCopyEngine},
                                     std::pair<int, CopySignal>{kCopyStackDepth + 8,
                                                                CopySignal::kNone}}) {
    CopySignal got = kUnset;
    Deep d{frames, &got};
    FakeCall c{descend, &d};
    e(&c);
    EXPECT_EQ(got, want) << frames << " frames above the copy engine";
  }
}

// Handles open on many threads while libraries come and go; the table is
// rebuilt without a lock and every answer stays right.
TEST(CopyDetect, ConcurrentWalksWhileLibrariesLoad) {
  void* x = openFake(UCACHE_COPYFAKE_XRDCL);
  ASSERT_NE(x, nullptr);
  copyDetectInit(fakeAnchor(x));
  Entry e = entry(x, "ucache_fake_copy_job");
  ASSERT_NE(e, nullptr);
  std::atomic<int> wrong{0};
  std::vector<std::thread> ts;
  for (int t = 0; t < 4; ++t)
    ts.emplace_back([&] {
      for (int i = 0; i < 500; ++i) {
        if (copierStackSignal() != CopySignal::kNone)
          ++wrong;
        if (seenFrom(e) != CopySignal::kCopyEngine)
          ++wrong;
      }
    });
  for (int i = 0; i < 20; ++i) { // each load and unload changes the loader's generation
    void* g = ::dlopen(UCACHE_COPYFAKE_GFAL, RTLD_NOW | RTLD_LOCAL);
    if (g)
      ::dlclose(g);
  }
  for (auto& t : ts)
    t.join();
  EXPECT_EQ(wrong.load(), 0);
}

// Cost of the walk from a reader-like stack. Run by hand:
//   ucache-unit-tests --gtest_also_run_disabled_tests --gtest_filter='*CostProbe*'
TEST(CopyDetect, DISABLED_CostProbe) {
  void* h = openFake(UCACHE_COPYFAKE_XRDCL); // ranges to compare against, none on the stack
  ASSERT_NE(h, nullptr);
  copyDetectInit(fakeAnchor(h));
  constexpr int kCalls = 20000;
  CopySignal got = kUnset;
  Deep d{28, &got};
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < kCalls; ++i) {
    d.left = 28;
    descend(&d);
  }
  const double ns =
      std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count();
  std::printf("copierStackSignal from a ~30-frame stack: %.0f ns per call\n", ns / kCalls);
  EXPECT_EQ(got, CopySignal::kNone);
}
