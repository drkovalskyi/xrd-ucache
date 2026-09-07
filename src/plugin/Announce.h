// The identity uCache presents to an XRootD server.
//
// A server learns who is talking to it from two strings the XRootD client
// sends inside the login request: an application name and a free-form
// information string. Left alone they name the process that happens to host
// the client -- root.exe, python3, cmsRun -- and a cache in front of that
// process is invisible: sites see the requests uCache makes but cannot tell
// they are uCache's.
//
// So uCache names itself as the application and keeps the host program in the
// information string, in the shape a User-Agent uses:
//
//     application  ucache
//     information  ucache/1.0.0 (root.exe)
//
// Both strings ride in the login request that opens a session, once per
// server, so nothing is sent per file and no extra request is made. They are
// the only thing that changes: the client reads them nowhere else, so what
// uCache asks for and how it behaves are untouched.
//
// This header is pure -- no XRootD, no I/O, no globals -- so the composition
// rules are testable on their own. The caller hands the result to the client;
// see the factory.
//
// Thread-safety: buildAnnouncement is a pure function of its arguments and may
// be called concurrently.
#pragma once

#include <string>

namespace ucache {

struct Announcement {
  std::string appName; // the application name: always kAnnounceAppName
  std::string monInfo; // "<name>/<version>" plus the host program in parentheses
};

// The name uCache answers to. Short and bounded on purpose: a collector may
// use it as a metric label.
inline constexpr const char* kAnnounceAppName = "ucache";

// Longest host-program name kept in the information string. The whole login
// carries every field in about a kilobyte, and a program name is normally a
// dozen characters; a pathological one is truncated rather than allowed to
// crowd out the fields beside it.
inline constexpr std::size_t kAnnounceHostAppMax = 64;

// Compose the two strings. `hostApp` is the application name the client had
// before uCache spoke up (the host program, or whatever the user set); it may
// be empty. Characters that would be read as field separators, and anything
// unprintable, are replaced -- a program name is data here, and it must not be
// able to invent a field.
Announcement buildAnnouncement(const std::string& hostApp, const std::string& version);

} // namespace ucache
