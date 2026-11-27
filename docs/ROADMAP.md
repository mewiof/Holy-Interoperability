# Roadmap

The order below is the order in which a title needs things during start-up, which is also the order in which
`holy run` reports them missing.

Legend: **done** — implemented and tested; **partial** — some of it works; **next** — the current frontier;
**open** — not started.

## 0. Foundation — done

- Package manifest and PE readers, hardened against malformed input.
- Launcher: in-place suspended start, in-memory import takeover, low-integrity sandbox, job object, timeout.
- Runtime: import binder with provider policy, reporting thunks, call tracing, crash reports.
- Static inspector with a console-library coverage figure.
- Self-authored sample title and end-to-end tests.

## 1. Process environment — partial

- done: package mount, read-only package, temp and scratch volumes.
- done: console identity (model reported to the title).
- done: lifecycle notification registration.
- open: processor topology. Titles assume the console's core and thread count and set affinities
  accordingly; hosts with fewer logical processors need masks clamped and counts reported consistently.
- partial: path interposition below the import table. DLLs the package ships now get the same interposers
  as the title image; code that reaches host functions in other ways (dynamic lookups of host exports) still
  sees host paths, and the sandbox still protects the package.
- done: the offline switch (default): the process sees a disconnected host network.

## 2. Memory (`xmem`) — partial

- done: `XMemAlloc` / `XMemFree` and the default-allocator pair.
- done: `XMemVirtualAlloc` with console page-size and graphics-protection flags.
- done: commit on demand when the host's commit charge cannot cover a title's up-front pools.
- done: physical page allocation and mapping (`XMemAllocatePhysicalPages`, `XMemMapPhysicalPages`,
  `XMemMapPhysicalPagesScatter`, `XMemFreePhysicalPages`, `XMemTransferMemory`): section-backed pages mapped
  into placeholder reservations, giving the aliased CPU and GPU views titles build.
- open: `XMemVirtualQuery`, `XMemGetWorkingSetStatistics`, `XMemIsToolsMemAvailable`.
- done: honouring a title's exported allocation hooks.
- done: lazy storage for mapped physical pages (commit follows use; texture pools of several GiB map on hosts
  with less spare commit).
- done: page numbers are 4 KiB frame numbers (multiples of 16), as titles that store them shifted right by four
  require; freed pages keep their storage while still mapped.

## 3. Game runtime (`xgameruntime`) — partial

A title's statically linked client library loads the runtime and looks up `InitializeApiImpl` (plus `Ex` and
`Ex2` variants), `QueryApiImpl`, `UninitializeApiImpl` and `XErrorReport`. Everything else is reached through
provider interfaces obtained from `QueryApiImpl`.

- done: entry points and the interface-query protocol; unknown interfaces and unbound slots report
  themselves by interface id and slot.
