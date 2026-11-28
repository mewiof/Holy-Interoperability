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
| `--user <none\|local>` | `none` (default): nobody is signed in; `local`: a local user is present, a profile of this host signed in to no service (titles wait at their account prompt without one). Its game saves are kept in the data directory's `saves` folder |
| `--unimplemented <trap\|continue>` | what a call into an unimplemented import does; default `trap` |
| `--trace <patterns>` | comma separated import-library patterns to trace, e.g. `xmem.dll,d3d12*` |
| `--log-level <level>` | `trace`, `debug`, `info` (default), `warn`, `error` |
| `--timeout <seconds>` | stop the title after this long |
| `--profile [ms]` | sample the title's threads (default every 2 ms) into `<data>/logs/profile.txt` |
| `--record-audio` | record what the title's process tree plays (the host's process loopback capture, 48 kHz stereo) into `<data>/logs/audio.wav`; `python tools/audio_report/audio_report.py <wav>` reports loudness, silence and sound/silence changes per second (a starved stream shows as sound broken by short silences many times a second) |

Environment: `HOLY_PIPELINES=eager|warm|lazy` chooses when host pipelines are compiled: `warm` (default) at first
use, and at creation for pipelines used in an earlier run; `eager` all at creation; `lazy` only at first use.

### Measuring

Every ten seconds, and when the title ends or crashes, the runtime log gets a statistics report: the process's
commit and working set, the system's commit, and named counters with their change since the previous report
(`d3d12.presents` is the frame count, `d3d12.frame_gap_max_ms` the longest frame of the last ten seconds,
`d3d12.vram_local_mb` the host GPU memory in use, `d3d12.host_textures_from_memory` and
`d3d12.host_texture_bytes_from_memory` the host copies of textures the title views in its memory, `xmem.*` and
`memory.*` title memory). A counter of live objects
that only grows while the title does the same thing over and over is a leak. `--profile` adds where the
title's threads spend their time.

Debugging aids, set in the environment:

