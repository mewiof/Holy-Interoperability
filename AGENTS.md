# Instructions for coding agents

This file is for AI agents (and humans who like checklists) working in this repository. The rules in
[CONTRIBUTING.md](CONTRIBUTING.md) apply in full; the points below are the ones agents most often get wrong.

## Hard rules

- **Never name a title anywhere in the repository**: not in code, comments, docs, tests, commit messages or
  branch names. That includes publisher names, ids, title-specific file names, and local paths containing
  them. When you learn something from running a title, record the *platform contract*, not the title.
- **Never add files the project does not own**, and never copy text or declarations from a console SDK.
- **A title package is read-only.** Never create, modify or delete anything inside a package directory, and
  never run a tool with a package directory as its output or scratch location. Keep inspection output, logs
  and notes outside the repository or under `local/` (git-ignored).
- **Do not weaken the sandbox or the package write-denial** to get a title further.
- **No protection circumvention**: no decryption, key handling, licence or anti-tamper bypasses, and no
  entitlement granting.
- Run `python tools/hygiene/check.py` before proposing a commit, and keep `local/denylist.txt` populated with
  the names of whatever you are testing with.

## Build and test

```sh
cmake --preset clang-cl          # once
cmake --build --preset clang-cl
ctest --preset clang-cl
```

`clang-cl` works from any shell. Also check MSVC before finishing a change: `cmake --preset vs`,
`cmake --build --preset vs`, `ctest --preset vs`.

## The bring-up loop

1. `holy inspect <package>` — static view: which imports are unresolved, console library coverage.
2. `holy run <package> --timeout 60` — stops at the first call into an unimplemented import and prints the
   library, symbol, caller offset and argument registers.
3. `holy run <package> --unimplemented continue --trace <libs> --log-level debug --timeout 60` — exploration:
   unimplemented calls return zero so later calls become visible. Results after the first such call are not
   trustworthy as behaviour, only as a list of what the title asks for.
4. Establish the contract, implement it in `core/libs/<library>`, add a check to
   `tests/sample_title/main.cpp`, document it in `docs/PLATFORM_NOTES.md`.

The runtime log is written to the title's data directory (`holy run` prints the path when a title fails).

## Conventions

- New console library: a directory under `core/libs`, declared with `holy_add_system_library`. If its file
  name collides with a host system DLL, add an alias in `core/common/provider.cpp` and never place a DLL with
  the colliding name in `libs/`.
- New interposed host function: add it to the table in `core/runtime/hooks.cpp`. Interposers must preserve
  the caller-visible last-error value.
- Anything a title can observe gets an end-to-end check in the sample title; pure logic gets a unit test.
- State what was verified and what was not. An unimplemented function must fail loudly (reporting thunk or
  `HOLY_UNIMPLEMENTED_EXPORT`), never return a plausible-looking success.
