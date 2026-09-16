# uCache

xrd-ucache or simply uCache is a transparent, per-user read cache for
remote ROOT/XRootD data. You install it once and point it at the servers
you read from; thereafter every file your jobs read from them is cached
on local disk, so the **second and later** passes over the same data
come from that disk instead of the network. Nothing in your analysis
code changes and the first cold run has minimal overhead, so there's
little to lose in trying it.

[![arXiv](https://img.shields.io/badge/arXiv-2609.00400-B31B1B.svg)](https://arxiv.org/abs/2609.00400)

## How it works

uCache is a plug-in for the XRootD client — the library ROOT already uses for
every `root://` URL. Before your job opens its first file, that library's
plug-in manager reads the plugin configs in your home directory and asks
whether any of them claims the server in your URL. If `ucache.conf` does, the
manager loads the library it names, and from then on uCache answers that
file's reads: a data block already on your local disk is served from there,
anything else is fetched from the server and kept for next time.

![ROOT hands every root:// URL to the XRootD client library, whose plug-in manager reads ucache.conf from your home directory and loads the uCache plugin; the plugin then serves data blocks from your local disk and fetches the rest from the server.](docs/images/how-it-works.png)

Nothing about your analysis changes — same commands, same file names, same
results; only where the bytes come from. Delete that one config file and the
same job runs exactly as it did before.

## Getting started

### 1. What you need

An XRootD client (5.6 or newer) and whatever reads your data — ROOT, uproot,
`cmsRun`. On EL9:

```sh
sudo dnf install epel-release xrootd-client root-netx
```

uCache itself needs no administrator. For Debian/Ubuntu, macOS, or building
from source, see [the user guide](docs/USER_GUIDE.md).

### 2. Install three files

Download the tarball from the Releases page, then unpack it where you stand
and copy the parts you want, so nothing is written anywhere you did not name:

```sh
tar xf xrd-ucache-<version>-el9-x86_64.tar.gz
cd xrd-ucache-<version>-el9-x86_64

mkdir -p ~/.local/bin ~/.local/lib64
cp bin/ucache bin/ucache-netbench  ~/.local/bin/
cp lib64/libXrdClUCache.so         ~/.local/lib64/

export PATH="$HOME/.local/bin:$PATH"
```

`libXrdClUCache.so` is the plugin, the only part loaded into your job.
`ucache` is the command you will use. `ucache-netbench` is a helper that
`ucache netbench` runs for you; it is a separate executable because it is the
one piece that needs the XRootD client library. The archive also carries the
guides under `share/doc/xrd-ucache/` for machines with no web access; nothing
requires them. If you would rather a package manager owned the files, each
release also ships an EL9 RPM.

### 3. Choose a disk

The disk decides whether caching helps at all: on a slow or network-backed
volume a cache can be worse than reading the origin. Use a **local SSD or
NVMe**, never AFS or NFS, and avoid `/tmp` if a system reaper cleans it. If
you have a choice, measure the candidates before committing to one —
`ucache bench /path/to/candidate`, a few minutes each; [storage
benchmarking](docs/BENCH.md) explains how to read the result.

### 4. Write one config file

Activation is that file and nothing else. Create the directory the XRootD
client reads, and put `ucache.conf` in it:

```sh
mkdir -p ~/.xrootd/client.plugins.d
```

`~/.xrootd/client.plugins.d/ucache.conf`:

```
# which servers to cache (several: separate them with ;)
url = eospublic.cern.ch:1094;eoscms.cern.ch:1094

# the plugin library, absolute path (~ is not expanded)
lib = /home/you/.local/lib64/libXrdClUCache.so
enable = true

# the cache itself: a local SSD or NVMe, not AFS or NFS
# UCACHE_DIR overrides it: one conf, a disk per machine
dir = /path/to/cache
```

This is the file in the diagram above, and each line is doing one job:

- `url` — the servers uCache intercepts, separated by `;`. The port is
  optional. `*` means every server, but a site-wide plugin config may already
  hold that slot, in which case yours is skipped and `ucache doctor` says so;
  naming your servers avoids the question.
- `lib` — a literal absolute path. This file is read by the XRootD client,
  which does not expand `~`, so run
  `echo "$HOME/.local/lib64/libXrdClUCache.so"` and paste what it prints.
- `enable` — the switch. `false` turns uCache off without deleting anything.
- `dir`, and anything below it, is uCache's own configuration. There is
  deliberately **no default cache directory**: a default would quietly land in
  your home, which at many sites is AFS, and caching network data onto a
  network filesystem defeats the point. If your home is shared across machines
  that each have their own local disk, leave `dir` out of the file and set
  `UCACHE_DIR` per machine instead — it overrides the conf, it also tells
  `ucache status` and `summary` which cache to read, and a machine where you
  forget it runs uncached and says so rather than caching onto the shared home.

Four syntax rules the client enforces strictly, none of which it explains: a
`#` comment must start in the **first column** — an indented one makes the
client reject the whole file and load nothing; a comment written after a value
becomes **part of that value**; every value is taken **literally**, with no
`$VAR`, `~` or hostname expanded for you; and the file must be named `*.conf`.

Nothing else takes part. No environment variable, no `LD_PRELOAD`, no ROOT
configuration, and no shell to reload — a batch job launched tomorrow picks up
the same file.

### 5. Check it before trusting it

```sh
ucache doctor                       # install, activation, settings, cache filesystem
ucache test root://<host>//<file>   # real cold and warm read, then cleans up
```

`doctor` is static and exits non-zero if anything is wrong. `test` is the
end-to-end proof: it reads a file twice and passes only if the second read
fetched nothing from the origin.

### 6. Run your job

Nothing changes. Run ROOT, uproot or `cmsRun` exactly as before; the first
pass fills the cache and later passes are served locally. If anything goes
wrong with the cache the read falls back to the origin rather than failing —
that behaviour is the design's first rule, not a safety net bolted on.

### 7. See whether it helped

```sh
ucache status     # what is cached, disk used, headroom
ucache summary    # is this cache worth having
ucache history    # per-run detail
```

One thing worth doing once: run the same job with `UCACHE_DISABLE=1`, so there
is a switched-off run to compare against. uCache reports a gain only when it
has measured one, and it reports a loss as a loss.

### Housekeeping

Eviction is on by default so the disk cannot fill; `ucache status` shows the
budget. To remove uCache, delete the files you copied and
`~/.xrootd/client.plugins.d/ucache.conf`.


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
- [Publishing a measurement](docs/PUBLISH.md) — the optional `ucache publish`:
  what a report gives you, your identity string and where it lives, and the
  field-by-field statement of what leaves the machine. Nothing is published
  unless you run it
- [On-disk format](docs/FORMAT.md) — what a cached entry is made of on disk,
  for anyone inspecting or writing tooling against it

## Citing uCache

The design and its measurements are described in the paper
([arXiv:2609.00400](https://arxiv.org/abs/2609.00400)). If uCache is useful
in your work, please cite it:

```bibtex
@misc{kovalskyi2026ucache,
  title         = {Client-side transparent caching for remote {ROOT} data analysis},
  author        = {Kovalskyi, Dmytro and Eysermans, Jan and D'Alfonso, Mariarosaria and Paus, Christoph},
  year          = {2026},
  eprint        = {2609.00400},
  archivePrefix = {arXiv},
  primaryClass  = {cs.DC},
  doi           = {10.48550/arXiv.2609.00400},
  url           = {https://arxiv.org/abs/2609.00400}
}
```

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
