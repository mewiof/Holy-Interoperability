# Usage

## What you need

A **title package directory**: the files of a title laid out the way the title sees them at run time, with
`MicrosoftGame.config` at the root next to the executable it names.

This project does not obtain, decrypt, unpack or convert packages, and it does not ship any console file. You
are responsible for ensuring that any binaries you use were obtained and are used in accordance with
applicable law and their license terms.

The package directory is treated as strictly read-only. `holy` never writes into it and prevents the title
from doing so.

## Inspect

```sh
holy inspect <package-dir> [--all]
```

Reads the manifest and the executable and reports, without loading or running any package code:

- which provider serves each imported library (`runtime`, `package`, `host`, `missing`);
- how many of each library's symbols resolve, and the list of unresolved ones;
- the console library coverage for this title.

`--all` adds the same report for every DLL in the package root.

## Run

```sh
holy run <package-dir> [options] [-- <title arguments>]
```

| Option | Meaning |
| --- | --- |
| `--exe <name>` | executable inside the package; default is the retail console executable from the manifest |
| `--data-dir <dir>` | writable per-title directory; default is per user and per title (see below) |
| `--no-sandbox` | do not lower the title's integrity level |
| `--console <series-x\|series-s>` | console model reported to the title; default `series-x` |
| `--unimplemented <trap\|continue>` | what a call into an unimplemented import does; default `trap` |
| `--trace <patterns>` | comma separated import-library patterns to trace, e.g. `xmem.dll,d3d12*` |
| `--log-level <level>` | `trace`, `debug`, `info` (default), `warn`, `error` |
| `--timeout <seconds>` | stop the title after this long |

Exit codes: `0` the title exited with 0; `1` the title exited with a failure code; `2` usage error; `3` the
title could not be started; `4` stopped by `--timeout`; `5` the title called an unimplemented import.

### What to expect today

Titles do not boot yet. A run ends at the first console function that is not implemented, with a report like:

```
E [import] unimplemented import <library>!<function> called from title+0x…
E [import]   rcx=… rdx=… r8=… r9=…
```

That line is the project's to-do list for the title, in order. See [ROADMAP.md](ROADMAP.md).

### The sandbox

By default the title runs at low integrity level. The operating system then refuses writes to the package and
to practically all of your files no matter which code attempts them. Leave it on unless you are debugging the
sandbox itself.

With the sandbox on, a custom `--data-dir` must be writable by low-integrity processes; the default location
is.

### Data directory and logs

Default: `%USERPROFILE%\AppData\LocalLow\HolyInterop\titles\<key>\`, where `<key>` is derived from ids in the
manifest.

```
logs/runtime.log    log of the last run (overwritten each run)
temp/               the title's T: drive
scratch/            the title's D: drive
```

Logs replace the package and data locations with `<package>` and `<data>` and refer to the executable as
`title`. They still describe one specific title's behaviour: when reporting a problem, quote the lines about
the platform API and do not name the title (see [CONTRIBUTING.md](../CONTRIBUTING.md)).

### Exploration mode

```sh
holy run <package-dir> --unimplemented continue --trace "xmem.dll,d3d12_xs.dll" --log-level debug --timeout 60
```

Unimplemented calls return zero instead of stopping the title, so later calls become visible. This is a tool
for finding out what a title asks for. Whatever happens after the first such call is not meaningful behaviour.

Tracing only ever wraps imported functions (data imports are left alone), so any library can be named, but
tracing everything (`*`) is slow and noisy.
