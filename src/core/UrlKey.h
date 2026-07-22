// URL keying and normalization.
//
// Key = scheme://host:port/path after normalization: scheme/host lowercased,
// default port made explicit (root/xroot -> 1094), consecutive slashes
// collapsed, "."/".." resolved. ALL CGI/opaque parameters are stripped by
// default — authz=/xrd.*/tokens must never reach disk or keys; UCACHE_KEEP_CGI
// whitelists semantically meaningful keys (kept sorted, canonical).
//
// Thread-safety: immutable value type; parse() is pure.
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace ucache {

struct UrlKey {
  std::string key;     // full normalized key (stored in meta for ls/debug)
  std::string hashHex; // sha256(key), lowercase hex
  std::string scheme;
  std::string host;    // lowercased, no port (for UCACHE_ALLOW/DENY globs)

  static std::optional<UrlKey> parse(const std::string& url,
                                     const std::vector<std::string>& keepCgi = {});

  // objects/<hh>/<hash>.data|.meta under cacheDir.
  std::string dataPath(const std::string& cacheDir) const;
  std::string metaPath(const std::string& cacheDir) const;
  std::string objectDir(const std::string& cacheDir) const;
};

// fnmatch-style glob matching for UCACHE_ALLOW/DENY host lists.
bool hostMatchesAny(const std::vector<std::string>& globs, const std::string& host);

// Remove the named CGI parameters from a URL, preserving everything else
// (auth tokens, xrd.* options) byte-for-byte. Used for the plugin's OWN
// origin connections (lazy cache-fill opens): client routing directives like
// tried=/triedrc= poison them — CMSSW's XrdAdaptor passes an exclusion list
// meant for ITS source diversity, and inheriting it forced the redirector to
// reselect unreachable data servers on every re-attempt.
std::string stripCgiParams(const std::string& url, const std::vector<std::string>& names);

} // namespace ucache
