# uCache

xrd-ucache or simply uCache is a transparent, per-user read cache for
remote ROOT/XRootD data. You install it once; thereafter every
`root://…` file your jobs read is cached on local disk, so the
**second and later** passes over the same data read from your NVMe
instead of the network. Nothing in your analysis code changes and the
first cold run has minimal overhead, so there's little to lose in
trying it.

## Try it

To use uCache with your analysis, install it and activate it with a
plugin configuration file. The following instructions show how to do
it using the EL9 (Alma/Rocky/RHEL 9) tarball.

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
`*` slot (see the guide `docs/USER_GUIDE.md`). Eviction is on by default (keep the disk from
filling; `ucache status` shows the budget).

Config file example

``` sh
url = eospublic.cern.ch:1094
lib = ~/.local/lib64/libXrdClUCache.so
enable = true
dir = /tmp/cache
```

Note: `/tmp/cache` is not an optimal location due to automatic
cleanup. Find a better location by testing candidate directories with
`ucache bench <dir>`.

## Testing

The self-contained suites live here: unit, differential, crash-recovery, soak
and fuzz tests. CI builds them and runs `ctest` on Linux and macOS for every
change. The macOS job stops short of the XRootD client plugin: the plugin is
built and fuzzed against a live origin on Linux, and on macOS it is built and
exercised by hand rather than in CI. The longer crash, soak and fuzz campaigns
run outside CI because of their duration.

Further validation is performed with tools outside this repository.

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
