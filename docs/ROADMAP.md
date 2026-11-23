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
- open: path interposition below the import table (code that reaches the host file API without going through
  the title's imports currently sees host paths; the sandbox still protects the package).
- open: an offline switch that fails network initialisation cleanly.

## 2. Memory (`xmem`) — partial

- done: `XMemAlloc` / `XMemFree` and the default-allocator pair.
- done: `XMemVirtualAlloc` with console page-size and graphics-protection flags.
- open: physical page allocation and mapping (`XMemAllocatePhysicalPages`, `XMemMapPhysicalPages`,
  `XMemMapPhysicalPagesScatter`, `XMemFreePhysicalPages`, `XMemTransferMemory`). Intended design: a
  section-backed pool of "physical" pages mapped into placeholder reservations, which also gives the aliased
  CPU and GPU views titles build.
- open: `XMemVirtualQuery`, `XMemGetWorkingSetStatistics`, `XMemIsToolsMemAvailable`.
- open: honouring a title's exported allocation hooks.

## 3. Game runtime (`xgameruntime`) — next

A title's statically linked client library loads the runtime and looks up `InitializeApiImpl` (plus `Ex` and
`Ex2` variants), `QueryApiImpl`, `UninitializeApiImpl` and `XErrorReport`. Everything else is reached through
provider interfaces obtained from `QueryApiImpl`.

- next: establish the calling contract of the entry points and the interface-query protocol.
- open: provider interfaces, roughly in start-up order: task queues and async blocks, system information,
  package and installation queries, persistent local storage, users, game saves, lifecycle, input-related
  services, store queries.
- Policy for services with no local equivalent: the runtime does not sign users in to anything, does not
  emulate online services, and never reports ownership of add-on content.

## 4. Graphics (`d3d12_xs`, `xg_xs`) — open

The largest piece.

- done: `D3D12ConfigureVirtualMemoryX` (accepted and recorded), root-signature helpers (forwarded).
- open: `D3D12XboxCreateDevice` and the object model behind it, translated onto the host's Direct3D 12.
- open: console extensions: presentation and frame pacing, title-managed graphics memory and placed
  resources at title-chosen addresses, console-specific command list and descriptor behaviour.
- open: shader formats: determine what titles ship and translate where the host cannot consume it directly.
- open: `xg_xs` texture layout computation.
- open: interaction with graphics helper DLLs that packages ship and that call into the device.

## 5. Audio (`acphal`, `xapu`, `chad`) — open

Hardware audio processing interfaces. Plan: software implementations of the processing the title requests,
output through the host's audio stack.

## 6. Input (`gameinput`) — open

The console flavour of the input API, built under an alias because the host has a different DLL of the same
name. Plan: implement on top of the host's controller APIs.

## 7. Storage (`xcrdapi`) — open

The console's fast-storage factory. Plan: implement on host file I/O first, the host's equivalent API later.

## Cross-cutting

- A way to run more than the default executable of a package, and to pass a launch context.
- Symbol-free minidumps next to the runtime log.
- CI: build and test on every push (workflow present, not yet exercised on the hosting service).
- Other host operating systems, by way of a Windows API layer beneath the project.
