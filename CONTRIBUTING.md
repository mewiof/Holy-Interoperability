# Contributing

Thanks for helping. This project lives or dies by a few rules about what may enter the repository, so those
come first.

## Content rules

These apply to code, comments, documentation, tests, commit messages, issues, pull requests and CI output.

1. **Never name a title.** No game names, publisher or studio names, title ids, store ids, package identity
   names, title-specific executable or archive names, and no file paths that contain any of them. Describe a
   problem by the platform API involved ("the title imports `X!Y`, which returns ..."), never by which title
   hit it.
2. **Never add a file the project does not own.** No title binaries or data, no console system libraries, no
   firmware or OS images, no keys, no SDK headers, import libraries or documentation copied from an SDK.
3. **Clean-room sources only.** A console API may be declared and implemented from:
   - public documentation and public headers of the *desktop* platform SDK (as a build dependency, never
     copied into the tree);
   - the names a title's import table lists;
   - the observable behaviour of calls a title makes (arguments, expected results), established with this
     project's own tracing or by inspecting call sites.

   It may not be derived from console SDK headers, console system binaries, or leaked material. If you have
   had access to such material under an agreement, do not contribute to the areas it covers.
4. **No protection circumvention.** No decryption or unpacking of package containers, no key handling, no
   bypassing of anti-tamper or licence checks. The project only ever sees packages that are already in the
   form a title sees at run time, and it never grants entitlements a user does not have.
5. **Test fixtures are self-authored.** The repository carries its own sample title
   ([tests/sample_title](tests/sample_title)). Tests must not need any third-party binary.
6. **Logs are local.** Runtime logs redact the package location, but still describe a specific title's
   behaviour. Quote only the lines that describe a platform API, and never commit log files.

`python tools/hygiene/check.py` enforces what can be enforced mechanically: no binary or foreign files, no
user-profile paths, and no term from your private deny-list (`local/denylist.txt`, git-ignored; put the names
of whatever you test with there so they can never slip into a commit). Run it before every commit. CI runs it
too.

## Working on the code

- Build and test: see [docs/BUILD.md](docs/BUILD.md). All tests must pass; add a test with every behaviour
  you add. The sample title is the place for end-to-end checks of anything a title can observe.
- Design overview: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md). Where things are headed:
  [docs/ROADMAP.md](docs/ROADMAP.md). What is known about each platform contract and how it was established:
  [docs/PLATFORM_NOTES.md](docs/PLATFORM_NOTES.md) — update it when you establish something new.
- Style: C++20, `clang-format` with the repository's `.clang-format`, warnings clean at `/W4` on both
  `clang-cl` and MSVC.
- Treat everything read from a package as untrusted input: bounds-check, cap iteration, never use a name from
  an image as a path.
- Never write into a package directory, in code or in tests. The sandbox and the path layer both exist to
  guarantee this; do not weaken either.

## Implementing a console library function

1. Run `holy inspect` and `holy run` on your own package to see which import the title reaches first.
2. Establish the contract (rule 3). Use `--trace` and `--unimplemented continue` to observe calls.
3. Implement it in the library's directory under [core/libs](core/libs), with a comment stating the contract
   and how it was established ("documented", "observed", "assumed").
4. If only the symbol's presence is needed so that a packaged DLL loads, use `HOLY_UNIMPLEMENTED_EXPORT`; it
   keeps the symbol out of the coverage figure.
5. Add the behaviour to the sample title and record the contract in `docs/PLATFORM_NOTES.md`.

## Licensing

Contributions are accepted under the repository's [MIT license](LICENSE). Only submit work you wrote and are
entitled to contribute.
