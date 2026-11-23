# Platform notes

What is known about each console platform contract the project implements or is about to, and how it was
established. Nothing here comes from console SDK headers or console system binaries.

Each entry is marked:

- **documented** — stated in public documentation or public desktop SDK headers;
- **observed** — established from calls titles make (import names, arguments at the call site, how the
  result is checked);
- **assumed** — a working hypothesis that still needs confirmation.

Notes describe the platform, never a title.

## Image format and imports

- *observed*: title executables are PE32+ x64, relocatable, GUI subsystem, with static TLS and ordinary
  exception tables. They are not marked as app-container images, and a desktop OS will create a process from
  one if its imports can be satisfied.
- *observed*: titles import the C runtime through the universal CRT API sets and ship their own C++ runtime
  DLLs in the package.
- *observed*: window and message functions are imported through `ext-ms-win-rtcore-ntuser-*` API sets. Hosts
  that lack an extension set get the classic DLL that implements the same functions.
- *observed*: a title executable may export an allocation-hook table for the memory library to find.

## Console identity

- *observed*: platform code linked into titles determines the device type by opening
  `HKLM\Software\Microsoft\Windows NT\CurrentVersion\OEM` and reading the `REG_DWORD` value `ConsoleMode`.
  If the key cannot be opened the code concludes it is on a PC.
- *observed*: value mapping used by that code: `0x10`, `0x20`, `0x21`, `0x30`, `0x31` → previous generation,
  base model; `0x40` → its slim revision; `0x50` → its high-end revision; `0x58` → that revision's
  development kit; `0x60` → current generation, smaller model; `0x64` → current generation, larger model;
  `0x68` → current generation development kit; anything else → unknown.
- Implementation: the runtime answers this one query with `0x64` or `0x60` (`--console`).

## Lifecycle

- *documented*: `RegisterAppStateChangeNotification` / `UnregisterAppStateChangeNotification` and the
  constrained-mode pair, with their callback signatures, are public desktop SDK declarations.
- Implementation: registrations succeed; callbacks never fire.

## Memory (`xmem.dll`)

- *assumed*: `XMemAlloc(size, attributes)` returns a pointer and `XMemFree(address, attributes)` releases
  it, with a 64-bit `attributes` word that packs object type (bits 0–13), page size (14–15), allocator id
  (16–23), log2 alignment (24–28) and memory type (29–32). This follows public documentation of the previous
  console generation and has not yet been confirmed against calls from a title; the first allocations are
  logged at debug level so it can be.
- *observed*: `XMemVirtualAlloc(address, size, allocation_type, allocator_id, protect)` returns a pointer and
  null on failure, which titles treat as fatal during start-up.
- *documented*: the page-size bits in `allocation_type` are the ones the public `winnt.h` names
  `MEM_64K_PAGES` (`MEM_LARGE_PAGES | MEM_PHYSICAL`) and `MEM_4MB_PAGES`; `protect` may carry the
  `PAGE_GRAPHICS_*` bits of the same header.
- *observed*: titles reserve very large regions (tens of GiB) at start-up, including a second region with
  graphics-only protection of the same size as the first, then commit pools inside separate allocations.
- *observed*: titles commit several GiB of pools during static initialisation (`MEM_RESERVE | MEM_COMMIT`
  with the large-page bit, graphics protection bits on some). A host whose commit charge cannot cover that
  refuses the requests although physical memory is not touched yet.
- Implementation: when a commit is refused for lack of commit charge (`ERROR_COMMITMENT_LIMIT` and related
  errors) the range is reserved instead and committed in 64 KiB chunks on first touch, from a vectored
  exception handler in the runtime. `ReadFile` into such memory commits the buffer first, because the kernel
  reports an access to uncommitted memory as an error instead of faulting it in. A warning is logged once.
- *assumed*: the physical-page functions follow the shape of the desktop's user-physical-pages API.
- Open: `XMemVirtualQuery`, `XMemGetWorkingSetStatistics`, `XMemIsToolsMemAvailable` signatures.

## Graphics (`d3d12_xs.dll`)

- *observed*: `D3D12ConfigureVirtualMemoryX(configuration)` is called once before any device exists, with a
  pointer to a 32-byte block, and returns an `HRESULT`; a negative result is fatal to the title. The second
  32-bit field has been seen to carry a small count derived from the size of the title's address-space
  reservation.
