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
- open: honouring a title's exported allocation hooks.

## 3. Game runtime (`xgameruntime`) — partial

A title's statically linked client library loads the runtime and looks up `InitializeApiImpl` (plus `Ex` and
`Ex2` variants), `QueryApiImpl`, `UninitializeApiImpl` and `XErrorReport`. Everything else is reached through
provider interfaces obtained from `QueryApiImpl`.

- done: entry points and the interface-query protocol; unknown interfaces and unbound slots report
  themselves by interface id and slot.
- done: task queues, async blocks, the time-sensitive thread assertion.
- done (the functions titles have been seen to call): connectivity hint, user add (nobody signed in) and
  change events, system analytics info and device identity, title id, package identity and installation
  state, persistent local storage, HDR mode.
- open: the rest of the user service, game saves, store queries.
- Policy for services with no local equivalent: the runtime does not sign users in to anything, does not
  emulate online services, and never reports ownership of add-on content.

## 4. Graphics (`d3d12_xs`, `xg_xs`) — open

The largest piece.

- done: `D3D12ConfigureVirtualMemoryX` (accepted and recorded), root-signature helpers (forwarded).
- open: `D3D12XboxCreateDevice` and the object model behind it, translated onto the host's Direct3D 12.
- open: console extensions: presentation and frame pacing, title-managed graphics memory and placed
  resources at title-chosen addresses, console-specific command list and descriptor behaviour.
- open: shader translation. Titles ship only native console GPU machine code (see
  [PLATFORM_NOTES.md](PLATFORM_NOTES.md)); nothing portable is included, so every shader must be translated
  from that machine code to a host shader format.
- open: `xg_xs` texture layout computation.
- open: interaction with graphics helper DLLs that packages ship and that call into the device.

## 5. Audio (`acphal`, `xapu`, `chad`) — open

Hardware audio processing interfaces. Plan: software implementations of the processing the title requests,
output through the host's audio stack.

## 6. Input (`gameinput`) — partial

The console flavour of the input API, built under an alias because the host has a different DLL of the same
name.

- done: `GameInputCreate` and the `IGameInput` methods titles call first, answering as a console with no
  input device attached.
- open: surfacing host controllers (and keyboard and mouse) as devices and readings.

## 7. Storage (`xcrdapi`) — partial

The console's fast-storage factory. Plan: implement on host file I/O first, the host's equivalent API later.

- done: factory, `OpenFile`, staging size.
- open: queues, requests (including the console's decompression formats), status arrays.

## Cross-cutting

- A way to run more than the default executable of a package, and to pass a launch context.
- Symbol-free minidumps next to the runtime log.
- CI: build and test on every push (workflow present, not yet exercised on the hosting service).
- Other host operating systems, by way of a Windows API layer beneath the project.