- done: task queues, async blocks, the time-sensitive thread assertion.
- done (the functions titles have been seen to call): connectivity hint, user add (nobody signed in) and
  change events, system analytics info and device identity, title id, package identity and installation
  state, persistent local storage, HDR mode, speech synthesis (the host's speech engine; it refuses to speak in
  the low-integrity sandbox, so synthesis there returns the host's error).
- open: the rest of the user service, game saves, store queries.
- Policy for services with no local equivalent: the runtime does not sign users in to anything, does not
  emulate online services, and never reports ownership of add-on content.

## 4. Graphics (`d3d12_xs`, `xg_xs`) — partial

The largest piece.

- done: `D3D12ConfigureVirtualMemoryX` (accepted and recorded), root-signature helpers (forwarded).
- partial: `D3D12XboxCreateDevice` on the host's Direct3D 12; command queues, fences, allocators, command-list
  creation and `Close`; console DXGI display modes; frame interval and frame events; descriptor heaps placed by
  the title; views; root signatures; committed and placed resources (buffers placed in title memory through
  host heaps over that memory); compute pipelines from shaders that keep their DXIL, and from native code.
- partial: shader translation from native GPU code (titles' own shaders carry nothing else): compute, vertex
  and pixel programs are translated straight into host bytecode (DXBC) without a shader compiler, covering
  scalar and vector ALU, scalar, buffer, global and group-shared memory, atomics, exports and interpolation.
  Open: cross-lane operations beyond quads in pixel programs, hull/domain/geometry stages, and checking
  translated code against known results (unit tests run chosen instructions on the host GPU).
- done: query heaps, command signatures, desktop-order committed resources, console samplers (field meaning
  partly assumed), heaps over title memory, pipeline state streams (desktop or translated programs).
- partial: `xg_xs` texture layout: swizzle mode suggestion and the texture computer's copy into the layout it
  reports (desktop copyable footprints); texture views at title addresses through host copies.
- done: compute pipelines from native code given directly (slot 67), native graphics pipelines (slot 57), derived
  pipelines (slot 55), image instructions, gathers, the unified typed buffer formats, cross-lane operations.
- partial: command recording: state, root arguments, draws and dispatches, barriers, clears, GPU-written values and
  timestamps; translated pipelines get their root arguments as native code expects them, and reach title memory
  through one memory map (memory slots and a page table) for every call. Not yet checked against known results;
  memory reached only through pointers in memory is covered once something has located it.
- partial: presentation: frame tokens, `PresentX` onto a swap chain on the title's window, captured frames.
- done: queue-ordered page mapping updates (queue slot 31) for texture pools, applied when called.
- done: native pipeline descriptions with sparse stage slots, `buffer_load_format_*` with the descriptor's format
  and selects, pixel-program quad permutes through fine derivatives (bit-exact, checked on the host GPU),
  DPP source-1 modifiers, `s_bfm_b64`; constant buffer views at unaligned addresses (null on the host).
- done: the console's rectangle-list topology no longer reaches the host (it removed the host device as hung a
  second into the reference title's first frames); command lists resubmitted while executing get pooled host
  lists.
- done: rectangle lists, through a generated geometry program that adds each rectangle's fourth corner (NGG
  pipelines still draw half of each rectangle).
- done: NGG vertex programs (a compute pass writing vertex and primitive records, then an indexed draw of them),
  half-precision interpolation after them, image instructions with 16-bit data, cube, multisampled,
  explicit-gradient and unnormalised images, packed 16-bit and sub-dword ALU operations, lane permutes through
  group-memory instructions, program data read through the program counter.
- done: translations cached on disk across runs and host pipelines compiled on worker threads; presents paced
  at the title's frame interval; console-only DXGI formats; the texture computer's tiling query.
- done: whole-wave reductions and other cross-lane operations in pixel and vertex programs (the quad as the
  wave), lane-mask counting consistent with one-lane waves, image atomics, subroutine calls, data after the end
  of programs, half-precision attributes after plain vertex programs.
- next: amplification and mesh pipelines in pipeline streams; NGG pipelines with indexed draws; integer views of
  images for plain loads and stores (they are declared as float).
- done: the crash of the reference title some seconds after its first frame (a resource whose `GetDesc` was
  called in the other argument order, which overwrote the resource object); host heaps over physical pages no
  longer leak system commit after the process ends.
- done: packed 16-bit render targets and depth textures that ask for unordered access are made as the host
  supports them (the reference title stopped at its own checks on both).
- done: a page table translated programs consult (title page to memory slot and offset) replaces the per-call
  region table: no scan per memory access, no views written per call, no limit of 64 pieces of memory, and memory
  located once stays reachable from every call. Unit tests cover the slot arithmetic and the lookup on the GPU.
- next: placed resources over physical pages that are not contiguous in one section (the reference title stops
  at its own check when one cannot be placed; seen with the host's commit exhausted, when sections can only be
  reserved). Tiled host buffers over the pages are not possible (the host refuses tile mappings into heaps over
  existing memory, measured); translated programs already reach such pages one by one through the page table.
- done: image loads, samples, gathers and size queries in translated programs return all four components (only
  the first was written: textures read as their red channel). The sample title now runs a native compute program
  through the console interfaces end to end (a texture view in title memory, root tables, the memory map) and
  checks the texel it stores.
- done: host copies of textures in title memory follow later writes: xmem reserves title memory with write
  watching (page protection cannot be used: measured, the host refuses protection changes on memory a heap was
  opened over), and textures over written pages, remapped pages and released memory are copied again before the
  next submission. The sample title checks a texel written after its view (and fails without the tracking).
- done: writes to physical pages through the title's views, watched by page protection of those views (the host
  allows it there, measured; the GPU's own views of the pages carry the heaps). The sample title checks a texel
  written through its view of a physical page.
- done: native pipelines' render targets from their format list (dword 3 is not a count: a quarter of a title's
  pipelines had no target or only the first), the second layout of their render state (477 pipelines read at the
  wrong offsets: target 0 never written), and their depth state (variants by depth target format). Draws without
  indices pass their first vertex in s2 (they fetched from the buffer's start). The sample title checks a draw from
  vertex 3 and a depth test with native programs and descriptions; each check fails without its fix.
- done: the stencil state of native pipelines (operations, functions, masks); the sample title checks it.
- next: GPU writes into title memory under textures (copies and translated programs writing texture memory).
- done: GPU heaps over title memory are released before the title frees the memory (they kept it charged:
  measured 1 GiB over 16 rounds of 64 MiB); an end-to-end check measures it.
- done: the page table follows the title's physical page mappings (xmem reports pages getting storage and being
  unmapped; a heap opened over section storage enters every title address showing it).
