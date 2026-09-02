# uCache

xrd-ucache or simply uCache is a transparent, per-user read cache for
remote ROOT/XRootD data. You install it once and point it at the servers
you read from; thereafter every file your jobs read from them is cached
on local disk, so the **second and later** passes over the same data
come from that disk instead of the network. Nothing in your analysis
code changes and the first cold run has minimal overhead, so there's
little to lose in trying it.

[![arXiv](https://img.shields.io/badge/arXiv-2609.00400-B31B1B.svg)](https://arxiv.org/abs/2609.00400)

## Try it

To use uCache with your analysis, install it and activate it with a
plugin configuration file. The following instructions show how to do
it using the EL9 (Alma/Rocky/RHEL 9) tarball. For Debian/Ubuntu, other Linux
distributions, or macOS — and for building from source on any of them — see
[the user guide](docs/USER_GUIDE.md).

First, ensure that you have all relevant packages for data access over XRootD:

```sh
sudo dnf install epel-release xrootd-client root-netx
```

Install uCache: download the latest tarball from this repository's
Releases page, then

```sh
# install plugin in ~/.local
tar -C ~/.local --strip-components=1 -xf xrd-ucache-<version>-el9-x86_64.tar.gz

# make CLI client accessible
export PATH="$HOME/.local/bin:$PATH"

ucache setup --dir /path/to/cache   # writes the ONE conf file (or write it
                                    # yourself per the guide — same result)
ucache doctor                       # static check: install + activation + settings
ucache test root://<host>//<file>   # end-to-end self-test (cold + warm, cleans up)
# ... run your ROOT/RDataFrame/uproot job normally ...
```

Activation is user-global and needs no root. The cache directory has **no
default** — `doctor` complains until you set one. `ucache setup
--host <host:port>` binds one host if a system plugin conf already claims the
`*` slot (see [the user guide](docs/USER_GUIDE.md)). Eviction is on by default (keep the disk from
filling; `ucache status` shows the budget).

Config file example

``` sh
url = eospublic.cern.ch:1094
lib = /home/<you>/.local/lib64/libXrdClUCache.so
enable = true
dir = /tmp/cache
```

`lib` must be a literal absolute path — this file is read by the XRootD
client, which does not expand `~`.

Note: `/tmp/cache` is not an optimal location due to automatic
cleanup. Find a better location by testing candidate directories with
`ucache bench <dir>`.

## Testing

The self-contained suites live here: unit, differential, crash-recovery, soak
and fuzz tests. CI builds them and runs `ctest` on Linux and macOS for every
change, along with 200 kill-9 crash-recovery iterations; a nightly job repeats
the differential at a million operations and the crash loop at a thousand.

The XRootD client plugin is built and fuzzed against a live origin on Linux
only. The macOS job stops short of it, so there the plugin is exercised by hand
rather than in CI.

Further validation is performed with tools outside this repository.

## Documentation

- [User guide](docs/USER_GUIDE.md) — install from the EL9 packages, or from
  source on Linux and macOS; activate, verify, configure, CLI reference
- [Cache management](docs/CACHE_MANAGEMENT.md) — space, eviction, choosing a
  cache disk
- [Troubleshooting](docs/TROUBLESHOOTING.md) — when it doesn't engage, or
  behaves oddly
- [Monitoring metrics](docs/STATS.md) — the numbers uCache records about your
  jobs, and the JSON files it writes
- [Storage benchmarking](docs/BENCH.md) — `ucache bench`: measuring a
  candidate cache directory, and why the choice matters
- [On-disk format](docs/FORMAT.md) — what a cached entry is made of on disk,
  for anyone inspecting or writing tooling against it

## License

MIT License — see `LICENSE`. Copyright (c) 2026 Massachusetts Institute of
Technology. Author: Dmytro Kovalskyi (MIT).

## Development and AI assistance

Claude Code (Anthropic's agentic coding tool) was used in the development of
uCache and its documentation. The authors specified the requirements and
architecture, directed each change, and validated the implementation by its
measured behavior: unit, differential, crash-recovery and fuzz test suites run
under memory and race detectors; integration tests against real analysis
frameworks and storage services; and physics validation showing that cached and
direct analyses produce identical results at full dataset scale.

Individual commits carry no AI attribution. Its absence from the commit history
is a convention, not an omission.
