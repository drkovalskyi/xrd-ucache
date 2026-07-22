#include "UCacheFile.h"

#include "Executor.h"
#include "Log.h"
#include "OpenRetry.h"
#include "Trace.h"
#include "vendor/crc32c.h"

#include <XProtocol/XProtocol.hh> // kXR_* wire codes for the OpenRetry drift asserts

#include <spawn.h>
#include <sys/resource.h>
#include <sys/stat.h>
extern char** environ; // POSIX (glibc: not declared without _GNU_SOURCE)
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <thread>

namespace ucache {

using XrdCl::AnyObject;
using XrdCl::ChunkInfo;
using XrdCl::ChunkList;
using XrdCl::HostList;
using XrdCl::ResponseHandler;
using XrdCl::XRootDStatus;

// OpenRetry.cc hardcodes these XRootD 5.8.3 ABI/wire values to stay XrdCl-free
// (the raw-errNo comparison is deliberate); assert they
// still match the headers so ABI drift breaks the build, not the field.
static_assert(XrdCl::errConnectionError == 108 && XrdCl::errErrorResponse == 400,
              "XrdCl status codes drifted from OpenRetry.cc");
static_assert(kXR_ItExists == 3018 && kXR_Overloaded == 3024,
              "kXR wire codes drifted from OpenRetry.cc");

namespace {

// ---- Test-only open fault injection (inert unless armed) -------------------
// UCACHE_TEST_OPEN_FAIL_N=N rewrites the target port of a handle's first N inner
// open ATTEMPTS to port 1 (privileged, never bound), so the real open fails
// with errConnectionError and the inner XrdCl::File lands in the terminal Error
// state — the exact condition the fresh-file retry must survive. Ships
// compiled-in and inert (deterministic CI beats a build-flag matrix); the only
// test-only hook in shipped plugin code (a deliberate precedent).
int openFaultFirstN() {
  static const int n = [] {
    const char* v = ::getenv("UCACHE_TEST_OPEN_FAIL_N");
    return v ? ::atoi(v) : 0;
  }();
  return n;
}
std::string rewritePortTo1(const std::string& url) {
  auto sch = url.find("://");
  if (sch == std::string::npos)
    return url;
  size_t authStart = sch + 3;
  size_t authEnd = url.find('/', authStart);
  if (authEnd == std::string::npos)
    authEnd = url.size();
  std::string auth = url.substr(authStart, authEnd - authStart);
  auto colon = auth.rfind(':');
  std::string newAuth =
      (colon != std::string::npos) ? auth.substr(0, colon) + ":1" : auth + ":1";
  return url.substr(0, authStart) + newAuth + url.substr(authEnd);
}
// attempt is 1-based; returns url unchanged unless the fault is armed and this
// attempt is within the first N.
std::string faultOpenUrl(const std::string& url, int attempt) {
  int n = openFaultFirstN();
  return (n > 0 && attempt <= n) ? rewritePortTo1(url) : url;
}

// ---- Test-only wire-read fault injection (inert unless armed) --------------
// UCACHE_TEST_READ_FAIL_N=N: the process's first N rounded wire reads (the
// miss machinery's inner Read/VectorRead) complete with errConnectionError
// instead of being issued — the origin dying mid-stream AFTER a healthy open.
// The relay pass-through and every other inner op are untouched, so the §5.2
// step-5 fail-open contract (the user still gets the exact bytes) is testable
// deterministically. Sibling of UCACHE_TEST_OPEN_FAIL_N above.
bool readFaultFire() {
  static std::atomic<int> budget{[] {
    const char* v = ::getenv("UCACHE_TEST_READ_FAIL_N");
    return v ? ::atoi(v) : 0;
  }()};
  if (budget.load(std::memory_order_relaxed) <= 0)
    return false; // production fast path: one relaxed load
  return budget.fetch_sub(1, std::memory_order_relaxed) > 0;
}

// ---- Case A: retrying open handler -----------------------------------------
// Wraps ROOT's open handler to retry a TRANSIENT open failure on a FRESH inner
// file (a failed open is terminal on the object), with full-jitter backoff
// scheduled off the XrdCl callback thread. Forwards EXACTLY ONE final completion
// to the real handler, then self-deletes. Captures only st_ (never the plugin
// object): the posted retry may run after Open has returned.
class RetryingOpenHandler : public ResponseHandler {
 public:
  RetryingOpenHandler(std::shared_ptr<HandleState> st, std::string url,
                      XrdCl::OpenFlags::Flags flags, XrdCl::Access::Mode mode,
                      uint16_t timeout, ResponseHandler* real, const Config& cfg)
      : st_(std::move(st)), url_(std::move(url)), flags_(flags), mode_(mode),
        timeout_(timeout), real_(real), cfg_(cfg) {}

  // Issue the first attempt on the ctor-created inner file. Always returns stOK:
  // `real_` will receive exactly one completion (now, after a retry, or a
  // forwarded failure), so the XrdCl sync wrapper waits for our handler instead
  // of short-circuiting on a sync error.
  XRootDStatus start() {
    XRootDStatus s = issueOn(st_->inner);
    if (!s.IsOK()) { // sync issue error: no callback will come
      lastStatus_ = s;
      decideRetryOrForward();
    }
    return XRootDStatus();
  }

 private:
  XRootDStatus issueOn(XrdCl::File* f) {
    ++attempt_;
    return f->Open(faultOpenUrl(url_, attempt_), flags_, mode_, this, timeout_);
  }

  void HandleResponseWithHosts(XRootDStatus* status, AnyObject* response,
                               HostList* hostList) override {
    std::unique_ptr<XRootDStatus> s(status);
    std::unique_ptr<AnyObject> r(response);
    std::unique_ptr<HostList> h(hostList);
    if (s && s->IsOK()) { // transparent on success: forward exactly what we got
      real_->HandleResponseWithHosts(s.release(), r.release(), h.release());
      delete this;
      return;
    }
    lastStatus_ = s ? *s : XRootDStatus(XrdCl::stError);
    decideRetryOrForward();
  }

  // Classify lastStatus_; schedule a fresh-file retry if retryable with budget
  // left, else forward the failure once.
  void decideRetryOrForward() {
    bool retry =
        attempt_ <= cfg_.openRetries && isRetryableOpen(lastStatus_.code, lastStatus_.errNo);
    if (retry) {
      if (st_->store)
        st_->store->stats().openRetries.fetch_add(1, std::memory_order_relaxed);
      uint64_t backoff = backoffMs(attempt_, cfg_);
      RetryingOpenHandler* self = this;
      Executor::instance().postAfter(backoff, [self] { self->doRetry(); });
      return;
    }
    if (attempt_ > 1 && st_->store) // gave up after >= 1 retry
      st_->store->stats().openRetriesExhausted.fetch_add(1, std::memory_order_relaxed);
    forwardFailure();
  }

  // Runs on the pool (off the XrdCl callback thread): swap in a fresh file and
  // re-issue. If the handle shut down mid-retry, forward the last failure.
  void doRetry() {
    bool valid;
    {
      std::lock_guard<std::mutex> g(st_->mu);
      valid = st_->innerValid;
    }
    if (!valid) {
      forwardFailure();
      return;
    }
    XrdCl::File* fresh = st_->resetInner();
    XRootDStatus s = issueOn(fresh);
    if (!s.IsOK()) { // sync re-issue error: classify again, never drop a completion
      lastStatus_ = s;
      decideRetryOrForward();
    }
  }

  void forwardFailure() {
    // Non-null empty HostList: the native completion contract (see complete()).
    real_->HandleResponseWithHosts(new XRootDStatus(lastStatus_), nullptr,
                                   new HostList());
    delete this;
  }

