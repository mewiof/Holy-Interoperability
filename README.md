# Holy Interoperability

A compatibility layer that runs x64 console title packages as native Windows processes.

Current-generation Xbox titles are ordinary x64 Windows executables that link against a set of console-only
system libraries. Holy Interoperability starts such an executable directly on a PC and supplies those
libraries itself, as clean-room reimplementations on top of the host's own APIs. There is no CPU emulation, no
virtual machine and no separate runtime process.

> **Status: bring-up.** A retail title boots, creates its graphics device and tens of thousands of pipelines
> translated from native GPU code, runs steadily at 30 frames a second for as long as it was tested (90 s, with
> the host's graphics debug layer finding nothing), and shows its first screen; no title is playable yet. See
> [Progress](#progress).

## Progress

_Last updated: 2026-11-27 (evening)._ The goal is a reference title running as smoothly as on the console: a correct
picture, sound and input at a stable 30 frames a second or better. The percentages are rough estimates of how
much of the work toward that goal each area has, not test coverage.

**Overall: about 44%** `████████▊░░░░░░░░░░░`

```mermaid
flowchart LR
  load[Loads and binds] --> boot[Runtime services] --> gpu[GPU device and pipelines] --> first[First frame after 3 s]
  first --> stable[Runs for minutes] --> picture[Correct picture] --> menu[Menus and input] --> game[In game] --> fps[Stable 30 FPS]
  classDef done fill:#2da44e,color:#ffffff,stroke:#1a7f37
  classDef active fill:#d29922,color:#ffffff,stroke:#9a6700
  classDef open fill:#f6f8fa,color:#57606a,stroke:#d0d7de
  class load,boot,gpu,first,stable done
  class picture,menu active
  class game,fps open
```

| Milestone (reference title) | State |
| --- | --- |
| The image loads, imports bind, runtime services answer | ✅ done |
| Graphics device, title memory as GPU memory, pipelines from native GPU code | ✅ done; translated rendering not yet checked against known results |
| Start-up in seconds, memory within the host's means | ✅ first frame after 3 s; about 5 GiB of commit at the first frames (was 17 GiB) |
| Frames presented at the title's pace | ✅ 30 a second, measured |
| Runs for minutes without crashing | ✅ 5 minutes at a steady 30 frames a second (longest frame 49 ms), commit and video memory flat, no live counter growing; 90 s with the host's debug layer finding nothing. The "invalid call" device removals were views reaching past their buffers and textures (the console allows them, the host cannot report them but by removing the device): views and copies now stay inside their resources |
| No memory leaks (RAM, VRAM, system commit) | 🟡 live-object counters in every run; host heaps no longer leak system commit after exit; released textures free their host copies; title memory the GPU used is given back when the title frees it (it stayed charged: 1 GiB over 16 rounds of 64 MiB, now none; an end-to-end check); a fence's value no longer keeps a thread and a host fence after the fence is released; root argument memory goes back when lists are reset (live count) |
| A correct picture | 🟡 the first screens show as they should (the account prompt, its text and button glyphs, a full-screen background) after frames were black. Fixed since: rectangle lists completed from the corner their sides meet at (the background covered half the screen); before that, with checks in the sample title that fail without each fix, vertices of draws without indices, native pipelines' render targets and depth tests, textures' four channels, fence values. Not yet checked: the title's 3D scenes |
| Menus and input | 🟡 a local user (`--user local`, signed in to no service) gets the title past its account prompt, with game saves on the host; scripted input drives it without a person (`HOLY_INPUT_SCRIPT`); past the start screen the title loads and shows its first-run settings screens, running for minutes. Fixed since: the texture layout library reported levels in the desktop's order (level 0 first) where the console's tiled layout keeps them smallest first, and titles find levels in their archives at those offsets (one read level 1 from the middle of level 0's compressed chunks and stopped at its own check); a GPU memory copy titles lock textures for the CPU with; texel addresses. An intermittent crash at start-up and during loading is gone (the texture layout library's computers were COM objects in the desktop's slot order; titles release them through slot 1 and read a layout through slot 2, which freed them and left garbage pitches): 6 of 6 runs clean, where 3 of 4 crashed. Text is whole: placed textures the GPU does not write follow what the title writes into their memory (glyphs rasterised on the CPU were missing). Pipelines the host refused (the console's rectangle topology type, input layouts under the translation's root signature) no longer drop draws. The intro video plays (its frame planes are written at a CPU address of the same physical pages the textures are placed at; the textures now follow every address of their pages). Fixed since: a crash in the intro's video decoder (a write to a page a texture watch had left read-only); the brightness screen's pipeline (an empty geometry subobject and a depth test with no depth format, both refused by the host). The intro's sound plays whole, and the video at its speed (measured with `--record-audio`: it was a third silence in 30 gaps a second; the title's audio engine sleeps between 10.67 ms mixing steps and woke only every 15.6 ms, the host's default timer resolution, now 1 ms for the title's process as on the console). Since: scripted input gets through the legal, terms and policy screens (scroll, tick, confirm) and the title starts loading; it stopped at its own archive check (a 3D texture sized as the host lays it out, half the console's thick 4 KiB layout: fixed), and now ends 20-40 s into loading, in its texture streaming (open: measured in 8 runs; the title's own checks find a 64 KiB streaming destination about 50 GB past the pool it belongs in, or a streaming request with a zero count; once the host device was removed by an invalid call; the host's debug layer reports nothing before). Fixed on the way: zlib requests that failed when inflated in place, the 6,000 refused attempts to open GPU heaps over read-only memory during loading; found: texture tiles arrive in the console's texture compression format, which has no public description (those requests now fail loudly instead of delivering compressed bytes as texels). Open: focused menu items lose their fill: the menus draw their shapes front to back under a stencil test that needs the stencil cleared every frame, and nothing the title calls clears it (found with stencil dumps and pipeline descriptions; with the stencil cleared the shapes come back, now in their colours: typed buffer views read zero before, fixed); the brightness screen's symbol is missing; the intro video is too bright (blacks at 42-53 of 255): the pass putting it on screen gets its texels without the sRGB decoding its view asks for and encodes them again; the policy screens' checkboxes do not tick with scripted input |
| Sound | ⬜ silent audio device |
| In game at a stable 30 FPS, no leaks | ⬜ |

| Area | Progress |
| --- | --- |
| Loader, sandbox, runtime, import binding | `██████████` 95% |
| Title memory (`xmem`, commit on demand) | `████████░░` 80% |
| Fast storage | `███████░░░` 70% |
| Game runtime services | `███████░░░` 68% (a local user, game saves on the host, the store offline) |
| Graphics: device, resources, command recording | `██████░░░░` 62% (committed buffers the CPU reaches in title memory; buffer, texture and resource copies; released memory let go of once the GPU is done; one memory map for translated programs; heaps released with title memory; root arguments in memory the GPU caches, owned by the recordings that use them) |
| Graphics: shader translation | `██████░░░░` 58% (every one of the reference title's 19,305 saved programs translates, ray intersection included (checked against hand-computed results, not yet against the console's); translations 45% smaller and laid out in program order, so the host compiles them in a third of the time: measured) |
| Graphics: pipelines and caches | `███████▌░░` 75% (translations cached on disk compressed, 8.7 times smaller; pipeline creation reads only their headers, the bytes are read when compiling: about 11 s of processor time less at every start; host pipelines compiled at first use or, when an earlier run used them, in the background) |
| Graphics: correct output | `███▊░░░░░░` 38% (typed buffer views read with their format (menu shapes were black); pipelines with an empty geometry subobject or a depth test without a depth format compile; texture tiling reported in the console's layout, mip tail first; rectangle lists drawn whole whichever corner the sides meet at; views and copies kept inside their resources; fence values belong to their fence; textures read with all four channels; texture copies follow the title's writes; native pipelines get their targets, depth tests and vertices right) |
| Graphics: performance | `████░░░░░░` 40% (host memory queries no longer stall the title: `VirtualQuery` takes up to 10 ms on large committed memory and was asked per timestamp, buffer and reached address (a loading thread spent up to 89 s of 100 in the kernel, 0.4 frames a second), now the allocation record answers in 0.15 us; a growing allocation re-opened its GPU heap with a quadratic walk (1.3-3 s a frame), now 15 ms with room to grow; host shader compile time, the first run's main cost, 3 times lower: 166 s to 56 s for 60 compute programs, measured without the driver's cache answering; page table and root arguments in memory the GPU caches; aligned reads as one load; memory accesses: two page-table reads instead of a scan; no views written per draw; recording a draw of native programs costs 0.3 us of CPU time, 0.4 us with a pipeline and table change each draw, measured over 20,000 draws; translated memory reads now cost the same as the desktop's on the same memory (1.0 times, was 1.5), and the GPU reads title memory over the bus without its second-level cache, about 20 times slower than video memory: measured; copies in video memory of what translated programs read, opt-in for now, make such reads 3 times faster) |
| Input | `█████░░░░░` 45% (scripted input for unattended runs) |
| Audio | `██░░░░░░░░` 18% (the title's own engine plays whole through the host: video sound continuous, measured; the console's audio processor, which decodes and mixes the rest, is still silent) |
| Measurement: statistics, profiler, leak checks, call check, frame trace | `█████████░` 89% (frame traces over several presents; pipelines described (blend, depth, stencil, rasterizer) with their host programs saved for the disassembler, and console render state blocks logged; stencil planes in texture dumps, and which resources share a depth target's memory; audio recorded from the title's process and reported without listening (`--record-audio`, tools/audio_report); draws shown as checkerboards by pixel program, and refused pipelines drawn as checkerboards instead of dropped; texture dumps tell empty title memory from stale host copies; a run report turns a run's log into frames a second, frame times, memory growth and leak suspects, with exit codes; one command runs and checks a title; offline host compile times of every kind of translated program, without the driver's cache answering) |

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
| `gameinput` (input) | One gamepad from the host's first XInput controller and the keyboard: readings, device enumeration and information, rumble |
| Offline switch, interposers for DLLs the package ships | Working, tested end to end |
| `xg_xs` (texture layout) | Swizzle mode suggestion, texture copies into a desktop-compatible layout, tiling queries |
| Audio console libraries (`AcpHal`, `xapu`, `chad`) | Audio processor heap; shape contexts, control and audio processors as a silent device (nothing is decoded, mixed or played) |
| Measurement | Statistics report in the runtime log (memory, live objects, frame rate, GPU memory) every ten seconds; `holy run --profile` sampling profiler |

What this means in practice: a retail title image loads, runs its static initialisation, initialises the
game runtime, creates its window, graphics device and over 80,000 pipelines (shaders translated from native
GPU code), streams its start-up data through fast storage, and presents frames onto a swap chain on its window
at 30 a second from 3 seconds after start, with about 5 GiB of commit. Its frames are still black, and it
loads for about a minute and a half before the development host runs out of commit (see
[docs/ROADMAP.md](docs/ROADMAP.md)). No
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
