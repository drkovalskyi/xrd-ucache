#include "UrlKey.h"

#include "vendor/sha256.h"

#include <algorithm>
#include <cctype>
#include <fnmatch.h>
#include <map>
#include <sstream>

namespace ucache {
namespace {

std::string toLower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return s;
}

int defaultPort(const std::string& scheme) {
  if (scheme == "root" || scheme == "xroot" || scheme == "roots" || scheme == "xroots")
    return 1094;
  if (scheme == "http")
    return 80;
  if (scheme == "https")
    return 443;
  return 0;
}

// Collapse duplicate slashes and resolve "." / ".." without escaping root.
std::string normalizePath(const std::string& in) {
  std::vector<std::string> segs;
  std::string seg;
  std::istringstream is(in);
  while (std::getline(is, seg, '/')) {
    if (seg.empty() || seg == ".")
      continue;
    if (seg == "..") {
      if (!segs.empty())
        segs.pop_back();
      continue;
    }
    segs.push_back(seg);
  }
  std::string out;
  for (const auto& s : segs)
    out += "/" + s;
  return out.empty() ? "/" : out;
}

} // namespace

std::optional<UrlKey> UrlKey::parse(const std::string& url,
                                    const std::vector<std::string>& keepCgi) {
  auto schemeEnd = url.find("://");
  if (schemeEnd == std::string::npos || schemeEnd == 0)
    return std::nullopt;
  std::string scheme = toLower(url.substr(0, schemeEnd));

  size_t authStart = schemeEnd + 3;
  size_t pathStart = url.find('/', authStart);
  if (pathStart == std::string::npos)
    return std::nullopt;
  std::string auth = url.substr(authStart, pathStart - authStart);
  if (auth.empty())
    return std::nullopt;
  // Drop userinfo if present (never part of the key).
  if (auto at = auth.rfind('@'); at != std::string::npos)
    auth = auth.substr(at + 1);

  std::string host = auth;
  int port = defaultPort(scheme);
  if (!auth.empty() && auth.front() == '[') {
    // [ipv6] or [ipv6]:port — brackets are mandatory for v6 literals.
    auto close = auth.find(']');
    if (close == std::string::npos)
      return std::nullopt;
    host = auth.substr(0, close + 1);
    if (close + 1 < auth.size() && auth[close + 1] == ':')
      port = std::atoi(auth.c_str() + close + 2);
  } else if (auto colon = auth.rfind(':'); colon != std::string::npos) {
    host = auth.substr(0, colon);
    port = std::atoi(auth.c_str() + colon + 1);
  }
  host = toLower(host);
  if (host.empty() || port <= 0)
    return std::nullopt;

  std::string rest = url.substr(pathStart);
  std::string cgi;
  if (auto q = rest.find('?'); q != std::string::npos) {
    cgi = rest.substr(q + 1);
    rest = rest.substr(0, q);
  }
  std::string path = normalizePath(rest);

  // Keep only whitelisted CGI keys, sorted for canonical form.
  std::string kept;
  if (!keepCgi.empty() && !cgi.empty()) {
    std::map<std::string, std::string> keptKv; // sorted by key
    std::istringstream is(cgi);
    std::string kv;
    while (std::getline(is, kv, '&')) {
      auto eq = kv.find('=');
      std::string k = eq == std::string::npos ? kv : kv.substr(0, eq);
      if (std::find(keepCgi.begin(), keepCgi.end(), k) != keepCgi.end())
        keptKv[k] = eq == std::string::npos ? "" : kv.substr(eq + 1);
    }
    for (const auto& [k, v] : keptKv)
      kept += (kept.empty() ? "?" : "&") + k + "=" + v;
  }

  UrlKey out;
  out.scheme = scheme;
  out.host = host;
  out.key = scheme + "://" + host + ":" + std::to_string(port) + path + kept;
  out.hashHex = sha256Hex(out.key);
  return out;
}

std::string UrlKey::objectDir(const std::string& cacheDir) const {
  return cacheDir + "/objects/" + hashHex.substr(0, 2);
}
std::string UrlKey::dataPath(const std::string& cacheDir) const {
  return objectDir(cacheDir) + "/" + hashHex + ".data";
}
std::string UrlKey::metaPath(const std::string& cacheDir) const {
  return objectDir(cacheDir) + "/" + hashHex + ".meta";
}

bool hostMatchesAny(const std::vector<std::string>& globs, const std::string& host) {
  for (const auto& g : globs)
    if (::fnmatch(g.c_str(), host.c_str(), FNM_CASEFOLD) == 0)
      return true;
  return false;
}

std::string stripCgiParams(const std::string& url, const std::vector<std::string>& names) {
  auto q = url.find('?');
  if (q == std::string::npos)
    return url;
  std::string kept;
  std::istringstream in(url.substr(q + 1));
  std::string param;
  while (std::getline(in, param, '&')) {
    if (param.empty())
      continue;
    std::string k = param.substr(0, param.find('='));
    if (std::find(names.begin(), names.end(), k) != names.end())
      continue;
    kept += (kept.empty() ? "" : "&") + param;
  }
  return kept.empty() ? url.substr(0, q) : url.substr(0, q) + "?" + kept;
}

} // namespace ucache