  std::shared_ptr<HandleState> st_;
  std::string url_;
  XrdCl::OpenFlags::Flags flags_;
  XrdCl::Access::Mode mode_;
  uint16_t timeout_;
  ResponseHandler* real_;
  const Config& cfg_;
  int attempt_ = 0;
  XRootDStatus lastStatus_;
};

// kXR readv element ceiling (16-byte header inside a 2 MiB frame); rounding
// that would exceed it is skipped for that chunk.
constexpr uint64_t kMaxReadvElem = 2 * 1024 * 1024 - 16;

uint64_t nowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Every completion the plugin synthesizes (trusted cache-only opens, locally
// served reads, no-op closes) must honor the NATIVE completion contract:
// XrdCl always hands HandleResponseWithHosts a non-null HostList, and real
// consumers dereference it unconditionally — CMSSW's XrdAdaptor SEGVs on
// null (tracerouteRedirections iterates *hostList; reproduced with cmsRun on
// a warm cache). An EMPTY list is the honest value for a locally-served
// completion (no host was contacted) and is safe in every consumer we
// audited (traceroute prints an empty trace without touching per-host
// transport queries; determineHostExcludeString is size-guarded). The
// receiving handler owns and deletes it, exactly as with native XrdCl.
void complete(ResponseHandler* h, XRootDStatus* status, AnyObject* response) {
  h->HandleResponseWithHosts(status, response, new HostList());
}

XRootDStatus* okStatus() { return new XRootDStatus(); }

AnyObject* chunkResponse(uint64_t off, uint32_t len, void* buf) {
  auto* obj = new AnyObject();
  obj->Set(new ChunkInfo(off, len, buf));
  return obj;
}

// Cache-freshness marker (UCACHE_REVALIDATE_S): objects/<aa>/<hash>.val, whose
// mtime records the last successful remote validation. touchVal() stamps it
// after a real origin stat; cacheFresh() trusts the entry while it is younger
// than the window AND a sidecar is present.
std::string valPath(const UrlKey& key, const std::string& cacheDir) {
  return key.objectDir(cacheDir) + "/" + key.hashHex + ".val";
}
void touchVal(const UrlKey& key, const std::string& cacheDir) {
  int fd = RealIO::instance().open(valPath(key, cacheDir), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd >= 0)
    RealIO::instance().close(fd);
}
bool cacheFresh(const UrlKey& key, const std::string& cacheDir, int revalidateSeconds) {
  struct ::stat st;
  if (RealIO::instance().stat(valPath(key, cacheDir), &st) != 0)
    return false;
  uint64_t now = static_cast<uint64_t>(::time(nullptr));
  if (now > static_cast<uint64_t>(st.st_mtime) + static_cast<uint64_t>(revalidateSeconds))
    return false; // stale — revalidate against the origin
  struct ::stat ms; // a sidecar must exist for the entry to be usable
  return RealIO::instance().stat(key.metaPath(cacheDir), &ms) == 0;
}

AnyObject* vreadResponse(const ChunkList& chunks) {
  auto* info = new XrdCl::VectorReadInfo();
  uint32_t total = 0;
  for (const auto& c : chunks)
    total += c.length;
  info->GetChunks() = chunks;
  info->SetSize(total);
  auto* obj = new AnyObject();
  obj->Set(info);
  return obj;
}

} // namespace

//------------------------------------------------------------------ state --

// Lazy synchronous origin open (cache-freshness path only): a trusted-cache
// handle deferred the remote open; a genuine miss needs it now. Serialized by
// innerOpenMu, off the fast path; runs on a caller/executor thread (never an
// XrdCl callback thread), so the blocking Open here is allowed (§5.3).
// Failed attempts re-arm after a cool-down instead of latching (a transient
// dead-server reselection must not kill the handle for the rest of the job).
constexpr uint64_t kLazyOpenRetryCooldownUs = 1'000'000;

bool HandleState::ensureInnerOpen() {
  std::lock_guard<std::mutex> g(innerOpenMu);
  if (innerOpened)
    return innerOpenOk;
  if (lazyOpenFailUs && nowUs() - lazyOpenFailUs < kLazyOpenRetryCooldownUs)
    return false; // recent failure: rate-limit re-attempts, keep the error
  const Config& cfg = globalConfig();
  using OF = XrdCl::OpenFlags;
  // The lazy open is the PLUGIN'S cache-fill connection, not the client's:
  // strip client routing directives (XrdAdaptor's tried=/triedrc= exclusion
  // list) — inheriting them forced the origin to reselect data servers this
  // host cannot route to, on EVERY re-attempt (found under cmsRun).
  // Everything else (auth tokens, xrd.* options) passes through untouched.
  // The relayed (non-trusted) open path keeps the client URL verbatim: there
  // the inner connection IS the client's connection.
  const std::string fillUrl = stripCgiParams(url, {"tried", "triedrc"});
  XRootDStatus s = inner->Open(faultOpenUrl(fillUrl, ++lazyOpenAttempt), OF::Read,
                               XrdCl::Access::None, static_cast<uint16_t>(0));
  // Case B: bounded synchronous retry of a TRANSIENT lazy
  // open on a FRESH inner file (a failed open is terminal). The sleeps run on
  // this caller/executor thread under innerOpenMu — the deliberate asymmetry
  // with Case A's off-thread backoff: acceptable because this is the
  // degraded/miss path only and innerOpened latches, so a handle runs at most
  // one retry sequence. openRetries == 0 (default) skips the loop entirely.
  int retry = 1;
  for (; !s.IsOK() && retry <= cfg.openRetries && isRetryableOpen(s.code, s.errNo); ++retry) {
    if (store)
      store->stats().openRetries.fetch_add(1, std::memory_order_relaxed);
    std::this_thread::sleep_for(std::chrono::milliseconds(backoffMs(retry, cfg)));
    XrdCl::File* fresh = resetInner();
    s = fresh->Open(faultOpenUrl(fillUrl, ++lazyOpenAttempt), OF::Read, XrdCl::Access::None,
                    static_cast<uint16_t>(0));
  }
  if (!s.IsOK() && retry > 1 && store) // retried at least once and still failed
    store->stats().openRetriesExhausted.fetch_add(1, std::memory_order_relaxed);
  innerOpenOk = s.IsOK();
  if (innerOpenOk) {
    innerOpened = true; // success latches; failure only arms the cool-down
    lazyOpenFailUs = 0;
  } else {
    lazyOpenError = s;
    lazyOpenFailUs = nowUs();
    // A failed open is TERMINAL on the object: install a fresh file
    // now so the post-cool-down re-attempt doesn't replay the cached error.
    if (innerOps == 0)
      resetInner();
    UCACHE_WARN("lazy origin open failed for %s (%s); cache miss fails with that error, "
                "re-attempt after cool-down",
                url.c_str(), s.ToString().c_str());
  }
  return innerOpenOk;
}

XrdCl::XRootDStatus HandleState::missError() {
  std::lock_guard<std::mutex> g(innerOpenMu);
  if (lazyOpenFailUs && !lazyOpenError.IsOK())
    return lazyOpenError; // the origin's own error: retryable by smart clients
  return XrdCl::XRootDStatus(XrdCl::stError, XrdCl::errInvalidOp);
}

XrdCl::File* HandleState::acquireInner() {
  // Trusted-cache handle: open the origin on first real need (fail-open —
  // nullptr here makes the miss path return an error, never bad data).
  if (cacheOnly && !innerOpened && !ensureInnerOpen())
    return nullptr;
  std::lock_guard<std::mutex> g(mu);
  if (!innerValid)
    return nullptr;
  ++innerOps;
  return inner;
}

void HandleState::releaseInner() {
  std::lock_guard<std::mutex> g(mu);
  if (--innerOps == 0)
    innerCv.notify_all();
}

// Pass-through relay to the origin for ops the cache never serves (PgRead,
// legacy/beyond-EOF geometries): acquire the inner FIRST — on a trusted-cache
// handle (UCACHE_REVALIDATE_S) that lazy-opens the origin. A never-opened
// XrdCl::File must never receive I/O: 5.8.3's PgRead SEGVs probing the
// transport of a channel with no URL (found by plugin_fuzz the moment
// the freshness window became the default). The inner is released when the op
// completes; if no usable inner exists (lazy open failed, teardown) the op
// fails cleanly with errInvalidOp instead.
// Bytes served by pure pass-through — the cache never touched them.
// Counted at issue time (a failed relay overcounts a little; the counter is
// a tier-share indicator, not an accounting invariant).
static void noteRelayBytes(const std::shared_ptr<HandleState>& st, uint64_t n) {
  if (st->store && n)
    st->store->stats().relayBytes.fetch_add(n, std::memory_order_relaxed);
}

template <typename Issue>
static XrdCl::XRootDStatus relayToInner(const std::shared_ptr<HandleState>& st,
                                        XrdCl::ResponseHandler* user, Issue issue) {
  struct Relay : XrdCl::ResponseHandler {
    std::shared_ptr<HandleState> st;
    XrdCl::ResponseHandler* user;
    void HandleResponseWithHosts(XrdCl::XRootDStatus* s, XrdCl::AnyObject* r,
                                 XrdCl::HostList* h) override {
      st->releaseInner();
      if (user)
        user->HandleResponseWithHosts(s, r, h);
      else {
        delete s;
        delete r;
        delete h;
      }
      delete this;
    }
  };
  if (XrdCl::File* f = st->acquireInner()) {
    auto* relay = new Relay;
    relay->st = st;
    relay->user = user;
    XrdCl::XRootDStatus s = issue(f, static_cast<XrdCl::ResponseHandler*>(relay));
    if (!s.IsOK()) {
      delete relay;
      st->releaseInner();
    }
    return s;
  }
  return st->missError(); // the origin's own error when the lazy open failed
}

void HandleState::shutdownInner() {
  std::unique_ptr<XrdCl::File> dead;
  {
    std::unique_lock<std::mutex> lk(mu);
    innerValid = false;
    innerCv.wait(lk, [this] { return innerOps == 0; });
    inner = nullptr;
    dead = std::move(innerOwned);
  }
  // `dead`'s File dtor (a synchronous Close) runs here with mu released —
  // matching the old by-value member, destroyed after shutdownInner unlocked.
}

XrdCl::File* HandleState::resetInner() {
  // Swap the terminally-failed inner file for a fresh one — a failed open is
  // terminal on the object, so retry needs a new File.
  // Precondition innerOps == 0: no legitimate inner op is in flight before the
  // open has succeeded.
  std::unique_ptr<XrdCl::File> dead;
  XrdCl::File* fresh;
  {
    std::lock_guard<std::mutex> g(mu);
    dead = std::move(innerOwned);
    innerOwned = std::make_unique<XrdCl::File>(false); // plugins disabled (no recursion)
    inner = innerOwned.get();
    fresh = inner;
  }
  return fresh; // `dead` (terminal Error — its Close is a no-op) destroyed here
}

void HandleState::noteCacheError(const Config& cfg) {
  std::lock_guard<std::mutex> g(mu);
  if (store)
    store->stats().failopenEvents.fetch_add(1, std::memory_order_relaxed);
  if (++errors >= cfg.maxErrors && !tripped) {
    tripped = true;
    if (store)
      store->stats().disabledHandles.fetch_add(1, std::memory_order_relaxed);
    UCACHE_WARN("handle for %s tripped after %d consecutive cache errors; "
                "pass-through from now on",
                url.c_str(), errors);
  }
}

void HandleState::noteCacheOk() {
  std::lock_guard<std::mutex> g(mu);
  errors = 0;
}

void HandleState::beginPersist() {
  std::lock_guard<std::mutex> g(persistMu);
  ++pendingPersists;
}

void HandleState::endPersist() {
  std::lock_guard<std::mutex> g(persistMu);
  if (--pendingPersists == 0)
    persistCv.notify_all();
}

void HandleState::drainPersists() {
  std::unique_lock<std::mutex> lk(persistMu);
  persistCv.wait(lk, [this] { return pendingPersists == 0; });
}

//--------------------------------------------------------------- handlers --

namespace {

// (Cache entry setup is lazy + synchronous on the first read — see
// UCacheFile::ensureEntry. Reads issued before an async setup completed used
// to fall through uncached; lazy sync setup on the caller thread closes that
// race by construction.)

void persistPagesReserved(const std::shared_ptr<HandleState>& st,
                          const std::shared_ptr<FileEntry>& entry, uint64_t off,
                          std::shared_ptr<std::vector<char>> buf, uint64_t len,
                          uint64_t gateLen = 0) {
  Executor::instance().post([st, entry, off, buf, len, gateLen] {
    entry->writePages(off, len, buf->data());
    entry->flushMeta(false);
    if (gateLen) // waiters re-dispatch only AFTER the pages are staged
      entry->endFetch(off, gateLen);
    st->endPersist(); // wakes a draining Close/dtor when the last one lands
  });
}

// Single-Read miss: rounded wire read, user completion first, async persist.
class MissReadHandler : public ResponseHandler {
 public:
  // gateLen != 0: this fetch owns the (wireOff, gateLen) fetch-gate slot
  // (fetch dedup) and must release it on every exit — after staging on success,
  // immediately on failure (waiters then fetch for themselves).
  MissReadHandler(std::shared_ptr<HandleState> st, std::shared_ptr<FileEntry> entry,
                  uint64_t userOff, uint32_t userLen, void* userBuf,
                  ResponseHandler* user, uint64_t wireOff,
                  std::shared_ptr<std::vector<char>> wireBuf, uint64_t t0,
                  uint64_t gateLen = 0)
      : st_(std::move(st)), entry_(std::move(entry)), userOff_(userOff), userLen_(userLen),
        userBuf_(userBuf), user_(user), wireOff_(wireOff), wireBuf_(std::move(wireBuf)),
        t0_(t0), gateLen_(gateLen) {}

