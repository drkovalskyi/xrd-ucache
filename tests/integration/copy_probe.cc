// Copies made through the plugin are the origin's bytes, and a reader in the
// same process still caches.
//
//   copy_probe <root-url> <local-path>
//
// Needs XRD_PLUGINCONFDIR binding the URL's host to the plugin, and UCACHE_DIR
// naming a cache directory that holds no entries yet. <local-path> is the same
// file as the origin serves it.
//
// The work runs in ONE child process (a fresh image of this program), so a
// copier and a reader share it:
//   1. XrdCl::CopyProcess copies the URL with the copy engine's default read
//      path: identical to the local file, and no cache entry appears;
//   2. a plain XrdCl::File read loop over the same URL caches it as usual: an
//      entry appears, holding data;
//   3. a second copy, through the copy engine's plain-read path, while the
//      file is cached: identical, and the cached entry is left as it was.
// The child then exits normally, which writes its stats line, and the parent
// checks that both copies were counted as copies and that the reader's fill
// was the only thing written. A plugin that failed to load passes steps 1 and 3
// on its own, which is why the count is checked.
#include <XrdCl/XrdClCopyProcess.hh>
#include <XrdCl/XrdClDefaultEnv.hh>
#include <XrdCl/XrdClFile.hh>
#include <XrdCl/XrdClPropertyList.hh>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <iterator>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {

int fails = 0;
void check(bool ok, const char* what) {
  std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok)
    ++fails;
}