- next: open heaps for physical storage the title maps before anything locates it, within the commit the host
  allows (memory reached only through pointers into such storage still reads as zero).
- done: native pipelines with a program that neither exports nor interpolates (depth-only passes) and with an
  unusual target field; placeholder pipelines in the reference title's first minute went from 426 to 66.
- done: unordered-access views of depth textures, through a twin texture kept in step at barriers.
- done: ray intersection image instructions (`image_bvh_intersect_ray`, `image_bvh64_intersect_ray`) in compute
  programs: box nodes of floats and of half floats, triangle nodes, sorted or not, A16 directions (unit tests with
  hand-computed results). Packages ship the builder that writes acceleration structures in the hardware's layout,
  so no builder is needed here. Partial: results not yet compared with the console's on the same nodes.
- done: scalar waits and mode writes without a register (`s_waitcnt_vscnt null`, `s_setreg_imm32_b32`)
  translate; programs using them were refused.
- next: textures streamed into scattered pool pages are copied through the CPU.
- open: textures the GPU writes in title memory as seen by the CPU.
- partial: buffers in video memory. The GPU reads title memory over the bus without its second-level cache,
  about 20 times slower than video memory once data misses the first-level caches (measured, `PLATFORM_NOTES.md`).
  Copies in video memory of pages translated programs read through bound buffers (`page_cache.h`), kept current
  with the CPU's writes, out of the cache once the GPU writes them; opt-in (`HOLY_BUFFER_COPIES`) until checked on
  the reference title. Next: the translated memory access path (the copies make its own cost the larger part).
- open: console extensions: presentation and frame pacing, title-managed graphics memory and placed
  resources at title-chosen addresses, console-specific command list and descriptor behaviour.
- open: interaction with graphics helper DLLs that packages ship and that call into the device.

## 5. Audio (`acphal`, `xapu`, `chad`) — partial

Hardware audio processing interfaces. Plan: software implementations of the processing the title requests,
output through the host's audio stack.

- done: the audio processor heap (`ApuCreateHeap`, `ApuAlloc`, `ApuFree`, address mapping and validity).
- partial: shape contexts, the control processor and both audio processors' connections and command queues, as a
  silent device: titles start audio, nothing is decoded, mixed or played.
- open: carrying out the processors' commands. They include decoding the console's compressed audio format, which
  needs either an original decoder or a third-party one (a decision for the maintainer). Titles drive the
  processors themselves (no middleware, no host audio interface): `HOLY_AUDIO_TRACE` records the command
  streams and the records commands name, the material for establishing the command formats.

## 6. Input (`gameinput`) — partial

The console flavour of the input API, built under an alias because the host has a different DLL of the same
name.

- done: `GameInputCreate` and the `IGameInput` methods titles call first.
- done: one gamepad from the host's first XInput controller and the keyboard (readings, device enumeration,
  device information, rumble).
- open: reading callbacks, several gamepads, keyboard and mouse readings.

## 7. Storage (`xcrdapi`) — partial

The console's fast-storage factory. Plan: implement on host file I/O first, the host's equivalent API later.

- done: factory, `OpenFile`, staging size, status arrays, queues, file and memory requests, zlib requests.
- done: a table-driven inflater that writes straight into the destination.
- open: asynchronous processing (requests are carried out one at a time when enqueued), other request kinds.

## Cross-cutting

- done: console processor instructions the host lacks (`CLZERO`, `MONITORX`, `MWAITX`) carried out by the runtime.
- done: process exits the title starts are logged with their stack.
- done: measurement: named counters and a periodic statistics report in the runtime log (memory, live objects,
  frame rate, GPU memory); a sampling profiler in the launcher (`holy run --profile`).
- done: pipelines compiled at first use (and early for pipelines used in earlier runs), shared by content,
  programs reloaded from the translation cache instead of kept in memory: the reference title's commit at its
  first frames went from about 17 GiB (exhausting the host) to about 5 GiB.
- done: commit on demand remembers what it committed: the reference title's first frame comes after 3 seconds
  instead of 30.

- A way to run more than the default executable of a package, and to pass a launch context.
- Symbol-free minidumps next to the runtime log.
- done: host caches (graphics driver and runtime shader caches) of the title process live in its data
  directory and are discarded after a run in which the host refused a pipeline.
- CI: build and test on every push (workflow present, not yet exercised on the hosting service).
- Other host operating systems, by way of a Windows API layer beneath the project.