  void HandleResponseWithHosts(XRootDStatus* status, AnyObject* response,
                               HostList* hostList) override {
    st_->releaseInner();
    std::unique_ptr<XRootDStatus> s(status);
    std::unique_ptr<AnyObject> r(response);
    std::unique_ptr<HostList> h(hostList);

    ChunkInfo* ci = nullptr;
    if (s && s->IsOK() && r)
      r->Get(ci);
    uint64_t got = ci ? ci->length : 0;
    bool covers = ci && wireOff_ <= userOff_ && wireOff_ + got >= userOff_ + userLen_;

    if (!covers) {
      // Wire failure or short read on the rounded span: fail open — retry
      // the exact user request as pure pass-through, completing the user
      // from that operation faithfully (§5.2 step 5).
      if (gateLen_)
        entry_->endFetch(wireOff_, gateLen_); // waiters refetch on their own
      st_->noteCacheError(globalConfig());
      if (XrdCl::File* f = st_->acquireInner()) {
        struct Relay : ResponseHandler {
          std::shared_ptr<HandleState> st;
          ResponseHandler* user;
          void HandleResponseWithHosts(XRootDStatus* s2, AnyObject* r2,
                                       HostList* h2) override {
            st->releaseInner();
            if (user)
              user->HandleResponseWithHosts(s2, r2, h2);
            else {
              delete s2;
              delete r2;
              delete h2;
            }
            delete this;
          }
        };
        auto* relay = new Relay;
        relay->st = st_;
        relay->user = user_;
        XRootDStatus s2 = f->Read(userOff_, userLen_, userBuf_, relay, 0);
        if (s2.IsOK()) {
          delete this;
          return;
        }
        delete relay;
        st_->releaseInner();
        complete(user_, new XRootDStatus(s2), nullptr);
      } else {
        complete(user_,
                 s && !s->IsOK() ? new XRootDStatus(*s)
                                 : new XRootDStatus(XrdCl::stError, XrdCl::errInternal),
                 nullptr);
      }
      delete this;
      return;
    }

    // Serve the user first (memcpy only — no disk on this thread)...
    std::memcpy(userBuf_, wireBuf_->data() + (userOff_ - wireOff_), userLen_);
    if (st_->store) {
      auto& stats = st_->store->stats();
      stats.missBytes.fetch_add(userLen_, std::memory_order_relaxed);
      stats.servedBytes.fetch_add(userLen_, std::memory_order_relaxed);
      stats.originBytes.fetch_add(got, std::memory_order_relaxed);
      stats.originReads.fetch_add(1, std::memory_order_relaxed);
      stats.originRtUs.add(nowUs() - t0_);
      if (stats.tracer)
        stats.tracer->rec("wire", entry_->key().key, wireOff_, got, nowUs() - t0_);
    }
    st_->noteCacheOk();
    // Reserve the persist slot BEFORE completing the user, so a Close on the
    // just-woken app thread cannot race past a not-yet-counted persist.
    st_->beginPersist();
    complete(user_, okStatus(), chunkResponse(userOff_, userLen_, userBuf_));
    // ...then persist full pages asynchronously (§5.2 step 4). Ownership of
    // the wire buffer MOVES to the task: single owner, no cross-thread
    // refcount teardown (keeps TSan exact and the lifetime obvious).
    persistPagesReserved(st_, entry_, wireOff_, std::move(wireBuf_), got, gateLen_);
    delete this;
  }

 private:
  std::shared_ptr<HandleState> st_;
  std::shared_ptr<FileEntry> entry_;
  uint64_t userOff_;
  uint32_t userLen_;
  void* userBuf_;
  ResponseHandler* user_;
  uint64_t wireOff_;
  std::shared_ptr<std::vector<char>> wireBuf_;
  uint64_t t0_;
  uint64_t gateLen_;
};

// Issue the rounded wire read for a single-Read miss, honoring the test-only
// wire-read fault: the injected completion is posted to the executor, standing
// in for the XrdCl callback thread, so the handler runs its real error path.
XRootDStatus issueWireRead(XrdCl::File* f, uint64_t off, uint64_t len, void* buf,
                           MissReadHandler* mh, uint16_t timeout) {
  if (readFaultFire()) {
    Executor::instance().post([mh] {
      mh->HandleResponseWithHosts(
          new XRootDStatus(XrdCl::stError, XrdCl::errConnectionError), nullptr, nullptr);
    });
    return XRootDStatus();
  }
  return f->Read(off, len, buf, mh, timeout);
}

std::pair<uint64_t, uint64_t> roundChunk(const FileEntry& e, uint64_t off, uint64_t len);

// Parked-read re-dispatch: runs on the executor after the owning fetch
// released the gate. Usually a RAM hit against the just-staged pages; if the
// owner failed, fetches for itself (joining any newer in-flight fetch).
// Free function on (st, entry) by design — the UCacheFile object may be gone
// (executor tasks must never capture the plugin object).
void redispatchRead(std::shared_ptr<HandleState> st, std::shared_ptr<FileEntry> entry,
                    uint64_t offset, uint32_t size, void* buffer,
                    ResponseHandler* handler) {
  uint64_t t0 = nowUs();
  if (entry->readCached(offset, size, buffer)) {
    if (st->store) {
      auto& stats = st->store->stats();
      stats.servedBytes.fetch_add(size, std::memory_order_relaxed);
      stats.hitReadUs.add(nowUs() - t0);
      if (stats.tracer)
        stats.tracer->rec("ram", entry->key().key, offset, size, nowUs() - t0);
    }
    st->noteCacheOk();
    complete(handler, okStatus(), chunkResponse(offset, size, buffer));
    return;
  }
  auto [ws, we] = roundChunk(*entry, offset, size);
  auto fired = std::make_shared<std::atomic<bool>>(false);
  auto again = [st, entry, offset, size, buffer, handler, fired] {
    if (fired->exchange(true))
      return;
    Executor::instance().post([st, entry, offset, size, buffer, handler] {
      redispatchRead(st, entry, offset, size, buffer, handler);
    });
  };
  if (!entry->beginFetch(ws, we - ws, again)) {
    Executor::instance().postAfter(10000, again); // missed-endFetch insurance
    return;
  }
  auto wireBuf = std::make_shared<std::vector<char>>(we - ws);
  if (XrdCl::File* f = st->acquireInner()) {
    auto* mh = new MissReadHandler(st, entry, offset, size, buffer, handler, ws, wireBuf,
                                   nowUs(), we - ws);
    XRootDStatus s = issueWireRead(f, ws, we - ws, wireBuf->data(), mh, 0);
    if (!s.IsOK()) {
      delete mh;
      st->releaseInner();
      entry->endFetch(ws, we - ws);
      complete(handler, new XRootDStatus(s), nullptr);
    }
    return;
  }
  entry->endFetch(ws, we - ws);
  complete(handler, new XRootDStatus(st->missError()), nullptr);
}

struct WireElem {
  uint64_t off = 0;
  uint64_t len = 0;
  std::shared_ptr<std::vector<char>> buf;
};

// Vector-read miss stage: one wire VectorRead of rounded+coalesced ranges.
class MissVReadHandler : public ResponseHandler {
 public:
  MissVReadHandler(std::shared_ptr<HandleState> st, std::shared_ptr<FileEntry> entry,
                   ChunkList userChunks, std::vector<size_t> missIdx,
                   std::vector<WireElem> wire, ResponseHandler* user, uint64_t t0)
      : st_(std::move(st)), entry_(std::move(entry)), userChunks_(std::move(userChunks)),
        missIdx_(std::move(missIdx)), wire_(std::move(wire)), user_(user), t0_(t0) {}

