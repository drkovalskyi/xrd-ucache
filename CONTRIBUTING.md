# Contributing

Pull requests are not being accepted at the moment. Issues are — bug reports,
questions, and reports from real deployments are all useful, and they are the
best way to influence what happens next.

## Most useful to report

- **A read that failed, or returned wrong data.** uCache is meant to fall back
  to reading from the origin whenever anything is wrong, so a job that broke
  rather than merely slowed down is the most serious kind of bug here. Report
  it even if you cannot reproduce it.
- **Platforms and storage systems we cannot test ourselves** — other XRootD
  client versions, other analysis frameworks, unusual filesystems for the
  cache directory.
- **Performance that does not match what the documentation describes**, with
  numbers.

## What to include

    ucache --version      # version and build id
    ucache doctor         # configuration and activation check
    ucache stats          # counters, if the report is about performance

Plus the platform, the XRootD client version, and what you expected set against
what happened. `ucache test <url>` is a quick end-to-end check that reports
whether the plugin engaged at all.

## Patches

If you have a fix, describe it in an issue, with a diff if you have one. It may
well get applied, with credit. This will open up later; the constraint is
review time, not interest.

## AI-assisted reports

Welcome — this project uses such tools itself (see the README). The conditions
are about responsibility, not tooling: check the claims before filing, and do
not submit unreviewed output from an autonomous agent. A report describing
behaviour that nobody observed costs more to answer than one that never
arrives.
