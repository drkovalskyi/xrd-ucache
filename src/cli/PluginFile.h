// Which plugin file an XRootD client actually opens.
//
// A conf's `lib =` (or XRD_PLUGIN) names a library, but an XRootD client does
// not open that name first. It inserts its own plugin version — the client's
// major version, 5 or 6 — before the file name's last dot and opens that:
// `lib = /usr/lib64/libXrdClUCache.so` makes a 5.x client open
// /usr/lib64/libXrdClUCache-5.so and a 6.x client libXrdClUCache-6.so. Only if
// that file cannot be loaded does it open the name as written. A name that
// already carries a version is treated the same way (libXrdClUCache-5.so
// becomes libXrdClUCache-5-6.so for a 6.x client, which does not exist, so the
// client opens libXrdClUCache-5.so as written — and then refuses it).
//
// This is how one conf serves clients of both majors: the plugin is installed
// as one file per major, and each client picks its own. `doctor` must check
// the file the client will open, not the name in the conf, so it resolves the
// name by the same rule.
//
// Pure functions: no I/O except through the `exists` predicate, so tests can
// supply a directory listing.

#pragma once

#include <algorithm>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace ucache {

// The name a client of `major` tries first for `lib`.
inline std::string versionedPluginName(const std::string& lib, int major) {
  const size_t slash = lib.rfind('/');
  const size_t base = slash == std::string::npos ? 0 : slash + 1;
  const size_t dot = lib.rfind('.');
  const size_t cut = (dot == std::string::npos || dot < base) ? lib.size() : dot;
  return lib.substr(0, cut) + "-" + std::to_string(major) + lib.substr(cut);
}

// The file a client of `major` loads for `lib`: the versioned name when it
// exists, else the name as written when that exists, else "".
inline std::string pluginFileFor(const std::string& lib, int major,
                                 const std::function<bool(const std::string&)>& exists) {
  if (lib.empty())
    return "";
  const std::string v = versionedPluginName(lib, major);
  if (exists(v))
    return v;
  return exists(lib) ? lib : "";
}

// Every per-major build that sits beside `lib` ({major, path}, ascending by
// major), given the names in its directory. For the case where the client's
// version is unknown and there is no single answer to give.
inline std::vector<std::pair<int, std::string>> pluginBuildsBeside(
    const std::string& lib, const std::vector<std::string>& dirEntries) {
  std::vector<std::pair<int, std::string>> out;
  const size_t slash = lib.rfind('/');
  const std::string dir = slash == std::string::npos ? "" : lib.substr(0, slash + 1);
  const std::string name = lib.substr(dir.size());
  const size_t dot = name.rfind('.');
  const std::string stem = dot == std::string::npos ? name : name.substr(0, dot);
  const std::string ext = dot == std::string::npos ? "" : name.substr(dot);
  for (const auto& e : dirEntries) {
    if (e.size() <= stem.size() + 1 + ext.size() || e.compare(0, stem.size(), stem) != 0 ||
        e[stem.size()] != '-' || e.compare(e.size() - ext.size(), ext.size(), ext) != 0)
      continue;
    const std::string num = e.substr(stem.size() + 1, e.size() - stem.size() - 1 - ext.size());
    if (num.empty() || num.size() > 3 ||
        !std::all_of(num.begin(), num.end(), [](char c) { return c >= '0' && c <= '9'; }))
      continue;
    out.emplace_back(std::stoi(num), dir + e);
  }
  std::sort(out.begin(), out.end());
  return out;
}

// `lib` without a per-major suffix (libXrdClUCache-5.so -> libXrdClUCache.so);
// unchanged when it has none. The name a conf should carry to serve every
// client, where naming one build pins every client to it.
inline std::string plainPluginName(const std::string& lib) {
  const size_t slash = lib.rfind('/');
  const size_t base = slash == std::string::npos ? 0 : slash + 1;
  size_t dot = lib.rfind('.');
  if (dot == std::string::npos || dot < base)
    dot = lib.size();
  const size_t dash = lib.rfind('-', dot);
  if (dash == std::string::npos || dash < base || dash + 1 == dot)
    return lib;
  for (size_t i = dash + 1; i < dot; ++i)
    if (lib[i] < '0' || lib[i] > '9')
      return lib;
  return lib.substr(0, dash) + lib.substr(dot);
}

// True when a library path names this plugin (plain or per-major build).
inline bool isUCachePluginName(const std::string& lib) {
  const size_t slash = lib.rfind('/');
  return lib.compare(slash == std::string::npos ? 0 : slash + 1, 14, "libXrdClUCache") == 0;
}

} // namespace ucache
