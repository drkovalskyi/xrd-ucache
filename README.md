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

## License

MIT License — see `LICENSE`. Copyright (c) 2026 Massachusetts Institute of
Technology. Author: Dmytro Kovalskyi (MIT).
