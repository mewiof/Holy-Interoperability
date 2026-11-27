# Holy Interoperability

A compatibility layer that runs x64 console title packages as native Windows processes.

Current-generation Xbox titles are ordinary x64 Windows executables that link against a set of console-only
system libraries. Holy Interoperability starts such an executable directly on a PC and supplies those
libraries itself, as clean-room reimplementations on top of the host's own APIs. There is no CPU emulation, no
virtual machine and no separate runtime process.

> **Status: bring-up.** A retail title boots, creates its graphics device and tens of thousands of pipelines
> translated from native GPU code, and presents frames at its own pace within seconds of starting, but what it
> draws is not visible yet and no title is playable. See [Progress](#progress).

## Progress

_Last updated: 2026-11-27._ The goal is a reference title running as smoothly as on the console: a correct
picture, sound and input at a stable 30 frames a second or better. The percentages are rough estimates of how
much of the work toward that goal each area has, not test coverage.

**Overall: about 20%** `████░░░░░░░░░░░░░░░░`

```mermaid
flowchart LR
  load[Loads and binds] --> boot[Runtime services] --> gpu[GPU device and pipelines] --> first[First frame after 3 s]
  first --> stable[Runs for minutes] --> picture[Correct picture] --> menu[Menus and input] --> game[In game] --> fps[Stable 30 FPS]
  classDef done fill:#2da44e,color:#ffffff,stroke:#1a7f37
  classDef active fill:#d29922,color:#ffffff,stroke:#9a6700
  classDef open fill:#f6f8fa,color:#57606a,stroke:#d0d7de
  class load,boot,gpu,first done
  class stable active
  class picture,menu,game,fps open
```

| Milestone (reference title) | State |
| --- | --- |
| The image loads, imports bind, runtime services answer | ✅ done |
| Graphics device, title memory as GPU memory, pipelines from native GPU code | ✅ done; translated rendering not yet checked against known results |
| Start-up in seconds, memory within the host's means | ✅ first frame after 3 s; about 5 GiB of commit at the first frames (was 17 GiB) |
| Frames presented at the title's pace | ✅ 30 a second, measured |
| Runs for minutes without crashing | 🟡 the fault seconds after the first frame is fixed (a calling-convention bug); next: a breakpoint in the title's own code about 18 s in |
| No memory leaks (RAM, VRAM, system commit) | 🟡 live-object counters in every run; host heaps no longer leak system commit after exit, released textures free their host copies |
| A correct picture | ⬜ frames are black |
| Menus and input | ⬜ no input devices yet |
| Sound | ⬜ silent audio device |
| In game at a stable 30 FPS, no leaks | ⬜ |

| Area | Progress |
| --- | --- |
| Loader, sandbox, runtime, import binding | `██████████` 95% |
| Title memory (`xmem`, commit on demand) | `████████░░` 80% |
| Fast storage | `███████░░░` 70% |
| Game runtime services | `██████░░░░` 60% |
| Graphics: device, resources, command recording | `█████░░░░░` 50% |
| Graphics: shader translation | `████░░░░░░` 40% |
| Graphics: pipelines and caches | `███████░░░` 70% |
| Graphics: correct output | `█░░░░░░░░░` 5% |
| Graphics: performance | `█░░░░░░░░░` 10% |
| Input | `█░░░░░░░░░` 10% |
| Audio | `█░░░░░░░░░` 10% |
| Measurement: statistics, profiler, leak checks | `███████░░░` 70% |

## How it works

- **Loader** ([core/loader](core/loader)) — starts the title's executable in place, suspended, and replaces
  its import directory *in the new process's memory* with a single import of the runtime. Nothing is patched
  on disk and the package directory is never written to.
- **Runtime** ([core/runtime](core/runtime)) — `holyrt.dll`, loaded into the title process. It binds every
  import of the title itself: to the host OS, to a DLL the package ships, to one of this project's console
  libraries, or to a thunk that reports precisely which function is still unimplemented when the title calls
  it. It also provides the title's view of the world: the package mounted read-only, scratch storage, console
  identity, lifecycle notifications, crash reports.
- **Console libraries** ([core/libs](core/libs)) — one DLL per reimplemented console system library, each
  declared from public documentation and from the calls titles observably make.
- **`holy`** ([tools/holy](tools/holy)) — the command line front end: `holy inspect` predicts how a title's
  imports will bind without running anything; `holy run` starts it.

The title process runs in a sandbox by default (low integrity level), so the operating system itself refuses
any write to the package or to the rest of the user's files, independently of this project's own path layer.

More detail: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Status

| Area | State |
| --- | --- |
| Loader: in-memory import takeover, sandbox, job control, timeouts | Working, tested end to end |
| Runtime: import binder, reporting thunks, call tracing, crash reports | Working, tested end to end |
| Title environment: package mount, read-only package, scratch storage, console identity, lifecycle registration | Working, tested end to end |
| `xmem` (title memory) | Heap, virtual and physical-page allocation and mapping (page numbers as titles expect them); commit on demand; lazily backed physical pages |
| `d3d12_xs` (graphics) | Device on the host's Direct3D 12, queues, fences, descriptor heaps, views, resources and heaps in title memory, pipelines from DXIL or from native GPU code translated to host bytecode (compute, vertex, pixel and NGG programs), compiled at first use and shared by content, translations cached on disk, command recording (state, root arguments, draws, dispatches, barriers), queue-ordered page mapping updates, presentation onto the title's window paced at its frame interval, captured frames; parts of translated code checked on the host GPU, translated rendering as a whole not yet checked against known results |
| `xgameruntime` (game runtime) | Entry points, task queues and async calls, networking (offline), users (nobody signed in), system identity, package state, persistent local storage, display mode, speech synthesis (refused by the host engine inside the sandbox); other services missing |
| `xcrdapi` (fast storage) | Factory, file opening, status arrays, queues, file and memory requests including zlib (processed synchronously) |
| `gameinput` (input) | Interface present; no input devices surfaced yet |
| Offline switch, interposers for DLLs the package ships | Working, tested end to end |
| `xg_xs` (texture layout) | Swizzle mode suggestion, texture copies into a desktop-compatible layout, tiling queries |
| Audio console libraries (`AcpHal`, `xapu`, `chad`) | Audio processor heap; shape contexts, control and audio processors as a silent device (nothing is decoded, mixed or played) |
| Measurement | Statistics report in the runtime log (memory, live objects, frame rate, GPU memory) every ten seconds; `holy run --profile` sampling profiler |

What this means in practice: a retail title image loads, runs its static initialisation, initialises the
game runtime, creates its window, graphics device and over 80,000 pipelines (shaders translated from native
GPU code), streams its start-up data through fast storage, and presents frames onto a swap chain on its window
at 30 a second from 3 seconds after start, with about 5 GiB of commit. Its frames are still black, and it
stops at a breakpoint in its own code about 18 seconds in (see [docs/ROADMAP.md](docs/ROADMAP.md)). No
title is playable.

Progress is measured as *console library coverage*: of the console-only symbols a title imports, how many
have a real implementation. `holy inspect` prints it for any package. Placeholder exports never count.

The plan from here is in [docs/ROADMAP.md](docs/ROADMAP.md).

## Quick start

Requirements: Windows 10/11 x64, CMake 3.25+, Ninja, and either LLVM (`clang-cl`) or Visual Studio 2022+ with
the C++ workload.

```sh
cmake --preset clang-cl
cmake --build --preset clang-cl
ctest --preset clang-cl
```

Then point the tool at a title package directory (a directory containing `MicrosoftGame.config` and the
title's files as the title sees them at run time):

```sh
build/clang-cl/bin/holy inspect <package-dir>
build/clang-cl/bin/holy run <package-dir>
```

Details: [docs/BUILD.md](docs/BUILD.md), [docs/USAGE.md](docs/USAGE.md).

## What this project does not do

- It does not ship, download or link any title, console system file, firmware, key, or SDK header.
- It does not decrypt, unpack or convert console package containers, and will not.
- It does not emulate online services, sign users in, or grant entitlements.
- It does not name, list or track individual titles. Compatibility is described per platform API.

## Disclaimer

This project is intended for interoperability, research, preservation, and compatibility purposes.

It does not include, distribute, or require copyrighted software, firmware, cryptographic keys, or proprietary
libraries. At run time it uses only the host operating system, the files of the package the user supplies, and
its own code.

Users are responsible for ensuring that any binaries used with this project are obtained and used in
accordance with applicable laws and their respective license terms.

Holy Interoperability is an independent project and is not affiliated with, authorized, or endorsed by
Microsoft Corporation. "Xbox" and "Windows" are trademarks of the Microsoft group of companies and are used
here only to describe what the software interoperates with.

## Contributing

Read [CONTRIBUTING.md](CONTRIBUTING.md) first. The content rules there (no title names, no files the project
does not own, clean-room sources only) are not negotiable and are checked by
[tools/hygiene/check.py](tools/hygiene/check.py).

## License

[MIT](LICENSE).
