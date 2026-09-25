// Which handles belong to a COPY rather than to a reader.
//
// uCache may show a reader a file in a layout of its own: a replica, or the
// slot layout recompression converts into as a job reads. Those layouts are a
// different, larger file than the one at the origin, and they read the same
// only to an application that follows the file's own offsets -- ROOT does. A
// copy does not: a copy tool reads bytes 0..size and writes them out, so a
// copy made through the cache would be the cache's layout, not the file. A
// copy is meant to be the origin's bytes.
//
// So a handle opened for a copy is passed straight through to the origin. It
// never uses the cache and never adds to it, and it answers every property the
// way the origin does (a checksum asked for at the end of a copy is the
// origin's). Two signals decide it, both when the handle opens:
//
//   - the program: xrdcp (and its xrdcopy alias), xrdfs, xrdadler32 and
//     edmCopyUtil only ever copy, so every read handle they open is a copy;
//     and hadd and ROOT's command-line tools (rootcp, rootls, ...) only merge,
//     copy or inspect files, so they are treated the same way: a merge that
//     copies baskets as it reads them, or a tool reporting a file's sizes and
//     codecs, must see the origin's file, not the cache's layout. Most of
//     ROOT's tools are Python scripts, recognised by the script the
//     interpreter runs;
//   - the call stack: a handle opened from inside XRootD's copy engine (the
//     one xrdcp, Python's XRootD.client.CopyProcess, gfal2 and rucio use), from
//     ROOT's static TFile::Cp or its file merger (TFileMerger, which hadd
//     uses, opening an input by name), or from gfal2's xrootd plugin.
//
// The stack check compares return addresses against address ranges that are
// resolved once per set of loaded libraries: the copy-engine functions are
// looked up in the very libXrdCl this plugin is bound to (only that library can
// route an open here), TFile::Cp in ROOT's I/O library once it is loaded, and
// gfal2's plugin by its file name. There is no symbol lookup per frame -- that
// is a linear scan of a library's symbol table under the loader's lock, per
// frame, per open.
//
// Both directions of error are safe. A copy that is not recognised reads
// through the cache as any reader does; a reader taken for a copy gets the
// origin's bytes, uncached. Neither hands anyone bytes in a layout it did not
// ask for: the program's name and the setting are fixed for the life of the
// process, and a copy handle never enters the per-process record of which
// layout a file was shown in.
//
// Not recognised, by construction: a copy made by reading a file handle in a
// loop (fsspec's get and open().read(), a hand-written loop), fast cloning or
// inspection inside a program of the user's own (TTree::CloneTree(-1, "fast"),
// TTree::Print on a file it opened to analyse), a merge of files the program
// opened itself before handing them to TFileMerger, and a copy through an
// XRootD proxy or a FUSE mount that has uCache inside it. Those copy with the
// cache switched off.
//
// This file has no XRootD dependency, so it is unit-tested on its own.
//
// Thread-safety: every function here may be called concurrently from any
// thread. The address table is immutable once built and is published through
// an atomic shared pointer; a thread that sees the set of loaded libraries
// change builds a fresh one without taking a lock (so a process that forks
// while another thread is building cannot inherit a held lock).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ucache {

enum class CopySignal : uint8_t {
  kNone,       // not a copy
  kExecutable, // the program is a copy tool
  kCopyEngine, // opened from inside XRootD's copy engine
  kRootCp,     // opened from inside ROOT's TFile::Cp
  kGfal,       // opened from inside gfal2's xrootd plugin
  kRootTool,   // the program is hadd or one of ROOT's command-line tools
  kMerge,      // opened from inside ROOT's TFileMerger
};

// What the signal was, in words, for a log line ("XRootD's copy engine").
const char* copySignalName(CopySignal s);

// Programs whose every read is a copy. Matched against the executable's file
// name (after symlinks are resolved), so a renamed copy of one is not matched.
inline constexpr const char* kCopyToolExecutables[] = {"xrdcp", "xrdcopy", "xrdfs", "xrdadler32",
                                                       "edmCopyUtil"};

