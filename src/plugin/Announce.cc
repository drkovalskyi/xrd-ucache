#include "Announce.h"

namespace ucache {
namespace {

// '&' separates fields in the login string and '=' separates a field from its
// value, so neither may appear inside one. Everything outside printable ASCII
// goes too: these strings end up in server logs, monitoring records and
// dashboards, and a newline or a control byte in any of those is somebody
// else's bug report.
std::string sanitize(const std::string& in, std::size_t maxChars) {
  std::string out;
  const std::size_t n = in.size() < maxChars ? in.size() : maxChars;
  out.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    const unsigned char c = static_cast<unsigned char>(in[i]);
    out.push_back((c == '&' || c == '=' || c < 0x20 || c >= 0x7f) ? '_' : in[i]);
  }
  return out;
}

} // namespace

Announcement buildAnnouncement(const std::string& hostApp, const std::string& version) {
  Announcement a;
  a.appName = kAnnounceAppName;
  a.monInfo = std::string(kAnnounceAppName) + "/" + sanitize(version, kAnnounceHostAppMax);
  // The host program, when there is one worth naming. Naming ourselves twice
  // says nothing, so a host program already called `ucache` is left off.
  const std::string host = sanitize(hostApp, kAnnounceHostAppMax);
  if (!host.empty() && host != kAnnounceAppName)
    a.monInfo += " (" + host + ")";
  return a;
}

} // namespace ucache
