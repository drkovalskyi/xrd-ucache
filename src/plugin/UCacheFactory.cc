#include <cstdio>
#include <cstdlib>
// Plugin factory + entry point. XrdCl resolves XrdClGetPlugIn
// from the library named in the plugin conf and calls it with the conf's
// key/value map; the returned factory produces per-file plugins.
// CreateFileSystem returns nullptr: XrdCl logs and continues without a
// plugin for filesystem objects (verified in XrdClFileSystem.cc) — all FS
// ops are naturally pass-through, and a broken cache can never break a job.
#include "Executor.h"
#include "Log.h"
#include "UCacheFile.h"

#include <XrdVersion.hh>

#include <mutex>

namespace ucache {

namespace {
std::once_flag gInitFlag;
Config* gConfig = nullptr;
std::shared_ptr<CacheStore>* gStore = nullptr;
// The plugin conf's key/value map: XrdCl hands it to XrdClGetPlugIn,
// but only for the duration of the call — copied here (and leaked, like the
// other globals) before the config is first built. First conf wins if two
// conf files name this library.
std::map<std::string, std::string>* gPluginConf = nullptr;

void initGlobals() {
  // Leaked intentionally: destruction order against XrdCl teardown and the
  // executor threads is unknowable; the final stats dump happens via atexit.
  gConfig = new Config(Config::fromEnv(gPluginConf));
  // No cache dir configured — deliberately no default. Null store =>
  // every handle is passthroughOnly_: the job runs correctly, just uncached.
  if (gConfig->cacheDir.empty() && !gConfig->disable)
    UCACHE_WARN("no cache dir configured — set `dir =` in ucache.conf (USER_GUIDE §2) "
                "or UCACHE_DIR; running uncached (pass-through)");
  gStore = new std::shared_ptr<CacheStore>(
      gConfig->cacheDir.empty()
          ? nullptr
          : std::make_shared<CacheStore>(RealIO::instance(), *gConfig));
  Executor::instance(static_cast<unsigned>(
      gConfig->threads > 0 ? gConfig->threads : 0));
  ::atexit([] {
    if (gStore && *gStore)
      (*gStore)->dumpStats(/*finalDump=*/true); // + Layer-2 records of live entries
  });
  UCACHE_INFO("xrd-ucache plugin initialized (dir=%s page=%u disable=%d)",
              gConfig->cacheDir.c_str(), gConfig->pageSize, gConfig->disable ? 1 : 0);
}
} // namespace

const Config& globalConfig() {
  std::call_once(gInitFlag, initGlobals);
  return *gConfig;
}

std::shared_ptr<CacheStore> globalStore() {
  std::call_once(gInitFlag, initGlobals);
  return *gStore;
}

class UCacheFactory : public XrdCl::PlugInFactory {
 public:
  XrdCl::FilePlugIn* CreateFile(const std::string& url) override {
    if (::getenv("UCACHE_DEBUG"))
      fprintf(stderr, "[ucache] CreateFile url=%s\n", url.c_str());
    auto* p = new UCacheFile();
    if (::getenv("UCACHE_DEBUG"))
      fprintf(stderr, "[ucache] CreateFile returning %p\n", static_cast<void*>(p));
    return p;
  }
  XrdCl::FileSystemPlugIn* CreateFileSystem(const std::string& /*url*/) override {
    if (::getenv("UCACHE_DEBUG"))
      fprintf(stderr, "[ucache] CreateFileSystem called (returns null)\n");
    return nullptr; // default FileSystem; see header comment
  }
};

} // namespace ucache

XrdVERSIONINFO(XrdClGetPlugIn, XrdClUCache)

extern "C" {
void* XrdClGetPlugIn(const void* config) {
  // `config` is the conf file's key/value map (std::map<std::string,
  // std::string>*, verified in 5.8.3 PlugInManager::LoadFactory) — valid only
  // during this call. ucache settings may live right in that file (one
  // config file); copy the map before the first globalConfig() build.
  if (config && !ucache::gPluginConf)
    ucache::gPluginConf = new std::map<std::string, std::string>(
        *static_cast<const std::map<std::string, std::string>*>(config));
  ucache::globalConfig(); // initialize early, on the loader's thread
  // Heap-allocated: XrdCl::PlugInManager takes ownership and deletes the
  // factory at teardown (FactoryHelper dtor) — a static here crashes exit.
  return new ucache::UCacheFactory();
}
}