  void HandleResponseWithHosts(XRootDStatus* status, AnyObject* response,
                               HostList* hostList) override {
    st_->releaseInner();
    std::unique_ptr<XRootDStatus> s(status);
    std::unique_ptr<AnyObject> r(response);
    std::unique_ptr<HostList> h(hostList);

    if (!s || !s->IsOK()) {
      // Wire failure: fail open — reissue the FULL original chunk list as
      // pure pass-through (hits included; wasteful but simple and faithful).
      st_->noteCacheError(globalConfig());
      if (XrdCl::File* f = st_->acquireInner()) {
        struct Relay : ResponseHandler {
          std::shared_ptr<HandleState> st;
          ResponseHandler* user;
          void HandleResponseWithHosts(XRootDStatus* s2, AnyObject* r2,
                                       HostList* h2) override {
            st->releaseInner();
            if (user)
              user->HandleResponseWithHosts(s2, r2, h2);
            else {
              delete s2;
              delete r2;
              delete h2;
            }
            delete this;
          }
        };
        auto* relay = new Relay;
        relay->st = st_;
        relay->user = user_;
        XRootDStatus s2 = f->VectorRead(userChunks_, nullptr, relay, 0);
        if (s2.IsOK()) {
          delete this;
          return;
        }
        delete relay;
        st_->releaseInner();
        complete(user_, new XRootDStatus(s2), nullptr);
      } else {
        complete(user_, new XRootDStatus(*s), nullptr);
      }
      delete this;
      return;
    }

    // Scatter wire bytes into the user's miss chunks (memcpy only).
    uint64_t missBytes = 0;
    for (size_t idx : missIdx_) {
      auto& c = userChunks_[idx];
      const WireElem* w = findWire(c.offset);
      std::memcpy(c.buffer, w->buf->data() + (c.offset - w->off), c.length);
      missBytes += c.length;
    }
    if (st_->store) {
      auto& stats = st_->store->stats();
      uint64_t wireBytes = 0;
      for (const auto& w : wire_)
        wireBytes += w.len;
      stats.missBytes.fetch_add(missBytes, std::memory_order_relaxed);
      stats.originBytes.fetch_add(wireBytes, std::memory_order_relaxed);
      stats.originReadvs.fetch_add(1, std::memory_order_relaxed);
      uint64_t total = 0;
      for (const auto& c : userChunks_)
        total += c.length;
      stats.servedBytes.fetch_add(total, std::memory_order_relaxed);
      stats.originRtUs.add(nowUs() - t0_);
      if (stats.tracer)
        stats.tracer->rec("wire", entry_->key().key, wire_.front().off, wireBytes,
                          nowUs() - t0_);
    }
    st_->noteCacheOk();
    // Reserve all persist slots before completing the user (see MissRead).
    for (size_t i = 0; i < wire_.size(); ++i)
      st_->beginPersist();
    complete(user_, okStatus(), vreadResponse(userChunks_));
    // Persist each wire element's full pages asynchronously; buffer
    // ownership moves to each task (single owner — see MissReadHandler).
    for (auto& w : wire_)
      persistPagesReserved(st_, entry_, w.off, std::move(w.buf), w.len);
    delete this;
  }

 private:
  const WireElem* findWire(uint64_t off) const {
    // wire_ is sorted by offset and covers every miss chunk's rounded range.
    const WireElem* best = &wire_.front();
    for (const auto& w : wire_)
      if (w.off <= off)
        best = &w;
      else
        break;
    return best;
  }

  std::shared_ptr<HandleState> st_;
  std::shared_ptr<FileEntry> entry_;
  ChunkList userChunks_;
  std::vector<size_t> missIdx_;
  std::vector<WireElem> wire_;
  ResponseHandler* user_;
  uint64_t t0_;
};

// Rounded [start,end) for a chunk; unrounded when rounding would exceed the
// readv element ceiling or run past EOF bookkeeping.
std::pair<uint64_t, uint64_t> roundChunk(const FileEntry& e, uint64_t off, uint64_t len) {
  const uint64_t P = e.pageSize();
  uint64_t start = off / P * P;
  uint64_t end = std::min<uint64_t>((off + len + P - 1) / P * P, e.fileSize());
  if (end - start > kMaxReadvElem)
    return {off, off + len};
  return {start, end};
}

// Issues the wire VectorRead for the miss stage. Runs on the caller's thread
// or an executor thread; touches inner only via acquire/release.
void issueMissVRead(const std::shared_ptr<HandleState>& st,
                    const std::shared_ptr<FileEntry>& entry, ChunkList userChunks,
                    std::vector<size_t> missIdx, ResponseHandler* user) {
  // Build rounded intervals in offset order, coalescing overlaps so the
  // origin never sees overlapping reads (§5.2 step 3).
  std::vector<std::pair<uint64_t, uint64_t>> ivals;
  ivals.reserve(missIdx.size());
  for (size_t idx : missIdx) {
    const auto& c = userChunks[idx];
    ivals.push_back(roundChunk(*entry, c.offset, c.length));
  }
  std::sort(ivals.begin(), ivals.end());
  std::vector<WireElem> wire;
  for (auto [s, e] : ivals) {
    if (!wire.empty() && s <= wire.back().off + wire.back().len) {
      uint64_t newEnd = std::max(wire.back().off + wire.back().len, e);
      wire.back().len = newEnd - wire.back().off;
    } else {
      wire.push_back({s, e - s, nullptr});
    }
  }
  ChunkList wireChunks;
  wireChunks.reserve(wire.size());
  for (auto& w : wire) {
    w.buf = std::make_shared<std::vector<char>>(w.len);
    wireChunks.emplace_back(w.off, static_cast<uint32_t>(w.len), w.buf->data());
  }

  XrdCl::File* f = st->acquireInner();
  if (!f) {
    complete(user, new XRootDStatus(st->missError()), nullptr);
    return;
  }
  auto* mh = new MissVReadHandler(st, entry, std::move(userChunks), std::move(missIdx),
                                  std::move(wire), user, nowUs());
  XRootDStatus s;
  if (readFaultFire()) // test hook: wire vector read dies mid-stream
    Executor::instance().post([mh] {
      mh->HandleResponseWithHosts(
          new XRootDStatus(XrdCl::stError, XrdCl::errConnectionError), nullptr, nullptr);
    });
  else
    s = f->VectorRead(wireChunks, nullptr, mh, 0);
  if (!s.IsOK()) {
    delete mh;
    st->releaseInner();
    complete(user, new XRootDStatus(s), nullptr);
  }
}

//---------------------------------------------------------- stitched view --

// Mid-handle overlay fault: the read errors faithfully —
// there is no origin to pass through to for overlay bytes — and the replica
// is dropped ONCE so subsequent opens get a clean v1 view. This handle keeps
// its (handle-stable) view; pages that still verify keep serving.
void replicaFault(const std::shared_ptr<HandleState>& st, const char* why) {
  UCACHE_WARN("replica overlay fault for %s (%s); read fails like a local file, "
              "replica dropped for future opens",
              st->url.c_str(), why);
  if (!st->replicaDropped.exchange(true)) {
    auto store = st->store;
    auto key = UrlKey::parse(st->url, globalConfig().keepCgi);
    if (store && key)
      Executor::instance().post([store, key] {
        ReplicaStore rs(RealIO::instance(), store->config(), store->stats());
        rs.drop(*key);
      });
  }
}

// Adapts the residual-fetch (vector) completion back into the original
// request's completion: the overlay/hit bytes are already in the user's
// buffer, so success just needs the original-shaped response.
class StitchAdapter : public ResponseHandler {
 public:
  // Single Read: respond with ChunkInfo(off, size, buf).
  StitchAdapter(ResponseHandler* user, uint64_t off, uint32_t size, void* buf)
      : user_(user), off_(off), size_(size), buf_(buf) {}
  // VectorRead: respond with the ORIGINAL chunk list.
  StitchAdapter(ResponseHandler* user, ChunkList original)
      : user_(user), original_(std::move(original)), isVRead_(true) {}

  void HandleResponseWithHosts(XRootDStatus* status, AnyObject* response,
                               HostList* hostList) override {
    std::unique_ptr<XRootDStatus> s(status);
    std::unique_ptr<AnyObject> r(response);
    std::unique_ptr<HostList> h(hostList);
    if (s && s->IsOK())
      complete(user_, okStatus(),
               isVRead_ ? vreadResponse(original_) : chunkResponse(off_, size_, buf_));
    else
      complete(user_, s ? new XRootDStatus(*s)
                        : new XRootDStatus(XrdCl::stError, XrdCl::errInternal),
               nullptr);
    delete this;
  }

