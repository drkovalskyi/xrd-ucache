// Stand-ins for the libraries whose frames mark a copy (copydetect_test.cc).
// Built three times, each as its own shared library:
//
//   UCACHE_FAKE_XRDCL  exports XrdCl::ClassicCopyJob::Run under the real
//                      library's mangled name (a mangled name leaves out the
//                      return type, so this one is identical), an anchor that
//                      names the library, and a function that is NOT a copy;
//   UCACHE_FAKE_GFAL   is named libgfal_plugin_xrootd: any frame in it counts;
//   UCACHE_FAKE_RIO    is named libRIO and exports the static TFile::Cp.
//
// Each calls back into the test from inside the function that matters, so the
// test can ask the detector what it sees from there. Built with sibling calls
// off, and with work after every call, so that frame is really on the stack.

namespace {
struct FakeCall {
  void (*fn)(void*);
  void* arg;
};

__attribute__((noinline)) void callBack(FakeCall* c) {
  c->fn(c->arg);
  asm volatile("" ::: "memory"); // work after the call: never a tail call
}
} // namespace

#define FAKE_EXPORT __attribute__((visibility("default")))

#if defined(UCACHE_FAKE_XRDCL)
namespace XrdCl {
class CopyProgressHandler;
class FAKE_EXPORT ClassicCopyJob {
 public:
  int Run(CopyProgressHandler* progress);
};
__attribute__((noinline)) int ClassicCopyJob::Run(CopyProgressHandler* progress) {
  callBack(reinterpret_cast<FakeCall*>(progress));
  asm volatile("" ::: "memory");
  return 0;
}
} // namespace XrdCl

extern "C" FAKE_EXPORT int ucache_fake_anchor() { return 7; }

extern "C" FAKE_EXPORT int ucache_fake_copy_job(FakeCall* c) {
  XrdCl::ClassicCopyJob job;
  const int r = job.Run(reinterpret_cast<XrdCl::CopyProgressHandler*>(c));
  asm volatile("" ::: "memory");
  return r;
}

// The same library, outside the copy engine: must not count.
extern "C" FAKE_EXPORT int ucache_fake_not_a_copy(FakeCall* c) {
  callBack(c);
  asm volatile("" ::: "memory");
  return 0;
}
#endif

#if defined(UCACHE_FAKE_GFAL)
extern "C" FAKE_EXPORT int ucache_fake_gfal(FakeCall* c) {
  callBack(c);
  asm volatile("" ::: "memory");
  return 0;
}
#endif

#if defined(UCACHE_FAKE_RIO)
class FAKE_EXPORT TFile {
 public:
  static bool Cp(const char* src, const char* dst, bool progressbar, unsigned int buffersize);
};
__attribute__((noinline)) bool TFile::Cp(const char* src, const char*, bool, unsigned int) {
  callBack(reinterpret_cast<FakeCall*>(const_cast<char*>(src)));
  asm volatile("" ::: "memory");
  return true;
}

extern "C" FAKE_EXPORT int ucache_fake_root_cp(FakeCall* c) {
  const bool ok = TFile::Cp(reinterpret_cast<const char*>(c), "", false, 0);
  asm volatile("" ::: "memory");
  return ok ? 0 : 1;
}
#endif
