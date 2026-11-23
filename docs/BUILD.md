# Building

## Requirements

- Windows 10 or 11, x64
- CMake 3.25 or newer
- The Windows SDK and the MSVC libraries (installed with Visual Studio 2022 or newer, "Desktop development
  with C++")
- One of:
  - LLVM (`clang-cl`) and Ninja — works from any shell, including Git Bash
  - Visual Studio's own compiler, through the Visual Studio generator or Ninja in a developer prompt

There are no third-party source dependencies.

Tested configurations: clang-cl 22 with Ninja, and Visual Studio 2026 (MSVC 14.51) with the Visual Studio
generator. Older toolchains that support C++20 should work but have not been tried.

## Presets

| Preset | Generator | Compiler | Notes |
| --- | --- | --- | --- |
| `clang-cl` | Ninja | clang-cl | default for development; no developer prompt needed |
| `msvc` | Ninja | cl | run from an "x64 Native Tools" prompt |
| `vs` | Visual Studio | cl | what CI uses; multi-config |

```sh
cmake --preset clang-cl
cmake --build --preset clang-cl
ctest --preset clang-cl
```

Output lands in `build/<preset>/bin` (`build/vs/bin/<config>` for the Visual Studio generator):

```
bin/holy.exe                 command line front end
bin/holyrt.dll               runtime loaded into title processes
bin/libs/                    console library implementations
bin/holy_unit_tests.exe      unit tests
bin/sample_title/            the repository's own sample title package
```

Keep `holy.exe`, `holyrt.dll` and `libs/` together; the launcher looks for the other two next to itself.

## Tests

`ctest` runs:

- `unit` — parsers, path model, provider policy, string helpers, including malformed and hostile input.
- `e2e.inspect` — static import report for the sample title.
- `e2e.run_sandboxed`, `e2e.run_unsandboxed` — the sample title started through the loader. It checks, from
  inside, static TLS, C++ exceptions, console memory allocation, the package mount, package write denial,
  scratch storage, console library loading, lifecycle registration, console identity and (sandboxed) that the
  OS itself refuses writes to the package.
- `e2e.unimplemented_traps`, `e2e.unimplemented_continues` — both policies for unimplemented imports.

The end-to-end tests need the Visual C++ redistributable runtime on the machine (it is there wherever Visual
Studio is), because the sample title links the DLL C runtime the way a title does.

## Options

| CMake option | Default | Meaning |
| --- | --- | --- |
| `HOLY_BUILD_TESTS` | `ON` | build unit tests and the sample title |
| `HOLY_WARNINGS_AS_ERRORS` | `OFF` | `/WX` |

## Editor support

`.clangd` points at `build/clang-cl/compile_commands.json`, so configure the `clang-cl` preset once and
clangd-based editors resolve includes and flags.

## Repository hygiene

```sh
python tools/hygiene/check.py
```

Checks the working tree for files that must never be committed. See [CONTRIBUTING.md](../CONTRIBUTING.md).