std::vector<char> slurp(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<char>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

std::vector<std::string> listDir(const std::string& dir) {
  std::vector<std::string> out;
  if (DIR* d = ::opendir(dir.c_str())) {
    while (dirent* e = ::readdir(d))
      if (e->d_name[0] != '.')
        out.push_back(e->d_name);
    ::closedir(d);
  }
  return out;
}

bool endsWith(const std::string& s, const std::string& tail) {
  return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

// Cache files ending in `ext` (".meta", ".data") under <cache>/objects/*/.
std::vector<std::string> cacheFiles(const std::string& cache, const std::string& ext) {
  std::vector<std::string> out;
  for (const auto& sub : listDir(cache + "/objects"))
    for (const auto& f : listDir(cache + "/objects/" + sub))
      if (endsWith(f, ext))
        out.push_back(cache + "/objects/" + sub + "/" + f);
  return out;
}

bool copy(const std::string& url, const std::string& dst) {
  XrdCl::CopyProcess process;
  XrdCl::PropertyList props, results;
  props.Set("source", url);
  props.Set("target", dst);
  props.Set("force", true);
  if (!process.AddJob(props, &results).IsOK() || !process.Prepare().IsOK())
    return false;
  return process.Run(nullptr).IsOK();
}

// Read the whole file through an ordinary handle, in 1 MiB requests.
bool readAll(const std::string& url, std::vector<char>& out) {
  XrdCl::File f;
  if (!f.Open(url, XrdCl::OpenFlags::Read).IsOK())
    return false;
  std::vector<char> buf(1 << 20);
  uint64_t off = 0;
  for (;;) {
    uint32_t got = 0;
    if (!f.Read(off, static_cast<uint32_t>(buf.size()), buf.data(), got).IsOK()) {
      const XrdCl::XRootDStatus closed = f.Close(); // the read's failure is the answer
      (void)closed;
      return false;
    }
    if (got == 0)
      break;
    out.insert(out.end(), buf.data(), buf.data() + got);
    off += got;
  }
  return f.Close().IsOK();
}

int child(const std::string& url, const std::string& local, const std::string& cache,
          const std::string& tmp) {
  const std::vector<char> want = slurp(local);
  check(!want.empty(), "the local reference file is readable");

  const std::string dst1 = tmp + "/copy1.bin", dst2 = tmp + "/copy2.bin";
  check(copy(url, dst1) && slurp(dst1) == want, "copy 1 (default read path) is identical");
  check(cacheFiles(cache, ".meta").empty(), "copy 1 created no cache entry");

  std::vector<char> got;
  check(readAll(url, got) && got == want, "a reader in the same process reads identically");
  const auto metas = cacheFiles(cache, ".meta"), datas = cacheFiles(cache, ".data");
  struct ::stat before{};
  const bool cached = metas.size() == 1 && datas.size() == 1 &&
                      ::stat(datas[0].c_str(), &before) == 0 && before.st_blocks > 0;
  check(cached, "... and the reader's handle cached the file (one entry, holding data)");

  // The copy engine's plain-read path: the one that would read the cache.
  XrdCl::DefaultEnv::GetEnv()->PutInt("CpUsePgWrtRd", 0);
  check(copy(url, dst2) && slurp(dst2) == want, "copy 2 (plain reads, file cached) is identical");
  struct ::stat after{};
  check(cached && ::stat(datas[0].c_str(), &after) == 0 && after.st_blocks == before.st_blocks &&
            after.st_size == before.st_size && cacheFiles(cache, ".meta") == metas,
        "copy 2 left the cached entry as it was");
  ::unlink(dst1.c_str());
  ::unlink(dst2.c_str());
  return fails ? 1 : 0;
}

// A counter from the last complete line of the child's stats file; -1 if none.
long long statOf(const std::string& cache, pid_t pid, const char* name) {
  const std::string mark = "-" + std::to_string(pid) + "-";
  for (const auto& f : listDir(cache + "/stats")) {
    if (f.find(mark) == std::string::npos || !endsWith(f, ".jsonl") ||
        endsWith(f, ".files.jsonl") || endsWith(f, ".trace.jsonl"))
      continue;
    std::ifstream in(cache + "/stats/" + f);
    std::string line, last;
    while (std::getline(in, line))
      if (!line.empty() && line.back() == '}')
        last = line;
    const std::string key = std::string("\"") + name + "\":";
    const auto at = last.find(key);
    if (at != std::string::npos)
      return std::atoll(last.c_str() + at + key.size());
  }
  return -1;
}

} // namespace

int main(int argc, char** argv) {
  if (argc == 6 && std::strcmp(argv[1], "--child") == 0)
    return child(argv[2], argv[3], argv[4], argv[5]);
  if (argc < 3) {
    std::fprintf(stderr, "usage: %s <root-url> <local-path>\n", argv[0]);
    return 2;
  }
  const char* cacheEnv = ::getenv("UCACHE_DIR");
  if (!cacheEnv || !*cacheEnv) {
    std::fprintf(stderr, "copy_probe: set UCACHE_DIR to an empty cache directory\n");
    return 2;
  }
  const std::string cache = cacheEnv;
  if (!cacheFiles(cache, ".meta").empty()) {
    std::fprintf(stderr, "copy_probe: %s already holds entries; give it an empty one\n",
                 cache.c_str());
    return 2;
  }
  char tmpl[] = "/tmp/ucache-copy-probe-XXXXXX";
  const char* tmp = ::mkdtemp(tmpl);
  if (!tmp) {
    std::perror("mkdtemp");
    return 2;
  }
  // A new image, not a bare fork: the client and the plugin start their
  // threads when the client library loads, and threads do not survive a fork.
  // The child owns the whole session, and its normal exit writes its stats line.
  std::fflush(stdout);
  const pid_t pid = ::fork();
  if (pid < 0) {
    std::perror("fork");
    return 2;
  }
  if (pid == 0) {
    ::execvp(argv[0], std::vector<char*>{argv[0], const_cast<char*>("--child"), argv[1], argv[2],
                                         const_cast<char*>(cache.c_str()), const_cast<char*>(tmp),
                                         nullptr}
                          .data());
    std::perror("exec");
    ::_exit(127);
  }
  int st = 0;
  ::waitpid(pid, &st, 0);
  ::rmdir(tmp);
  const bool childOk = WIFEXITED(st) && WEXITSTATUS(st) == 0;
  if (!childOk)
    ++fails;
  const long long copies = statOf(cache, pid, "copier_handles");
  const long long writes = statOf(cache, pid, "page_writes");
  std::printf("stats: copier_handles=%lld page_writes=%lld\n", copies, writes);
  check(copies >= 2, "both copies counted as copies (copier_handles >= 2): the plugin engaged");
  check(writes > 0, "the reader's fill was written (page_writes > 0)");
  std::printf(fails ? "copy_probe: %d FAILED\n" : "copy_probe: ALL PASS\n", fails);
  return fails ? 1 : 0;
}
