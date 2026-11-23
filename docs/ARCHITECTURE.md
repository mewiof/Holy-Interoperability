# Architecture

## The problem

A title built for the current console generation is a PE32+ x64 executable. Its code is native to any modern
x64 PC, and most of what it imports are ordinary Windows API sets and C runtime functions. What stops it from
starting on a desktop is narrow and specific:

1. It statically imports **console-only system libraries** (graphics, memory, audio hardware, input, storage)
   that do not exist on a desktop, so the OS loader rejects the image before a single instruction runs.
2. It loads the console **game runtime** by name, and a desktop may carry an unrelated DLL of the same name.
3. It assumes the **console environment**: the package mounted read-only on a fixed drive, scratch volumes,
   a console model to identify, lifecycle notifications, fixed hardware.

So the job is not emulation. It is (a) getting the image loaded with its imports satisfied, (b) reimplementing
the console-only libraries on top of host APIs, and (c) presenting the environment a title expects.

## Components

```
tools/holy            command line front end (inspect, run)
core/common           logging, strings, paths, boot block, provider policy, path model, XML reader
core/pe               bounds-checked PE32+ reader (file and mapped layouts), read-only file mapping
core/package          package manifest reader
core/analysis         static prediction of how an image's imports will bind
core/loader           process creation, sandbox, in-memory import takeover
core/runtime          holyrt.dll: boot, import binder, thunks, interposers, crash reports
core/libs/<name>      one DLL per reimplemented console system library
tests/unit            unit tests
tests/sample_title    self-authored stand-in for a console title (end-to-end tests)
```

## Launch sequence

1. **Read the manifest.** `core/package` reads `MicrosoftGame.config` from the package root and picks the
   retail console executable.
2. **Create the process suspended**, from the executable where it lies, with the package root as current
   directory. With the sandbox on (default) the process gets a duplicate of the launcher's token lowered to
   low integrity. The process joins a job object so it cannot outlive the launcher.
3. **Take over the imports.** Before the OS loader has processed the image, the launcher writes a small block
   into the new process just above the image and points the image's import directory at it. The block
   contains a *boot block* (where the original import directory was, package and data paths, policy flags)
   followed by a one-entry import table naming `holyrt.dll`. See
   [core/common/boot_block.h](../core/common/boot_block.h).
4. **Resume.** The OS loader maps and initialises the image exactly as it would any process — relocations,
   static TLS, exception tables, security cookie — and finds only one import: the runtime.
5. **Boot the runtime.** In its `DllMain`, `holyrt.dll` finds the boot block, opens its log, installs crash
   reporting, binds the title's original import directory itself, then restores the image headers.
6. **The title runs.** TLS callbacks and the entry point execute normally.

Why this shape, rather than mapping the image manually or rewriting it into a new file:

- The OS does everything the OS is good at. Static TLS, unwinding, debugger support and crash dumps all work
  because the title is a normally loaded image.
- Nothing derived from the title is ever written to disk, and the package is not touched.
- The project controls import resolution completely, which is the only part that needs to differ.

## Import binding

For each imported library the binder asks the provider policy ([core/common/provider.h](../core/common/provider.h))
who serves it:

| Provider | Meaning | Resolution |
| --- | --- | --- |
| `runtime` | a console library implemented in `libs/` | loaded from `libs/`, symbols by export lookup |
| `package` | a DLL the title package ships | loaded from the package by the OS loader |
| `host` | a host system library | loaded from the system directory only |
| `missing` | a console-only library nobody implements yet | every symbol gets a reporting thunk |

Rules worth knowing:

- Import names come from an untrusted image and are never used as paths. Only bare file names are accepted.
- Console-only names never fall back to the host, even if the host has a DLL with that name. Where a console
  library's name collides with a host DLL, the implementation is built under an alias and never placed on the
  DLL search path under the colliding name.
- For symbols served by the host, the runtime may substitute an **interposer**
  ([core/runtime/hooks.cpp](../core/runtime/hooks.cpp)) where console behaviour differs: library loading,
  file paths, the console identity query, lifecycle registration, the top-level exception filter.
- A symbol nobody provides is bound to a **reporting thunk** ([core/runtime/thunks.cpp](../core/runtime/thunks.cpp)).
  When the title calls it, the thunk logs the library, symbol, caller and argument registers, then applies
  the policy: terminate (default) or return zero (`--unimplemented continue`, exploration only).
- The same thunk can sit in front of implemented imports to trace calls (`--trace`).
- DLLs shipped in a package are loaded by the OS and resolve *their* console imports through the DLL search
  path, which includes `libs/`. `HOLY_UNIMPLEMENTED_EXPORT` exists so such a DLL can load while a symbol it
  needs is still a placeholder; placeholders are marked and never count as implemented.

`holy inspect` runs the same policy statically ([core/analysis](../core/analysis)), parsing package and
runtime libraries without loading them, so its prediction matches what `holy run` will do.

## The title's environment

**Paths** ([core/common/vfs.h](../core/common/vfs.h)). The package is mounted on `G:`, temporary storage on
`T:` and scratch on `D:`; the latter two are directories under the per-title data directory. Relative paths
resolve against the package root, as on the console.

**The package is read-only, twice over.**

- The path layer refuses any operation that would create, modify or delete something under the package root.
  This is what a title sees on the console, where the package volume is read-only.
- The sandbox makes the operating system refuse the same writes for *all* code in the process — packaged
  DLLs, the C runtime, anything that bypasses the import table. The sample title verifies this by calling the
  host's file API directly.

**Data directory.** Per title, under the user's low-integrity application data folder so sandboxed titles can
write to it. The directory name is derived from ids in the manifest, not from the title's name. It holds
`logs/`, `temp/` and `scratch/`.

**Console identity.** Platform code linked into titles asks the registry which console model it runs on; the
runtime answers that single query (see [PLATFORM_NOTES.md](PLATFORM_NOTES.md)).

**Lifecycle.** Suspend/resume and constrained-mode registrations succeed; a desktop process is never
suspended by a lifecycle manager, so the callbacks do not fire.

**Crash reports.** Error-severity exceptions are logged first-chance (rate limited) and, if unhandled, with
registers and a module+offset stack trace, before the title's own filter runs.

**Logs** never spell out the package location (`<package>`, `<data>`) or the executable's name (`title+0x…`).

## Console libraries

Each library under `core/libs` is a normal DLL exporting the console API's names. Implementations state their
contract and how it was established (documented, observed, assumed). They link the C runtime statically and
talk to the runtime through a small C interface ([core/runtime/api.h](../core/runtime/api.h)).

The big ones, in the order a title needs them, are the game runtime, memory, graphics, audio, input and
storage. Their state and plan are in [ROADMAP.md](ROADMAP.md).

## Security posture

Title packages are untrusted input and untrusted code.

- All parsing (PE, manifest) is bounds-checked with capped iteration, and covered by tests with hostile input.
- Static inspection never loads or executes package code.
- Running a title is sandboxed by default: low integrity, job object, no write access to user files.
- The runtime does not weaken the process: no debug privileges, no changes outside the title process.

## Portability

The host is 64-bit Windows. Because titles are Windows executables that rely on Windows APIs, the natural
route to other operating systems is a Windows API layer underneath this project, not a second backend inside
it.