// The functions whose frames mark a copy, as the dynamic linker exports them.
// These are the Itanium-ABI names; a typo would switch detection off without a
// sound, so the unit test pins every string. The XrdCl names exist, with these
// exact spellings, from the 5.6 client floor through 6.x.
inline constexpr const char* kCopyEngineSymbols[] = {
    "_ZN5XrdCl14ClassicCopyJob3RunEPNS_19CopyProgressHandlerE",    // ClassicCopyJob::Run
    "_ZN5XrdCl17ThirdPartyCopyJob3RunEPNS_19CopyProgressHandlerE", // ThirdPartyCopyJob::Run
    "_ZN5XrdCl11CopyProcess3RunEPNS_19CopyProgressHandlerE",       // CopyProcess::Run
    "_ZN5XrdCl6XCpSrc3RunEPv", // XCpSrc::Run: a multi-source copy's reader thread
};
// hadd and ROOT's command-line tools: matched against the executable's file
// name, and, when the program is a Python interpreter, the file name of the
// script it runs.
inline constexpr const char* kRootToolPrograms[] = {
    "hadd",   "rootcp",       "rootmv",     "rooteventselector", "rootslimtree", "rootls",
    "rootprint", "rootdrawtree", "rootbrowse", "rootrm",            "rootmkdir"};

// static Bool_t TFile::Cp(const char*, const char*, Bool_t, UInt_t), in libRIO.
inline constexpr const char* kRootCpSymbol = "_ZN5TFile2CpEPKcS1_bj";
// TFileMerger, in libRIO: AddFile(const char*, Bool_t) opens an input by name;
// OpenExcessFiles() reopens inputs a merge of many files had to close.
inline constexpr const char* kRootMergeSymbols[] = {"_ZN11TFileMerger7AddFileEPKcb",
                                                    "_ZN11TFileMerger15OpenExcessFilesEv"};
// gfal2's xrootd plugin has no SONAME and is loaded by path; its file name is
// libgfal_plugin_xrootd.so.
inline constexpr const char* kGfalXrootdPrefix = "libgfal_plugin_xrootd";

// Frames looked at, counting the detector's own. Deeper than any open issued
// by a copy engine (a handful of frames under the engine's Run) or by TFile::Cp
// through ROOT's plugin manager (a dozen or so); a bound, because the walk
// costs time per frame on every open.
inline constexpr int kCopyStackDepth = 32;

// The file name of a path, without a trailing " (deleted)" (what the kernel
// appends to the link of an executable replaced while it runs).
std::string executableBaseName(const std::string& path);
bool isCopyToolExecutable(const std::string& base);
bool isRootToolProgram(const std::string& base);
// The running program's file name ("" if it cannot be found). Computed once.
const std::string& hostExecutable();
// For a program that is a Python interpreter, the file name of the script it
// runs: the first argument that is not an option ("" for none, for -c and -m,
// and for any other program). `argv` is the whole command line.
std::string scriptOfCommandLine(const std::vector<std::string>& argv);
// scriptOfCommandLine of this process's command line. Computed once.
const std::string& hostScript();
// kExecutable or kRootTool when the program alone makes every read a copy,
// else kNone. Free: both names are computed once.
CopySignal copyProgramSignal();
// Whether a loaded object is gfal2's xrootd plugin / ROOT's I/O library, by
// its path.
bool isGfalXrootdObject(const std::string& path);
bool isRootIoObject(const std::string& path);

// Record `xrdclAnchor` -- the address of any function in the libXrdCl this
// plugin is bound to -- compute the program's name, prime the stack walker
// (its first use can load a library, which must not happen inside an open)
// and resolve the address table. May be called again: the anchor is replaced
// and the table rebuilt.
void copyDetectInit(const void* xrdclAnchor);

// The signal for a handle being opened on this thread now: the program's name
// first (free), then the stack walk.
CopySignal copierSignal();
// The stack walk alone, over at most `depth` frames.
CopySignal copierStackSignal(int depth = kCopyStackDepth);

} // namespace ucache