- *assumed*: `D3D12SerializeVersionedRootSignature` and `D3D12CreateRootSignatureDeserializer` have the
  desktop signatures (same names, same role); they are forwarded to the host.
- Open: `D3D12XboxCreateDevice` and everything behind it.

## Game runtime (`xgameruntime.dll`)

- *observed*: the client library linked into titles loads the runtime by name and looks up, in this order:
  `QueryApiImpl`, `InitializeApiImpl`, `InitializeApiImplEx`, `InitializeApiImplEx2`, `XErrorReport`,
  `UninitializeApiImpl`. It refuses to continue unless `QueryApiImpl`, `XErrorReport`,
  `UninitializeApiImpl` and at least one initialiser exist.
- *observed*: entry-point signatures:
  - `HRESULT InitializeApiImplEx2(uint64_t version_a, uint64_t version_b, uint64_t flags, const void* options)`;
    `InitializeApiImplEx` is the same without `options`; `InitializeApiImpl(uint32_t, uint32_t)` takes the
    low halves. The version words are constants of the client library, each packing two 16-bit build numbers
    in bits 0-15 and 32-47. Bit 1 of `flags` is always set by the client; bits 2 and 3 depend on options and
    on how the runtime was found.
  - `HRESULT QueryApiImpl(const GUID* provider, const GUID* iid, void** object)`.
  - `void XErrorReport(HRESULT, const char* message)`: the client reports usage and installation errors.
  - `void UninitializeApiImpl(void)`.
- *observed*: any runtime function called before explicit initialisation makes the client initialise
  implicitly. A `QueryApiImpl` result of `REGDB_E_CLASSNOTREG` is turned into the client's own "missing
  dependencies" error.
- *documented*: when the runtime cannot be loaded, initialisation fails with the public error code
  `E_GAMERUNTIME_DLL_NOT_FOUND` (`0x89240101`).
- A desktop may have an unrelated DLL with the same file name. A title's request must never be answered by
  it, including requests made by DLLs the package ships; see "Packaged libraries" below.

### Provider interfaces

- *observed*: every runtime function is a thunk in the title's client library: it calls
  `QueryApiImpl(&provider, &iid, &object)`, calls one method through the object's vtable, then `Release`
  (slot 2). Slots 0-2 are `IUnknown`; functions start at slot 3. Provider and interface id are often the same
  GUID. Functions that may block call slot 29 of the core interface first.
- *observed*: within an interface, slots follow the order of the documented declarations of the public
  desktop headers (`XAsync.h`, `XTaskQueue.h`, `XUser.h`, ...). Each slot below was confirmed from the thunk
  that calls it (argument count and widths, return handling) and, where noted, from the call site.
- Implementation: an interface id the runtime does not know yields an object whose every method reports
  itself as unimplemented, so the log names the exact interface and slot a title needs.

| Interface (provider if different) | Slot: function |
| --- | --- |
| `073b7dcb-1fcf-4030-94be-e3c9eb623428` core | 3 `XAsyncGetStatus`, 4 `XAsyncGetResultSize`, 5 `XAsyncCancel`, 6 `XAsyncRun`, 7 `XAsyncBegin`, 9 `XAsyncSchedule`, 10 `XAsyncComplete`, 11 `XAsyncGetResult`, 12 `XTaskQueueCreate`, 13 `XTaskQueueCreateComposite`, 14 `XTaskQueueGetPort`, 15 `XTaskQueueDuplicateHandle`, 16 `XTaskQueueDispatch`, 17 `XTaskQueueCloseHandle`, 19 `XTaskQueueSubmitDelayedCallback`, 22 `XTaskQueueTerminate`, 25 `XTaskQueueGetCurrentProcessTaskQueue`, 29 `XThreadAssertNotTimeSensitive` |
| `bf2346b2-39af-4658-b5ea-44713c7e83b3` (`37e56907-2f10-41e8-b72f-36edb185331a`) networking | 15 `XNetworkingGetConnectivityHint`, 16 `XNetworkingRegisterConnectivityHintChanged`, 17 `XNetworkingUnregisterConnectivityHintChanged` |
| `01acd177-91f9-4763-a38e-ccbb55ce32e0` user | 7 `XUserAddAsync`, 8 `XUserAddResult`, 33 `XUserRegisterForChangeEvent`, 34 `XUserUnregisterForChangeEvent`; the other slots follow `XUser.h` order (3 `XUserDuplicateHandle`, 4 `XUserCloseHandle`, ... 20 `XUserCheckPrivilege`) and are not bound yet |
| `b884675d-b738-4a9c-815d-9a9a1e0c6c9b` | 3 `XSystemGetAnalyticsInfo` (result through a hidden pointer, 0x90 bytes) |
| `1861cf2e-e18b-4834-a9f5-b4a4e6efb4cf` (`e349bd1a-fc20-4e40-b99c-4178cc6b409f`) | 3 `XSystemGetConsoleId` (39-byte buffer), 4 `XSystemGetXboxLiveSandboxId` (16), 5 `XSystemGetAppSpecificDeviceId` (45) |
| `2549f142-6419-4a06-97b5-931aab7c2f34` (`973a344e-24bf-4d0f-8457-56c534892b29`) | 3 `XGameGetXboxTitleId`; 4 is a three-argument void function, assumed `XLaunchNewGame` (not bound) |

