# Holy Interoperability

A compatibility layer that runs x64 console title packages as native Windows processes.

Current-generation Xbox titles are ordinary x64 Windows executables that link against a set of console-only
system libraries. Holy Interoperability starts such an executable directly on a PC and supplies those
libraries itself, as clean-room reimplementations on top of the host's own APIs. There is no CPU emulation, no
virtual machine and no separate runtime process.

> **Status: early bring-up.** The loader and runtime foundation work and are covered by tests. Real title
> images load, bind and run their startup code, but nothing reaches rendering yet. See [Status](#status).

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
| `d3d12_xs` (graphics) | Device on the host's Direct3D 12, queues, fences, descriptor heaps, views, resources and heaps in title memory, pipelines from DXIL or from native GPU code translated to host bytecode, command recording (state, root arguments, draws, dispatches, barriers), queue-ordered page mapping updates, presentation onto the title's window with captured frames; quad permutes and format loads of translated code checked on the host GPU, the rest of translated rendering not yet checked against known results; the host device is sometimes removed as hung during the reference title's first frames (cause open) |
| `xgameruntime` (game runtime) | Entry points, task queues and async calls, networking (offline), users (nobody signed in), system identity, package state, persistent local storage, display mode, speech synthesis (refused by the host engine inside the sandbox); other services missing |
| `xcrdapi` (fast storage) | Factory, file opening, status arrays, queues, file and memory requests including zlib (processed synchronously) |
| `gameinput` (input) | Interface present; no input devices surfaced yet |
| Offline switch, interposers for DLLs the package ships | Working, tested end to end |
| `xg_xs` (texture layout) | Swizzle mode suggestion, texture copies into a desktop-compatible layout |
| Audio console libraries | Not started: the reference title stops at its first call into them (`ApuCreateHeap`) when its graphics device survives that far |

What this means in practice: a retail title image loads, runs its static initialisation, initialises the
game runtime, creates its window, graphics device and pipelines (shaders translated from native GPU code),
streams its start-up data through fast storage, records its first command lists, and presents its first
frame onto a swap chain on its window, about half a minute after start. It then stops in its own start-up
code: its movie player's pipelines are built from inconsistent shader sets, and the title uses them before
they exist (see [docs/ROADMAP.md](docs/ROADMAP.md)). Nothing it draws has been seen yet and no title is
playable.

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
