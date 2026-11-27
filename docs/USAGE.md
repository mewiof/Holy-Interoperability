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
| `--network <offline\|host>` | `offline` (default): the title sees a disconnected network; `host`: it may use the host's |
| `--unimplemented <trap\|continue>` | what a call into an unimplemented import does; default `trap` |
| `--trace <patterns>` | comma separated import-library patterns to trace, e.g. `xmem.dll,d3d12*` |
| `--log-level <level>` | `trace`, `debug`, `info` (default), `warn`, `error` |
| `--timeout <seconds>` | stop the title after this long |
| `--profile [ms]` | sample the title's threads (default every 2 ms) into `<data>/logs/profile.txt` |

Environment: `HOLY_PIPELINES=eager|warm|lazy` chooses when host pipelines are compiled: `warm` (default) at first
use, and at creation for pipelines used in an earlier run; `eager` all at creation; `lazy` only at first use.

### Measuring

Every ten seconds, and when the title ends or crashes, the runtime log gets a statistics report: the process's
commit and working set, the system's commit, and named counters with their change since the previous report
(`d3d12.presents` is the frame count, `d3d12.frame_gap_max_ms` the longest frame of the last ten seconds,
`d3d12.vram_local_mb` the host GPU memory in use, `xmem.*` and `memory.*` title memory). A counter of live objects
that only grows while the title does the same thing over and over is a leak. `--profile` adds where the
title's threads spend their time.

Debugging aids, set in the environment:

| Variable | Effect |
| --- | --- |
| `HOLY_D3D_DEBUG=1` | the host's Direct3D 12 debug layer and device-removed data, its errors in the runtime log (also on with `--log-level debug`) |
| `HOLY_D3D_DRED=1` | only the device-removed data (what the GPU was doing, page faults), without the debug layer's checks and their cost |
| `HOLY_D3D_CHECK=1` | after every call into the device, its queues and command lists (and the runtime's own host calls), the host device is checked; the first call after which it was removed is logged with the 32 calls before it |
| `HOLY_D3D_TRACE=<n>` | every call between presents `n-1` and `n` is logged, with what draws, clears, barriers, pipelines, render targets and viewports were given: an account of one frame |
| `HOLY_CAPTURE_EVERY=<n>` | besides presents 1, 30 and 300, write every `n`th presented frame to `<data>/frames/present-<n>.bmp` |
| `HOLY_DUMP_TARGETS=<n>` | at present `n`, every live texture of the title is written to `<data>/frames/textures-<n>/` and its per-channel minimum and maximum logged |
| `HOLY_WATCH=at=<offset>;if=<reg>+<off>:<value>;watch=<reg>+<off>` | hardware watchpoints: when the title executes `title+<offset>` (and the 32-bit value at `<reg>+<off>` matches), writes to the 8 bytes at the `watch` address (`*<reg>+<off>`: at the address stored there) are logged with their stacks, in every thread, for the three latest such addresses |

In exploration mode (`--unimplemented continue`), a pipeline whose creation reports itself as unimplemented is
handed out as a placeholder whose draws are dropped (counted as `d3d12.placeholder_pipelines`), so the title's
later calls show.

Exit codes: `0` the title exited with 0; `1` the title exited with a failure code; `2` usage error; `3` the
title could not be started; `4` stopped by `--timeout`; `5` the title called an unimplemented import.

### What to expect today

Titles do not boot yet. A run ends at the first console function that is not implemented, with a report like:

```
E [import] unimplemented import <library>!<function> called from title+0x…
E [import]   rcx=… rdx=… r8=… r9=…
```

That line is the project's to-do list for the title, in order. See [ROADMAP.md](ROADMAP.md).

### Input

Titles see one gamepad: the host's first XInput controller, and the keyboard while the title's window has the
focus:

| Gamepad | Keyboard |
| --- | --- |
| left stick | W A S D |
| right stick | I J K L |
| d-pad | arrow keys |
| A / B / X / Y | Space / Escape or Backspace / R / F |
| menu / view | Enter / Tab |
| left / right shoulder | Q / E |
| left / right trigger | Z / C |
| left / right stick click | Left Shift / V |

### The network

By default the title runs offline: it is told there is no connectivity, host names do not resolve, and
connections to anything but the local host fail, for the title image and for every DLL the package ships.
`--network host` lets it use the host's network. The project never signs anyone in or emulates an online
service either way.

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