### Behaviour

- Task queues and async blocks: the documented semantics of `XTaskQueue.h` and `XAsync.h` (dispatch modes,
  composite queues, delayed callbacks, waiters, monitors, termination with cancellation, a process task queue
  with thread-pool ports). Waiters of an async call return after its completion routine has run. The runtime
  never touches an async block after its completion routine, since titles free blocks there.
- *assumed*: a registered waiter fires once per registration.
- Networking (*documented* struct: level, cost, IANA interface type, then four flags; *observed*: titles treat
  levels 3 and 4 as online and wait for `networkInitialized`). Offline: initialised, level 1 (none). With the
  host network: the host's `GetNetworkConnectivityHint`, whose enumerations have the same values. Change
  notifications register and never fire.
- Users: nobody is signed in. `AddDefaultUserSilently` completes with the documented
  `E_GAMEUSER_NO_DEFAULT_USER` (`0x89245106`); a request that would show the account picker completes with
  `E_ABORT`, the result of a dismissed picker (*assumed* closest answer: there is no picker).
- System: analytics info reports the host's Windows version for both versions and family `Windows.Xbox`,
  form `Xbox Series X` or `Xbox Series S` per `--console` (*assumed* spellings, after documented examples).
  The title id is the manifest's `TitleId`. The sandbox is `RETAIL`. Device ids are stable per host and
  title (a hash of the host machine id and the title id); the console id is formatted as a braced GUID
  (*assumed*; it fits the documented size).

## Network

- *observed*: titles and the DLLs packages ship reach the network through WinHTTP and Winsock (including
  Winsock imports by ordinal), besides the runtime's connectivity service.
- Implementation: the offline switch (default; `--network host` turns it off) makes the process look like a
  PC whose network is disconnected: host names other than the local host fail with `WSAHOST_NOT_FOUND`,
  connections and datagrams to non-loopback addresses fail with `WSAENETUNREACH`, and
  `WinHttpSendRequest` fails with `ERROR_WINHTTP_NAME_NOT_RESOLVED`, the documented errors of the host APIs
  in that situation.

## Packaged libraries

- *observed*: DLLs shipped in packages import host functions directly (file access, library loading,
  networking), and one kind of them loads the game runtime by name itself.
- Implementation: after the OS loader binds a packaged DLL, the runtime points its imports of interposed host
  functions at the interposers, as it does for the title image. By-ordinal imports are matched by the name
  the host library exports under that ordinal.

## Console input (`GameInput.dll`)

- *observed*: titles import ordinal 1, `GameInputCreate(IGameInput**)`, and call `IGameInput` with the
  public v0 slot order (slot 9 receives `RegisterDeviceCallback`'s seven arguments: device, kind mask,
  status filter, enumeration kind, context, callback, token).
- *observed*: `0x838A0003` (`GAMEINPUT_E_READING_NOT_FOUND`) is the "no reading" result.
- Implementation, partial: no input device is surfaced yet. Enumeration finds no device, reading queries
  return `0x838A0003`, callback registrations succeed and never fire. Slots 3-6, 8, 9, 12 and 13 are bound.

## Libraries not started

Import names seen, contracts open: the audio hardware interfaces (`AcpHal.dll`, `xapu.dll`, `chad.dll`), the fast-storage factory (`XCRDAPI.dll`,
`DStorageGetFactory`), and texture layout (`xg_xs.dll`).