| Variable | Effect |
| --- | --- |
| `HOLY_D3D_DEBUG=1` | the host's Direct3D 12 debug layer and device-removed data, its errors in the runtime log (also on with `--log-level debug`) |
| `HOLY_D3D_DRED=1` | only the device-removed data (what the GPU was doing, page faults), without the debug layer's checks and their cost |
| `HOLY_D3D_CHECK=1` | after every call into the device, its queues and command lists (and the runtime's own host calls), the host device is checked; the first call after which it was removed is logged with the 32 calls before it |
| `HOLY_TEXTURE_TRACKING=0` | host copies of textures in title memory are made once and not kept current (for comparing) |
| `HOLY_BUFFER_COPIES=<MiB>` | copies in video memory of title memory translated programs read through bound buffers, up to that many MiB (off by default until checked on the reference title; the live count is `d3d12.cached_pages`) |
| `HOLY_D3D_TRACE=<n>[+<count>]` | every call between presents `n-1` and `n` (from the start for `n` = 1; with `+<count>`, through the `count-1` presents after it, as recording a frame can outlast a present interval while every call is logged) is logged, with what draws, clears, barriers, pipelines, render targets, viewports, blend factors and stencil references were given, and for draws their vertex buffers (first vertex as floats), root arguments and, behind root arguments that point into a shader-visible heap, the image descriptors there with the heap slot they name and what the view in that slot was made as (resource address, size, formats): an account of one frame |
| `HOLY_CAPTURE_EVERY=<n>` | besides presents 1, 30 and 300, write every `n`th presented frame to `<data>/frames/present-<n>.bmp` (a run's first capture removes the earlier runs' captures, and a texture dump the earlier dumps: a 4K capture is 33 MB) |
| `HOLY_CAPTURE_FROM=<n>[+<count>]` | also write presents `n` to `n+count-1` (8 by default): consecutive frames, for what changes from frame to frame (flicker) |
| `HOLY_INPUT_SCRIPT=<presses>` | timed presses of the gamepad for runs without a person at the controls: `<when>:<controls>[/<ms>]` separated by `;`, `<when>` seconds since the process started (`8.5`, or `10-60/2` for every 2 s from 10 s to 60 s), `<controls>` joined by `+` from `A B X Y Menu View Up Down Left Right LB RB LS RS LT RT LSUp LSDown LSLeft LSRight RSUp RSDown RSLeft RSRight`; presses last 150 ms unless `<ms>` says otherwise |
| `HOLY_XMEM_TRACE=1` | every physical page mapping and unmapping, title memory reservation and large allocation in the log |
| `HOLY_XG_TRACE=1` | the tiling the texture layout library reports to titles, in the log |
| `HOLY_TRACE_DUMP=<symbol>:<argument>:<bytes>` | with `--trace`, a traced call of `symbol` also logs `bytes` (up to 1024) of the memory its register argument `argument` (0-3) points to |
| `HOLY_CHECKER_PROGRAMS=<hashes>\|all` | pipelines with these pixel programs (comma separated hashes, as `HOLY_D3D_TRACE` names them: `host <vertex> <pixel>`) draw a purple and black checkerboard instead, showing where draws land whose shading makes them invisible. Pipelines the host refuses always fall back to the checkerboard (logged), so missing programs show on screen |
| `HOLY_DUMP_TARGETS=<n>` | at present `n`, every live texture of the title is written to `<data>/frames/textures-<n>/` and its per-channel minimum and maximum logged (depth textures with stencil also their stencil plane, as integers: `-stencil` files); the log lists each depth texture in title memory with the other resources over the same memory |
| `HOLY_DESCRIBE_PIPELINES=<hashes>\|all` | pipelines with these programs (as traces name them) are described in the log when compiled: blend, depth and stencil, rasterizer, formats, and their host programs saved as `<data>/logs/programs/<hash>.dxbc` (for `fxc -dumpbin`); console pipelines' render state blocks are logged as they are made. `stencil` in the list adds every console pipeline that uses the stencil or writes depth, and every compiled pipeline that tests the stencil. Program names follow the translator's build: take them from a trace of the same build |
| `HOLY_AUDIO_TRACE=1` (or a number: the events kept, 4000 by default) | the audio libraries write what titles send them to `<data>/logs/audio-xapu.txt` (configuration, every 64-byte command and the record it names), `audio-chad.txt` (connections, commands) and `audio-acphal.txt` (shape contexts, control processor commands and payloads): one event a line, milliseconds then dwords in hexadecimal, for studying the audio processors' commands |
| `HOLY_WATCH=at=<offset>;if=<reg>+<off>:<value>;watch=<reg>+<off>` | hardware watchpoints: when the title executes `title+<offset>` (and the 32-bit value at `<reg>+<off>` matches), writes to the 8 bytes at the `watch` address (`*<reg>+<off>`: at the address stored there) are logged with their stacks, in every thread, for the three latest such addresses; `HOLY_WATCH=watch=title+<offset>` watches writes to that address from the start instead |

In exploration mode (`--unimplemented continue`), a pipeline whose creation reports itself as unimplemented is
handed out as a placeholder whose draws are dropped (counted as `d3d12.placeholder_pipelines`), so the title's
later calls show.

`holy run` warns, before starting the title and after a failure, when the host has less than 8 GiB of commit
left: titles commit several GiB, and a host short of it fails them in ways that point elsewhere (the GPU device
removed, the title's own allocations failing).

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

### Reading a run

```sh
python tools/run_report/run_report.py <data>/logs/runtime.log [--warmup SECONDS] [--min-fps FPS]
```

Turns the runtime log's statistics reports into a verdict: frames a second for every ten seconds, the longest
frame, frames over 50 ms, commit and working-set growth a minute after the warm-up (default 60 s, loading), the
least free system commit (under 1 GiB, failures are suspect), and leak suspects: counters of live objects or
held bytes that grew in every report after the warm-up. With `--min-fps` the exit code says what failed (1: a
steady interval below it, 2: a leak suspect, 3: both, 4: no reports), for scripts that run a title and check it.

```sh
python tools/run_report/check_title.py <package-dir> [--seconds N] [--min-fps FPS] [--warmup SECONDS] [-- holy run options]
```

Runs the title for N seconds (default 120) with `holy run --timeout`, keeps the run's log as
`<data>/logs/runtime-<time>.log` and prints its report; the exit code is the report's, or 5 when the title could
not be started.

### Translating shaders offline

```sh
holy_translate_corpus <data>/shaders [--threads N] [--report <file>] [--compile]
holy_translate_corpus --translate <shader file> <container file>
```

The runtime saves the shaders a title passes to pipeline creation in its data directory (`shaders/`). This
tool translates every one of them the way the runtime does, on all processors, and reports how many translate
for each kind of program, the failures grouped by reason, and the translation time: a way to find gaps in
the translator without running the title. It only reads the directory; keep any report outside the repository.
`--compile` also compiles each translation with the host's shader compiler (compute and NGG programs as compute
pipelines, vertex programs alone, pixel programs after the vertex program the runtime pairs them with), lists
what the host refuses (`HOLY_D3D_DEBUG=1` adds the debug layer's reasons) and reports the time by translation
size; each program gets a store of a unique value first, so the host's shader caches (which key on
the code as optimised, not on the container) never answer and repeated runs measure the same. `--translate`
writes one shader's host container, for `fxc -dumpbin`.
