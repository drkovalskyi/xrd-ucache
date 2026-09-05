# Publishing measurements — `ucache publish`

uCache can send a measurement to a report service and hand you back a web
page: the numbers, a grade for the disk, a comparison of the cache against the
origins it reads from, and recommendations. Nothing about this is automatic.
Publishing is a command you type, it runs in the foreground, and the plugin
that serves your reads does not know the service exists. This page says
exactly what a publish sends, what it never sends, and how the pieces fit.

```sh
ucache bench --threads 32 /scratch/cache --publish   # one disk measurement
ucache netbench root://eos.example.org//eos/f.root --publish   # one origin measurement
ucache publish   # the cache: its history, its disk and any disk benchmarked on the
                 # same volume, this machine's origin measurements
```

Every form accepts `--dry-run` (print the exact payload and send nothing),
`--yes` (no confirmation; a script needs it, because publishing from a
non-terminal without it is refused), `--label TEXT` (a short name for the disk
or cache, shown on your pages instead of a path), and `--url URL`
(`UCACHE_PUBLISH_URL` does the same) for a different service instance.
`ucache publish --runs N` changes how many of the newest runs go (200 by
default).

## What leaves the machine

The rule is short: **numbers, hardware, and opaque identifiers leave; names of
places and of people do not.** The table lists every field that has a name in
it or that the rules read; the last row covers the rest. Everything is decided
in the client before the first byte goes out; the service checks again and
refuses a payload that matches its own patterns for places, but that is a
backstop, not the mechanism.

| record | field | what it contains | what is sent |
|---|---|---|---|
| storage benchmark (`ucache bench`) | every measured rate, latency and count | numbers | as measured |
| | `host` | the hostname | blank; the machine block carries a salted hash instead |
| | `path` | the benchmarked directory | **not sent** — `location`, a salted hash of hostname + path, replaces it |
| | `mount`, `mount_source`, `mount_opts`, `mount_super_opts` | the mount point, `server:/export` on NFS or a volume name on LVM, the options | **not sent**; `volume`, a salted hash of hostname + mount point, replaces the mount point |
| | `dev_name`, `dev_model`, `dev_rotational`, `dev_sched`, `dev_size_gb`, `dev` | the block device (`sdb1`, `nvme0n1p1`, `rbd0`), its model, SSD or HDD, scheduler, size, major:minor | as measured: hardware, not a place |
| | `mount_fstype`, `fs` | the filesystem type | as measured |
| | `cmd` | the command line | kept, normalized: the executable becomes `ucache`, `--log` and its value are removed, every path becomes `<path>`. The parameters stay because they are what makes two runs comparable |
| | `kernel`, `arch`, `ncpu`, `mem_gb`, `cpu_model`, `version`, `time` | machine facts, tool version, when | as recorded |
| origin benchmark (`ucache netbench`) | rates and latencies per stream count | numbers | as measured |
| | `host` | your machine's hostname | blank |
| | `url` | the file you measured against | reduced to `root://<registrable domain>`, e.g. `root://cern.ch` — no host, no path |
| run history (`ucache history --json`) | bytes per tier, duration, measured gain, faults, instruction counts | numbers | as recorded, newest 200 runs |
| | `start` | when the run began | the date only |
| | `host`, `pid` | hostname, process id | blank, dropped |
| | per-file records | every file your jobs read | **never sent**; the client derives one list per run of the origin *domains* those files came from (`["root://cern.ch"]`) |
| machine block | `os`, `os_release`, `arch`, `kernel`, `cpu_model`, `ncpu`, `mem_gb`, `xrootd_client`, `ucache_version` | hardware and software facts | as gathered |
| | `id` | | a salted hash of the hostname |
| label | `label` | text you typed | as typed, at most 80 characters. The client refuses a label containing `/`, `\`, `@` or `~`; the service refuses one matching its place patterns (`/home/`, `/eos/`, `/data/`, `user@host`, an IP address). Neither can recognise every path: type a name, never a location |
| all records | everything else (`fs`, `mode`, `dev` as major:minor, `total_gb`, `free_gb`, `error`, `build_id`, the write-shape words, every count) | numbers, fixed vocabulary, or an error message from the C library | as recorded |

The identifiers are what link records without revealing what they hash:

| id | made from | where it lives |
|---|---|---|
| owner | random | the identity string (below) |
| **salt** | random, 128 bits | the identity string — **never sent** |
| machine | `sha256(salt, hostname)` | computed each time |
| location (a benchmarked directory) | `sha256(salt, hostname, path)` | computed; written into the record |
| volume (its mount point) | `sha256(salt, hostname, mount point)` | computed; written into the record |
| cache instance | random, minted once | `<cache dir>/install-id`; survives `ucache clear` |

Because the salt never leaves your machine, nobody who sees the hashes — us
included — can test a guess at a path or a hostname against them.

## The identity string

`ucache-id:<owner-uuid>:<salt-hex>` is one line in one file
(`~/.config/ucache/identity`, honouring `XDG_CONFIG_HOME`, or
`$UCACHE_IDENTITY_FILE`), created by your first `publish` or `--publish` and
printed once, with the notice below. It is the key to your
owner page — everything you publish, grouped by machine, disk and cache — and
the only way a second machine or a browser lands on the same page:

```sh
ucache identity                    # show it (and where it lives)
ucache identity --set 'ucache-id:…' # install it on another machine
ucache identity --new              # start over under a new owner
```

Treat it like a password. It contains the salt, so anyone holding it can
compute the same hashes; and anyone holding the owner URL can read the pages.
Do not paste it into a ticket or a chat.

Two identities that should have been one (a browser and a CLI, a lost file)
can be merged on either owner page: paste the other identity's owner id there.
Holding both is the proof of ownership; there are no accounts.

## What you get back

```
report   https://ucache.web.cern.ch/s/3f9a…        this submission, frozen
cache    https://ucache.web.cern.ch/r/9c2e…        the cache instance: every submission, the trend
owner    https://ucache.web.cern.ch/u/7b1d…        everything you published
findings
  [good] Storage grade GOOD: 1-thread random 4 KiB read p50 0.13 ms …
  [info] No origin measurement for this machine yet: run `ucache netbench` …