 private:
  ResponseHandler* user_;
  uint64_t off_ = 0;
  uint32_t size_ = 0;
  void* buf_ = nullptr;
  ChunkList original_;
  bool isVRead_ = false;
};

// Serve one user chunk of the stitched view into `dest`: overlay segments
// from .tdata (verified), original segments from the v1 cache when present.
// Residual original sub-ranges are appended to `resid` for one wire fetch.
// Returns false on an overlay fault (caller errors the request). Runs on the
// executor (disk IO). Sub-chunk residuals are inherent to stitched entries —
// the v1 "never split chunks" invariant applies to plain entries only.
bool stitchChunk(const std::shared_ptr<HandleState>& st,
                 const std::shared_ptr<FileEntry>& entry,
                 const std::shared_ptr<ReplicaView>& view, uint64_t off, uint32_t len,
                 void* dest, ChunkList& resid, uint64_t& localBytes,
                 uint64_t& overlayBytes) {
  for (const auto& seg : view->map(off, len)) {
    char* d = static_cast<char*>(dest) + (seg.off - off);
    if (seg.overlay) {
      if (!view->read(seg.tdataOff, seg.len, d)) {
        replicaFault(st, "page CRC/IO failure");
        return false;
      }
      localBytes += seg.len;
      overlayBytes += seg.len; // replica-tier share, distinct from v1 hits
    } else if (seg.off + seg.len > entry->fileSize()) {
      replicaFault(st, "extent map gap past origin EOF"); // corrupt map
      return false;
    } else if (entry->hasRange(seg.off, seg.len) &&
               entry->readCached(seg.off, seg.len, d)) {
      localBytes += seg.len;
    } else {
      resid.emplace_back(seg.off, static_cast<uint32_t>(seg.len), d);
    }
  }
  return true;
}

// Full stitched service of Read/VectorRead-shaped requests on the executor.
void stitchedServe(std::shared_ptr<HandleState> st, std::shared_ptr<FileEntry> entry,
                   std::shared_ptr<ReplicaView> view, ChunkList userChunks,
                   bool isVRead, ResponseHandler* user) {
  ChunkList resid;
  uint64_t localBytes = 0, overlayBytes = 0;
  const uint64_t t0 = nowUs();
  for (const auto& c : userChunks)
    if (!stitchChunk(st, entry, view, c.offset, c.length, c.buffer, resid, localBytes,
                     overlayBytes)) {
      complete(user, new XRootDStatus(XrdCl::stError, XrdCl::errDataError), nullptr);
      return;
    }
  if (st->store && localBytes)
    st->store->stats().servedBytes.fetch_add(localBytes, std::memory_order_relaxed);
  if (st->store && overlayBytes) {
    auto& stats = st->store->stats();
    stats.replicaBytesServed.fetch_add(overlayBytes, std::memory_order_relaxed);
    stats.replicaReadUs.add(nowUs() - t0);
    entry->obs().replicaBytes.fetch_add(overlayBytes, std::memory_order_relaxed);
    if (stats.tracer)
      stats.tracer->rec("replica", entry->key().key, userChunks[0].offset, overlayBytes,
                        nowUs() - t0);
  }
  if (resid.empty()) {
    st->noteCacheOk();
    if (isVRead)
      complete(user, okStatus(), vreadResponse(userChunks));
    else
      complete(user, okStatus(),
               chunkResponse(userChunks[0].offset, userChunks[0].length,
                             userChunks[0].buffer));
    return;
  }
  // One wire fetch for every residual original sub-range, through the
  // existing rounded/coalesced/persisting miss machinery; the adapter
  // restores the original response shape.
  StitchAdapter* adapter =
      isVRead ? new StitchAdapter(user, userChunks)
              : new StitchAdapter(user, userChunks[0].offset, userChunks[0].length,
                                  userChunks[0].buffer);
  std::vector<size_t> idx(resid.size());
  for (size_t i = 0; i < idx.size(); ++i)
    idx[i] = i;
  issueMissVRead(st, entry, std::move(resid), std::move(idx), adapter);
}

} // namespace

//---------------------------------------------------------------- plugin ---

UCacheFile::UCacheFile() : st_(std::make_shared<HandleState>()) {
  st_->store = globalStore();
  {
    std::lock_guard<std::mutex> g(st_->mu);
    // The inner file is owned by HandleState (not as a plugin member): a posted
    // retry task swaps it and may run after Open returns, so it must reach the
    // file through st_ without capturing the plugin object.
    // enablePlugIns=false — prevents recursion.
    st_->innerOwned = std::make_unique<XrdCl::File>(false);
    st_->inner = st_->innerOwned.get();
    st_->innerValid = true;
  }
}

UCacheFile::~UCacheFile() {
  // Drain outstanding page persists so a short-lived process still leaves a
  // fully populated cache (Close normally does this; the dtor covers the
  // no-explicit-Close path). Not on any read's latency path.
  st_->drainPersists();
  std::shared_ptr<FileEntry> e;
  std::shared_ptr<ReplicaView> v;
  {
    std::lock_guard<std::mutex> g(st_->mu);
    st_->closed = true;
    e.swap(st_->entry);
    v.swap(st_->view);
  }
  if (e)
    e->flushAll(); // synchronous (covers the no-explicit-Close path)
  st_->shutdownInner();
}

// CPU-span evidence: process user+sys CPU in µs, and a counter of
// concurrently-open ucache handles (attribution is per-file only when the
// access pattern is sequential — overlap sets the `blended` flag).
uint64_t processCpuUs() {
  struct rusage ru;
  ::getrusage(RUSAGE_SELF, &ru);
  return static_cast<uint64_t>(ru.ru_utime.tv_sec) * 1000000u + ru.ru_utime.tv_usec +
         static_cast<uint64_t>(ru.ru_stime.tv_sec) * 1000000u + ru.ru_stime.tv_usec;
}
std::atomic<int> gOpenUCacheHandles{0};

// Write the entry's .cost sidecar (tiny, advisory: tmp+rename).
void writeCostSidecar(const std::string& url, const Config& cfg, uint64_t cpuUs, bool blended) {
  auto key = UrlKey::parse(url, cfg.keepCgi);
  if (!key || cpuUs == 0)
    return;
  const std::string base = key->objectDir(cfg.cacheDir) + "/" + key->hashHex;
  struct ::stat st;
  if (::stat((base + ".meta").c_str(), &st) != 0)
    return; // nothing cached
  char buf[96];
  int n = std::snprintf(buf, sizeof buf, "v1 cpu_us=%llu blended=%d\n",
                        static_cast<unsigned long long>(cpuUs), blended ? 1 : 0);
  const std::string tmp = base + ".cost.tmp";
  int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0)
    return;
  ssize_t r = ::write(fd, buf, static_cast<size_t>(n));
  ::close(fd);
  if (r == n)
    ::rename(tmp.c_str(), (base + ".cost").c_str());
  else
    ::unlink(tmp.c_str());
}

XrdCl::XRootDStatus UCacheFile::Open(const std::string& url, XrdCl::OpenFlags::Flags flags,
                                     XrdCl::Access::Mode mode, ResponseHandler* handler,
                                     uint16_t timeout) {
  st_->cpu0Us = processCpuUs(); // CPU-span start
  if (gOpenUCacheHandles.fetch_add(1) > 0)
    st_->cpuBlended = true; // another handle already open: spans overlap

  const Config& cfg = globalConfig();
  st_->url = url;
  using OF = XrdCl::OpenFlags;
  bool writey = flags & (OF::Update | OF::Write | OF::New | OF::Delete);
  passthroughOnly_ = writey || cfg.disable || !st_->store;
  if (writey && st_->store) {
    // §4.4: write access invalidates any cached entry for this URL.
    auto store = st_->store;
    auto key = UrlKey::parse(url, cfg.keepCgi);
    if (key)
      Executor::instance().post([store, key] { store->invalidate(*key); });
  }
  // Cache-freshness (UCACHE_REVALIDATE_S, default 7 days): if a
  // recent local entry is trusted, skip the remote open+stat entirely and
  // serve from cache — the origin is touched only if a genuine miss forces a
  // lazy open (fail-open). This is the reliability lever: warm reads don't
  // depend on a flaky/loaded/WAN remote. UCACHE_REVALIDATE_S=0 (revalidate
  // every open) never enters here.
  if (!passthroughOnly_ && cfg.revalidateSeconds > 0) {
    auto key = UrlKey::parse(url, cfg.keepCgi);
    if (key && cacheFresh(*key, cfg.cacheDir, cfg.revalidateSeconds)) {
      {
        std::lock_guard<std::mutex> g(st_->mu);
        st_->cacheOnly = true;
      }
      cacheOpen_ = true;
      UCACHE_INFO("trusted recent cache for %s; serving without remote open "
                  "(UCACHE_REVALIDATE_S=%d)", url.c_str(), cfg.revalidateSeconds);
      if (handler) {
        auto h = handler;
        Executor::instance().post([h] { complete(h, okStatus(), nullptr); });
      }
      return XRootDStatus();
    }
  }
  // The cache entry is created lazily on the first read (ensureEntry). With
  // open-retry enabled (opt-in) wrap the app handler so a TRANSIENT open failure
  // is retried on a fresh inner file (Case A); write-opens and
  // the null-handler case are never wrapped. Otherwise Open forwards unchanged.
  if (cfg.openRetries > 0 && !writey && handler) {
    auto* rh = new RetryingOpenHandler(st_, url, flags, mode, timeout, handler, cfg);
    return rh->start();
  }
  if (openFaultFirstN() > 0) // test hook only; inert (byte-identical) in production
    return st_->inner->Open(faultOpenUrl(url, 1), flags, mode, handler, timeout);
  return st_->inner->Open(url, flags, mode, handler, timeout);
}

// Runs on the caller's (app/IMT) thread — never an XrdCl callback thread —
// so the synchronous inner Stat here is allowed (§5.3). Serialized by
// setupMu so concurrent first-reads set up exactly once.
std::shared_ptr<FileEntry> UCacheFile::ensureEntry() {
  if (passthroughOnly_ || !st_->store)
    return nullptr;
  {
    std::lock_guard<std::mutex> g(st_->mu);
    if (st_->setupDone)
      return (st_->closed || st_->tripped) ? nullptr : st_->entry;
  }
  std::lock_guard<std::mutex> setup(st_->setupMu);
  {
    std::lock_guard<std::mutex> g(st_->mu);
    if (st_->setupDone)
      return (st_->closed || st_->tripped) ? nullptr : st_->entry;
  }
  const uint64_t setupT0 = nowUs(); // entry-setup span (sidecar load,
                                    // validation, view adoption) — the per-open
                                    // cost 65k opens multiply on a slow disk
  const Config& cfg = globalConfig();
  auto key = UrlKey::parse(st_->url, cfg.keepCgi);
  bool allowed = key &&
                 (cfg.allowHosts.empty() || hostMatchesAny(cfg.allowHosts, key->host)) &&
                 !hostMatchesAny(cfg.denyHosts, key->host);
  std::shared_ptr<FileEntry> entry;
  std::shared_ptr<ReplicaView> view;
  std::unique_ptr<XrdCl::StatInfo> statClone;

  bool cacheOnly;
  {
    std::lock_guard<std::mutex> g(st_->mu);
    cacheOnly = st_->cacheOnly;
  }

  // Trusted recent cache (UCACHE_REVALIDATE_S): build from the local sidecar,
  // NO remote stat. If the entry is gone (evicted since Open), degrade to the
  // origin path below.
  bool degraded = false;
  if (cacheOnly && allowed) {
    if (auto meta = MetaFile::load(RealIO::instance(), key->metaPath(cfg.cacheDir));
        meta && meta->key == key->key) {
      entry = st_->store->open(*key, meta->fileSize, meta->originMtime, meta->cksumKind,
                               meta->originCksum);
      if (entry) {
        statClone = std::make_unique<XrdCl::StatInfo>("0", meta->fileSize,
                                                      XrdCl::StatInfo::IsReadable,
                                                      meta->originMtime);
        if (cfg.transpose) {
          ReplicaStore rs(RealIO::instance(), cfg, st_->store->stats());
          view = rs.openView(*key, meta->fileSize, meta->originMtime, meta->cksumKind,
                             meta->originCksum);
          if (view)
            statClone->SetSize(view->virtualSize());
        }
      }
    }
    if (!entry) {
      std::lock_guard<std::mutex> g(st_->mu);
      st_->cacheOnly = false; // no usable trusted cache — hit the origin once
      degraded = true;
    }
  } else if (cacheOnly) {
    // Trusted at Open but the key is no longer allowed: degrade so the lazy
    // origin open below gives the handle a real pass-through target.
    std::lock_guard<std::mutex> g(st_->mu);
    st_->cacheOnly = false;
    degraded = true;
  }

  // Origin path: the default (revalidate every open), or a degraded trusted
  // handle. When degraded, the inner file was never opened at Open — open it
  // lazily now (best-effort; a failed open just yields pass-through).
  // `degraded` must re-enable this block: `cacheOnly` is the pre-degrade
  // snapshot, and gating on it alone left the handle entry-less on an
  // unopened inner file — every read failed errInvalidOp (found by the
  // revalidate degrade-race gate).
  if (!entry && (!cacheOnly || degraded)) {
    if (degraded)
      st_->ensureInnerOpen();
    XrdCl::StatInfo* si = nullptr;
    XRootDStatus s = st_->inner->Stat(false, si); // synchronous — caller thread
    std::unique_ptr<XrdCl::StatInfo> sip(si);
    if (s.IsOK() && si && allowed) {
      statClone = std::make_unique<XrdCl::StatInfo>(*si);
      entry = st_->store->open(*key, si->GetSize(), si->GetModTime());
      if (!entry)
        st_->store->stats().failopenEvents.fetch_add(1, std::memory_order_relaxed);
      // Transposed replica: adopt the stitched view when one
      // validates + fully verifies (openView); the reported size becomes
      // the stitched fEND' so ROOT's fBEGIN <= fEND <= size check holds.
      if (entry && cfg.transpose) {
        ReplicaStore rs(RealIO::instance(), cfg, st_->store->stats());
        view = rs.openView(*key, si->GetSize(), si->GetModTime());
        if (view) {
          statClone->SetSize(view->virtualSize());
          UCACHE_INFO("serving stitched replica view for %s (virtual %llu bytes)",
                      key->key.c_str(),
                      static_cast<unsigned long long>(view->virtualSize()));
        }
      }
      // Record the validation time so a later open within the window can trust
      // the cache and skip the origin (UCACHE_REVALIDATE_S).
      if (entry && cfg.revalidateSeconds > 0)
        touchVal(*key, cfg.cacheDir);
    }
  }
  if (st_->store) {
    auto& stats = st_->store->stats();
    stats.openUs.add(nowUs() - setupT0);
    if (stats.tracer && entry)
      stats.tracer->rec("open", entry->key().key, 0, 0, nowUs() - setupT0,
                        /*sampled=*/false);
  }
  std::lock_guard<std::mutex> g(st_->mu);
  st_->setupDone = true;
  st_->statInfo = std::move(statClone);
  if (!st_->closed && !st_->tripped) {
    st_->entry = entry;
    st_->view = view;
  }
  return st_->entry;
}

std::shared_ptr<ReplicaView> UCacheFile::currentView() const {
  std::lock_guard<std::mutex> g(st_->mu);
  return st_->view;
}


// ---- Background recompression trigger (recompress = on) -------------------
// At Close, an entry with cached data and no replica gets its URL appended to
// <dir>/recompress.pending, and a nice'd drainer helper (`ucache recompress
// --drain`) is spawned, rate-limited per process. The helper is our own CLI:
// found via UCACHE_RECOMPRESS_HELPER (tests) or PATH; absent helper = the
// queue simply waits for the next `ucache recompress` (fail-open, as ever).
// Runs on the executor, never on the Close path itself.
void queueRecompress(const std::string& url, const Config& cfg) {
  auto key = UrlKey::parse(url, cfg.keepCgi);
  if (!key)
    return;
  struct ::stat st;
  if (::stat(ReplicaStore::tmetaPath(*key, cfg.cacheDir).c_str(), &st) == 0)
    return; // replica already exists
  if (::stat((key->objectDir(cfg.cacheDir) + "/" + key->hashHex + ".meta").c_str(), &st) != 0)
    return; // nothing cached — nothing to recompress
  const std::string pending = cfg.cacheDir + "/recompress.pending";
  int fd = ::open(pending.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
  if (fd < 0)
    return;
  std::string line = url + "\n";
  ssize_t r = ::write(fd, line.data(), line.size()); // O_APPEND: atomic line
  (void)r;
  ::close(fd);

  // Spawn the drainer at most every 15 s per process; the drainer's own flock
  // makes concurrent spawns harmless (they exit immediately).
  static std::atomic<uint64_t> lastSpawn{0};
  uint64_t now = nowUs();
  uint64_t prev = lastSpawn.load(std::memory_order_relaxed);
  if (prev && now - prev < 15'000'000)
    return;
  if (!lastSpawn.compare_exchange_strong(prev, now))
    return;
  const char* helper = ::getenv("UCACHE_RECOMPRESS_HELPER");
  std::string exe = helper && *helper ? helper : "ucache";
  pid_t pid = 0;
  const char* argv[] = {exe.c_str(), "recompress", "--drain", nullptr};
  // Fully detach the drainer from the user's terminal: stdin from
  // /dev/null, stdout+stderr appended to recompress.log — a background worker
  // must never print onto whatever the user is doing. Spawn proceeds without
  // the redirections only if file_actions setup itself fails (fail-open).
  posix_spawn_file_actions_t fa;
  bool haveFa = ::posix_spawn_file_actions_init(&fa) == 0;
  if (haveFa) {
    const std::string log = cfg.cacheDir + "/recompress.log";
    ::posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    ::posix_spawn_file_actions_addopen(&fa, 1, log.c_str(), O_WRONLY | O_APPEND | O_CREAT, 0644);
    ::posix_spawn_file_actions_adddup2(&fa, 1, 2);
  }
  if (::posix_spawnp(&pid, exe.c_str(), haveFa ? &fa : nullptr, nullptr,
                     const_cast<char**>(argv), environ) == 0)
    UCACHE_INFO("recompress: queued %s; drainer pid %d", url.c_str(), static_cast<int>(pid));
  if (haveFa)
    ::posix_spawn_file_actions_destroy(&fa);
}

XrdCl::XRootDStatus UCacheFile::Close(ResponseHandler* handler, uint16_t timeout) {
  // Wait for queued page persists before flushing meta, so the cache is
  // complete on disk when the process (which may exit right after Close)
  // goes away. This blocks Close, never a read (§5.2 step 4).
  st_->drainPersists();
  std::shared_ptr<FileEntry> e;
  std::shared_ptr<ReplicaView> v;
  {
    std::lock_guard<std::mutex> g(st_->mu);
    st_->closed = true;
    e.swap(st_->entry);
    v.swap(st_->view); // releases the overlay fd
  }
  if (e)
    e->flushAll(); // synchronous: staged pages + bitmap must hit disk before we may exit
  // Background recompression: a closed entry's read set is complete —
  // queue it for the drainer. Off the Close path (executor task); captures
  // only values, never the plugin object.
  if (gOpenUCacheHandles.fetch_sub(1) > 1)
    st_->cpuBlended = true; // handles still open: the tail of this span overlaps
  if (e && st_->cpu0Us) {
    uint64_t cpu = processCpuUs() - st_->cpu0Us;
    std::string url = st_->url;
    bool blended = st_->cpuBlended;
    Executor::instance().post(
        [url, cpu, blended] { writeCostSidecar(url, globalConfig(), cpu, blended); });
  }
  if (e && !v && globalConfig().recompress) {
    std::string url = st_->url;
    Executor::instance().post([url] { queueRecompress(url, globalConfig()); });
  }
  // A trusted cache-only handle that never hit a miss never opened the origin;
  // there is nothing to close remotely (UCACHE_REVALIDATE_S).
  if (!st_->inner->IsOpen()) {
    if (handler)
      complete(handler, okStatus(), nullptr);
    return XRootDStatus();
  }
  return st_->inner->Close(handler, timeout);
}

XrdCl::XRootDStatus UCacheFile::Stat(bool force, ResponseHandler* handler,
                                     uint16_t timeout) {
  // A trusted cache-only handle (UCACHE_REVALIDATE_S) answers Stat from local
  // metadata for BOTH force values — the whole point is to not touch the
  // origin. If setup degrades (entry gone), it opens the origin and we fall
  // through to the normal path.
  if (cacheOpen_) {
    ensureEntry();
    std::lock_guard<std::mutex> g(st_->mu);
    if (st_->cacheOnly && st_->statInfo && !st_->closed) {
      auto* obj = new AnyObject();
      obj->Set(new XrdCl::StatInfo(*st_->statInfo));
      complete(handler, okStatus(), obj);
      return XRootDStatus();
    }
  }
  if (!force) {
    // Set up eagerly (caller thread, like the first read): ROOT may Stat
    // before reading, and a transposed entry must report the stitched size
    // from the very first answer.
    if (st_->inner->IsOpen() || cacheOpen_)
      ensureEntry();
    std::lock_guard<std::mutex> g(st_->mu);
    // Serve from validated metadata: only once the entry is
    // attached (validation done) and a StatInfo clone is on hand.
    if (st_->entry && st_->statInfo && !st_->closed) {
      auto* obj = new AnyObject();
      obj->Set(new XrdCl::StatInfo(*st_->statInfo));
      complete(handler, okStatus(), obj);
      return XRootDStatus();
    }
  } else if (auto view = currentView()) {
    // force=true forwards, but the size a transposed handle reports must
    // stay the stitched one — the origin doesn't know about the extension.
    struct SizeRelay : ResponseHandler {
      ResponseHandler* user;
      uint64_t vsize;
      void HandleResponseWithHosts(XRootDStatus* s, AnyObject* r, HostList* h) override {
        if (s && s->IsOK() && r) {
          XrdCl::StatInfo* si = nullptr;
          r->Get(si);
          if (si)
            si->SetSize(vsize);
        }
        if (user)
          user->HandleResponseWithHosts(s, r, h);
        else {
          delete s;
          delete r;
          delete h;
        }
        delete this;
      }
    };
    auto* relay = new SizeRelay;
    relay->user = handler;
    relay->vsize = view->virtualSize();
    XRootDStatus s = st_->inner->Stat(force, relay, timeout);
    if (!s.IsOK())
      delete relay;
    return s;
  }
  return st_->inner->Stat(force, handler, timeout);
}

XrdCl::XRootDStatus UCacheFile::Read(uint64_t offset, uint32_t size, void* buffer,
                                     ResponseHandler* handler, uint16_t timeout) {
  auto entry = ensureEntry();
  if (auto view = currentView(); entry && view) {
    // Stitched entry: serve on the executor — overlay + v1
    // cache locally, residual original sub-ranges via the miss machinery.
    if (size == 0 || offset + size > view->virtualSize()) {
      noteRelayBytes(st_, size);
      return relayToInner(st_, handler, [=](XrdCl::File* f, ResponseHandler* rh) {
        return f->Read(offset, size, buffer, rh, timeout); // garbage in, origin's answer out
      });
    }
    auto st = st_;
    ChunkList one;
    one.emplace_back(offset, size, buffer);
    Executor::instance().post([st, entry, view, one = std::move(one), handler]() mutable {
      stitchedServe(st, entry, view, std::move(one), /*isVRead=*/false, handler);
    });
    return XRootDStatus();
  }
  if (!entry || size == 0 || offset + size > entry->fileSize()) {
    noteRelayBytes(st_, size);
    return relayToInner(st_, handler, [=](XrdCl::File* f, ResponseHandler* rh) {
      return f->Read(offset, size, buffer, rh, timeout);
    });
  }

  if (entry->hasRange(offset, size)) {
    // HIT: disk read + verification on the executor (§5.3).
    auto st = st_;
    Executor::instance().post([st, entry, offset, size, buffer, handler] {
      uint64_t t0 = nowUs();
      if (entry->readCached(offset, size, buffer)) {
        if (st->store) {
          auto& stats = st->store->stats();
          stats.servedBytes.fetch_add(size, std::memory_order_relaxed);
          stats.hitReadUs.add(nowUs() - t0);
          if (stats.tracer)
            stats.tracer->rec("hit", entry->key().key, offset, size, nowUs() - t0);
        }
        st->noteCacheOk();
        complete(handler, okStatus(), chunkResponse(offset, size, buffer));
        return;
      }
      // CRC quarantine: demote to wire miss (counted by core).
      auto [ws, we] = roundChunk(*entry, offset, size);
      auto wireBuf = std::make_shared<std::vector<char>>(we - ws);
      if (XrdCl::File* f = st->acquireInner()) {
        auto* mh = new MissReadHandler(st, entry, offset, size, buffer, handler, ws,
                                       wireBuf, nowUs());
        XRootDStatus s = issueWireRead(f, ws, we - ws, wireBuf->data(), mh, 0);
        if (!s.IsOK()) {
          delete mh;
          st->releaseInner();
          complete(handler, new XRootDStatus(s), nullptr);
        }
      } else {
        complete(handler, new XRootDStatus(st->missError()), nullptr);
      }
    });
    return XRootDStatus();
  }

  // MISS from the caller's thread: rounded wire read. Dedup: an identical
  // rounded miss already in flight (another handle/thread — RDF-IMT re-reads
  // the same baskets constantly) parks this read instead of duplicating the
  // fetch; the owner's staged pages then serve it from RAM.
  auto [ws, we] = roundChunk(*entry, offset, size);
  {
    auto st = st_;
    auto fired = std::make_shared<std::atomic<bool>>(false);
    auto redispatch = [st, entry, offset, size, buffer, handler, fired] {
      if (fired->exchange(true))
        return;
      Executor::instance().post([st, entry, offset, size, buffer, handler] {
        redispatchRead(st, entry, offset, size, buffer, handler);
      });
    };
    if (!entry->beginFetch(ws, we - ws, redispatch)) {
      Executor::instance().postAfter(10000, redispatch); // missed-endFetch insurance
      return XRootDStatus();
    }
  }
  auto wireBuf = std::make_shared<std::vector<char>>(we - ws);
  if (XrdCl::File* f = st_->acquireInner()) {
    auto* mh = new MissReadHandler(st_, entry, offset, size, buffer, handler, ws, wireBuf,
                                   nowUs(), we - ws);
    XRootDStatus s = issueWireRead(f, ws, we - ws, wireBuf->data(), mh, timeout);
    if (!s.IsOK()) {
      delete mh;
      st_->releaseInner();
      entry->endFetch(ws, we - ws);
      return s;
    }
    return XRootDStatus();
  }
  // No usable inner (teardown, or a trusted handle whose lazy origin open
  // failed): fail with the ORIGIN's error when there is one — smart clients
  // (CMSSW XrdAdaptor) classify and retry it; errInvalidOp is terminal there.
  entry->endFetch(ws, we - ws);
  return st_->missError();
}

// §4.7: PgRead is pure pass-through, never cached, never served from cache —
// ROOT's remote-read path is Read/VectorRead (F5); xrdcp's pgread traffic is
// deliberately uncached in v1. On a trusted-cache handle (UCACHE_REVALIDATE_S,
// default-on) the relay lazy-opens the origin first
// (relayToInner). EXCEPTION: a transposed handle
// must never pass PgRead through — the origin lacks the extension bytes and
// has stale patched windows — so it is served from the stitched view with
// locally computed kXR page checksums (crc32c, same as the wire protocol).
XrdCl::XRootDStatus UCacheFile::PgRead(uint64_t offset, uint32_t size, void* buffer,
                                       ResponseHandler* handler, uint16_t timeout) {
  auto entry = ensureEntry();
  if (auto view = currentView(); entry && view) {
    if (size == 0 || offset + size > view->virtualSize()) {
      noteRelayBytes(st_, size);
      return relayToInner(st_, handler, [=](XrdCl::File* f, ResponseHandler* rh) {
        return f->PgRead(offset, size, buffer, rh, timeout);
      });
    }
    struct PgAdapter : ResponseHandler {
      ResponseHandler* user;
      uint64_t off;
      uint32_t size;
      void* buf;
      void HandleResponseWithHosts(XRootDStatus* s, AnyObject* r, HostList* h) override {
        std::unique_ptr<AnyObject> rr(r); // ChunkInfo response; rebuilt below
        std::unique_ptr<HostList> hh(h);
        if (s && s->IsOK()) {
          std::vector<uint32_t> cks;
          uint64_t pos = off;
          const auto* p = static_cast<const uint8_t*>(buf);
          while (pos < off + size) {
            uint64_t pageEnd = std::min<uint64_t>((pos / 4096 + 1) * 4096, off + size);
            cks.push_back(crc32c(p + (pos - off), pageEnd - pos));
            pos = pageEnd;
          }
          auto* obj = new AnyObject();
          obj->Set(new XrdCl::PageInfo(off, size, buf, std::move(cks)));
          complete(user, s, obj);
        } else {
          complete(user, s ? s : new XRootDStatus(XrdCl::stError, XrdCl::errInternal),
                   nullptr);
        }
        delete this;
      }
    };
    auto* pg = new PgAdapter;
    pg->user = handler;
    pg->off = offset;
    pg->size = size;
    pg->buf = buffer;
    auto st = st_;
    ChunkList one;
    one.emplace_back(offset, size, buffer);
    Executor::instance().post([st, entry, view, one = std::move(one), pg]() mutable {
      stitchedServe(st, entry, view, std::move(one), /*isVRead=*/false, pg);
    });
    return XRootDStatus();
  }
  noteRelayBytes(st_, size);
  return relayToInner(st_, handler, [=](XrdCl::File* f, ResponseHandler* rh) {
    return f->PgRead(offset, size, buffer, rh, timeout);
  });
}

XrdCl::XRootDStatus UCacheFile::VectorRead(const ChunkList& chunks, void* buffer,
                                           ResponseHandler* handler, uint16_t timeout) {
  auto entry = ensureEntry();
  auto chunkSum = [&chunks] {
    uint64_t n = 0;
    for (const auto& c : chunks)
      n += c.length;
    return n;
  };
  // The combined-buffer variant is legacy and rare: pass through unchanged.
  if (!entry || buffer || chunks.empty()) {
    noteRelayBytes(st_, chunkSum());
    return relayToInner(st_, handler, [&](XrdCl::File* f, ResponseHandler* rh) {
      return f->VectorRead(chunks, buffer, rh, timeout);
    });
  }

  if (auto view = currentView()) {
    // Stitched entry: whole vector served on the executor.
    for (const auto& c : chunks)
      if (c.length == 0 || c.offset + c.length > view->virtualSize()) {
        noteRelayBytes(st_, chunkSum());
        return relayToInner(st_, handler, [&](XrdCl::File* f, ResponseHandler* rh) {
          return f->VectorRead(chunks, buffer, rh, timeout);
        });
      }
    auto st = st_;
    if (st_->store)
      st_->store->stats().readvChunks.fetch_add(chunks.size(), std::memory_order_relaxed);
    ChunkList userChunks = chunks;
    Executor::instance().post(
        [st, entry, view, userChunks = std::move(userChunks), handler]() mutable {
          stitchedServe(st, entry, view, std::move(userChunks), /*isVRead=*/true, handler);
        });
    return XRootDStatus();
  }

  for (const auto& c : chunks)
    if (c.length == 0 || c.offset + c.length > entry->fileSize()) {
      noteRelayBytes(st_, chunkSum());
      return relayToInner(st_, handler, [&](XrdCl::File* f, ResponseHandler* rh) {
        return f->VectorRead(chunks, buffer, rh, timeout);
      });
    }

  // Atomic per-chunk classification (§5.2 step 1).
  std::vector<size_t> missIdx;
  for (size_t i = 0; i < chunks.size(); ++i)
    if (!entry->hasRange(chunks[i].offset, chunks[i].length))
      missIdx.push_back(i);
  if (st_->store) {
    auto& stats = st_->store->stats();
    stats.readvChunks.fetch_add(chunks.size(), std::memory_order_relaxed);
    if (!missIdx.empty() && missIdx.size() != chunks.size())
      stats.readvMixed.fetch_add(1, std::memory_order_relaxed); // the serial hit+wire shape
  }

  if (missIdx.size() == chunks.size()) {
    // All-miss: no disk stage; issue the wire read from this thread.
    issueMissVRead(st_, entry, chunks, std::move(missIdx), handler);
    return XRootDStatus();
  }

  // Hit stage on the executor; misses (incl. CRC demotions) follow as one
  // wire vector read.
  auto st = st_;
  ChunkList userChunks = chunks;
  Executor::instance().post(
      [st, entry, userChunks = std::move(userChunks), missIdx = std::move(missIdx),
       handler]() mutable {
        uint64_t t0 = nowUs();
        uint64_t hitBytes = 0;
        std::vector<size_t> misses = std::move(missIdx);
        std::vector<bool> isMiss(userChunks.size(), false);
        for (size_t i : misses)
          isMiss[i] = true;
        for (size_t i = 0; i < userChunks.size(); ++i) {
          if (isMiss[i])
            continue;
          const auto& c = userChunks[i];
          if (entry->readCached(c.offset, c.length, c.buffer)) {
            hitBytes += c.length;
          } else {
            misses.push_back(i); // CRC demotion
          }
        }
        if (st->store && hitBytes)
          st->store->stats().hitReadUs.add(nowUs() - t0);
        if (misses.empty()) {
          if (st->store) {
            uint64_t total = 0;
            for (const auto& c : userChunks)
              total += c.length;
            st->store->stats().servedBytes.fetch_add(total, std::memory_order_relaxed);
          }
          st->noteCacheOk();
          complete(handler, okStatus(), vreadResponse(userChunks));
          return;
        }
        std::sort(misses.begin(), misses.end());
        issueMissVRead(st, entry, std::move(userChunks), std::move(misses), handler);
      });
  return XRootDStatus();
}

void UCacheFile::invalidateOnWrite() {
  std::shared_ptr<FileEntry> e;
  std::shared_ptr<ReplicaView> v;
  {
    std::lock_guard<std::mutex> g(st_->mu);
    e.swap(st_->entry);
    v.swap(st_->view); // a written-to URL has no valid replica (§4.4)
    st_->tripped = true; // no caching on this handle after a write
  }
  if (st_->store) {
    auto store = st_->store;
    auto key = UrlKey::parse(st_->url, globalConfig().keepCgi);
    if (key)
      Executor::instance().post([store, key] { store->invalidate(*key); });
  }
}

XrdCl::XRootDStatus UCacheFile::Write(uint64_t offset, uint32_t size, const void* buffer,
                                      ResponseHandler* handler, uint16_t timeout) {
  invalidateOnWrite();
  return st_->inner->Write(offset, size, buffer, handler, timeout);
}

XrdCl::XRootDStatus UCacheFile::Write(uint64_t offset, XrdCl::Buffer&& buffer,
                                      ResponseHandler* handler, uint16_t timeout) {
  invalidateOnWrite();
  return st_->inner->Write(offset, std::move(buffer), handler, timeout);
}

XrdCl::XRootDStatus UCacheFile::VectorWrite(const ChunkList& chunks,
                                            ResponseHandler* handler, uint16_t timeout) {
  invalidateOnWrite();
  return st_->inner->VectorWrite(chunks, handler, timeout);
}

XrdCl::XRootDStatus UCacheFile::WriteV(uint64_t offset, const struct iovec* iov, int iovcnt,
                                       ResponseHandler* handler, uint16_t timeout) {
  invalidateOnWrite();
  return st_->inner->WriteV(offset, iov, iovcnt, handler, timeout);
}

XrdCl::XRootDStatus UCacheFile::Sync(ResponseHandler* handler, uint16_t timeout) {
  return st_->inner->Sync(handler, timeout);
}

XrdCl::XRootDStatus UCacheFile::Truncate(uint64_t size, ResponseHandler* handler,
                                         uint16_t timeout) {
  invalidateOnWrite();
  return st_->inner->Truncate(size, handler, timeout);
}

XrdCl::XRootDStatus UCacheFile::Fcntl(const XrdCl::Buffer& arg, ResponseHandler* handler,
                                      uint16_t timeout) {
  return st_->inner->Fcntl(arg, handler, timeout);
}

XrdCl::XRootDStatus UCacheFile::Visa(ResponseHandler* handler, uint16_t timeout) {
  return st_->inner->Visa(handler, timeout);
}

bool UCacheFile::IsOpen() const { return cacheOpen_ || st_->inner->IsOpen(); }

bool UCacheFile::SetProperty(const std::string& name, const std::string& value) {
  return st_->inner->SetProperty(name, value);
}

bool UCacheFile::GetProperty(const std::string& name, std::string& value) const {
  if (st_->inner->GetProperty(name, value) && !value.empty())
    return true;
  // Trusted cache-only handles never opened a transport, so the properties
  // clients act on are unset. Native XrdCl has a contract for exactly this
  // situation — the LOCAL REDIRECT: LastURL = file://localhost/<path> makes
  // both ROOT (TNetXNGFile::GetVectorReadLimits) and uproot
  // (get_server_config) take their local-file branch: parseable URL (uproot
  // dies on None), sane readv defaults, and NO follow-up server query — a
  // warm open must never touch the network (an offline-warm leg gates this;
  // synthesizing a real host here hung offline warm reads on the clients'
  // FileSystem config query). DataServer stays unset for the same reason:
  // both clients default gracefully on its absence, and any real value
  // invites contact with an origin that may be down. The path is the URL's
  // (never the cache's .data file: a sparse page file read directly would
  // be silent corruption; this path fails loudly if anything opens it).
  if (name == "LastURL") {
    std::lock_guard<std::mutex> g(st_->mu);
    if (!st_->url.empty()) {
      value = "file://localhost" + XrdCl::URL(st_->url).GetPath();
      return true;
    }
  }
  return false;
}

} // namespace ucache