```

The URLs are unguessable and unlisted, and they are the only access control:
whoever has one can read that page. The owner URL is the one to keep private.
Every finding links to a page explaining the rule behind it and how to read
the numbers yourself.

## Where things are kept locally

- `~/.local/share/ucache/records.jsonl` (honouring `XDG_DATA_HOME`, or
  `$UCACHE_RECORDS_FILE`): every `bench` and `netbench` record, **raw and
  unredacted**, plus the labels you gave (with the hostname and path they
  belong to) and a line per published report. `bench --publish` and `publish`
  read it: it is how a later `publish` sends a cache's disk measurements with
  its history, and how a label is asked once. Private to you (mode 0600).
  Delete it whenever you like; nothing else reads it.
- `<cache dir>/install-id`: the cache instance's identity. Written
  world-readable on purpose, so a second user reading the same cache
  publishes to the same instance page; anyone who can read the cache
  directory can therefore reach that page. Delete the file and the next
  publish starts a new instance page; the old one stays.
- `last-payload.json`, beside the record store: written only when a publish
  fails, so you can inspect exactly what was refused and retry by hand with the
  printed `curl` command.

## A redacted sample

`ucache publish --dry-run`, for a cache whose disk was benchmarked once with
`ucache bench --threads 8 /scratch/cache`, prints this (shortened; the ids are
placeholders, as they are in every dry run). A `bench --dry-run` payload is the
same without the top-level `install_id`, `location` and `volume` and without
the `history` block; a `netbench --dry-run` payload has a `netbench` record
instead of a `bench` one.

```json
{
  "schema": 1,
  "kind": "publish",
  "ucache_version": "1.0.0",
  "owner_id": "00000000-0000-4000-8000-000000000000",
  "install_id": "00000000-0000-4000-8000-000000000001",
  "location": "11111111111111111111111111111111",
  "volume": "22222222222222222222222222222222",
  "label": "scratch SSD",
  "machine": {
    "id": "00000000000000000000000000000000",
    "os": "almalinux", "os_release": "9.5", "arch": "x86_64",
    "kernel": "5.14.0-503.el9_5.x86_64",
    "cpu_model": "Intel(R) Xeon(R) Gold 6338 CPU @ 2.00GHz",
    "ncpu": 32, "mem_gb": 128.0,
    "xrootd_client": "v5.8.3", "ucache_version": "1.0.0"
  },
  "bench": [
    {
      "schema": 1, "host": "", "fs": "xfs", "mode": "O_DIRECT",
      "file_mb": 1024, "randr1_iops": 8930, "randr1_us_p50": 105, "randr16_iops": 71204,
      "seq_read_mbps": 512.3, "seq_write_mbps": 318.0, "fsync_p50_ms": 1.92,
      "cmd": "ucache bench --threads 8 <path>",
      "mount_fstype": "xfs", "dev_model": "SAMSUNG MZ7L31T9", "dev_rotational": 0, "dev_size_gb": 1920.0,
      "location": "11111111111111111111111111111111",
      "volume": "22222222222222222222222222222222",
      "install_id": "00000000-0000-4000-8000-000000000001"
    }
  ],
  "netbench": [],
  "history": { "schema": 1, "totals": { "runs": 38, "gain": 2.878 },
               "runs": [ { "start": "2026-08-27", "host": "", "kind": "warm", "gain": 4.514,
                           "origins": ["root://cern.ch"] } ] }
}
```

The real run replaces the placeholders with the hashes and ids described above
and nothing else changes. `ucache bench` also prints the same redacted record
as a `ucache-bench-public:` line once an identity exists — safe to paste
anywhere, unlike the `ucache-bench-json:` line above it, which carries the
path and hostname for your own log.

## When it does not go through

- **`curl` is required.** The client shells out to the system's `curl`
  (`UCACHE_CURL` names another one). Without it nothing is sent and the
  payload is saved for a manual retry.
- **Rate limits.** The service accepts a handful of publishes an hour from a
  new identity and a few new identities a day from one address; a `429`
  reply carries a wait time, which the client honours when it is short and
  otherwise reports.
- **Refusals name the field.** A payload that matches the service's place
  patterns — in practice only a label can, and only one the client's own
  check let through — is refused with `HTTP 422` and the field, the payload is
  kept at the path printed, and the `curl` command to resend it is shown.
  A reply that accepts the payload but carries no report URL is reported as
  a failure too, without a retry: the service has the data.
- **Connection failures and restarts** are retried three times (2, 8 and
  30 seconds apart). `publish` is a foreground command, so a failure is a
  non-zero exit and a message, never a retry in the background.

## Removing what you published

Open an issue on the project's GitHub repository with the report URL, the
install id or your owner id, and the submissions under it are deleted whole
(they remain in the service's backups for its retention period, which the
privacy page states). A merged owner can be un-merged from the owner page;
deleting `install-id` or running `ucache identity --new` starts fresh pages
without touching the old ones.
