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

- *observed*: `XMemAlloc(size, attributes)` returns a pointer and `XMemFree(address, attributes)` releases
  it. The 64-bit `attributes` word packs object type (bits 0–13), page size (14–15), allocator id (16–23),
  log2 alignment (24–28) and memory type (29–32): titles' own allocation hooks test exactly the object-type,
  page-size and allocator-id fields.
- *observed*: a title may export `__XMemAllocationHooks__`, a table whose first two entries are an allocation
  function with `XMemAlloc`'s signature and a release function with `XMemFree`'s. Every `XMemAlloc`/`XMemFree`
  goes to them; they chain to `XMemAllocDefault`/`XMemFreeDefault`. Titles register callbacks in them per
  allocator id and thread to place system allocations at addresses of their choosing.
- *documented* flags also seen in `XMemVirtualAlloc`: `MEM_RESERVE_PLACEHOLDER` (titles reserve placeholder
  regions) and `MEM_REPLACE_PLACEHOLDER` (titles replace parts of them with committed or reserved memory);
  protection may carry every `PAGE_GRAPHICS_*` bit, `PAGE_GRAPHICS_NOCACHE` included.
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
- *observed*: the physical-page functions have the shape of the desktop's user-physical-pages functions with
  64 KiB pages: `BOOL XMemAllocatePhysicalPages(uint64_t attributes, ULONG_PTR* count, ULONG_PTR* pages)`
  (count in and out; titles round byte sizes up with `>> 16` and check for `TRUE` and the full count),
  `BOOL XMemFreePhysicalPages(ULONG_PTR count, ULONG_PTR* pages)`,
  `void* XMemMapPhysicalPages(void* address, ULONG_PTR count, ULONG_PTR* pages)` (the result is tested as a
  pointer; a null array unmaps) and `BOOL XMemMapPhysicalPagesScatter(void** addresses, ULONG_PTR count,
  ULONG_PTR* pages)`. Pages are mapped into reservations made with `XMemVirtualAlloc` and the 64 KiB-page
  flags.
- *observed*: `XMemTransferMemory(uint32_t, size_t bytes)` is called with sizes rounded to 2 MiB and capped at
  16 MiB; the result is not acted on (*assumed* `HRESULT`).
- Implementation: page ids are opaque. A page gets its storage when first mapped: a 64 KiB slice of a
  committed pagefile-backed section laid out like the region it is mapped into (one section per chunk of up to
  1 GiB), at the offset of the granule. Pages a title maps side by side are therefore contiguous in one section,
  which lets the graphics library open one GPU heap over them. Mapping replaces 64 KiB placeholders with views
  of the slices, so a page mapped at two addresses is the same memory; private reservations titles carved out of
  a placeholder become placeholders again when pages are mapped into them. Without enough commit charge,
  sections are reserved and pages committed when mapped or touched.
  `XMemTransferMemory` has nothing to move between partitions on a host and succeeds.
- *observed*: titles allocate and map several GiB of physical pages for a texture pool during start-up and
  treat a failed mapping as fatal.
- *documented* host behaviour: the host opens GPU heaps only over committed sections (`SEC_COMMIT`); a
  `SEC_RESERVE` section is refused even when all of it is committed through a view.
- Implementation: storage is created lazily. Mapping a page that has none only records the mapping; the page
  gets its slice, and the granule its view, on first CPU access (a vectored exception handler in the memory
  library), when a buffer in it is handed to the kernel (the runtime's commit-before-I/O path calls the memory
  library first), or when the graphics library asks for the memory behind an address. Fresh storage reads as
  zero. Commit charge is taken per section (up to 1 GiB) when its first page is used, not at mapping time.
  Code holding the page-table lock never touches title memory (title arrays are copied in and out around
  it), since a fault there would re-enter the backing handler.
- Implementation: commits of 64 MiB or more are always reserved and committed on first touch, not only when
  the host refuses them: titles commit pools of several GiB up front and otherwise take the host's spare
  commit charge before anything else can.
- Open: `XMemVirtualQuery`, `XMemGetWorkingSetStatistics`, `XMemIsToolsMemAvailable` signatures.

## Graphics (`d3d12_xs.dll`)

- *observed*: `D3D12ConfigureVirtualMemoryX(configuration)` is called once before any device exists, with a
  pointer to a 32-byte block, and returns an `HRESULT`; a negative result is fatal to the title. The second
  32-bit field has been seen to carry a small count derived from the size of the title's address-space
  reservation.
- *assumed*: `D3D12SerializeVersionedRootSignature` and `D3D12CreateRootSignatureDeserializer` have the
  desktop signatures (same names, same role); they are forwarded to the host.
- *observed*: `HRESULT D3D12XboxCreateDevice(adapter, const parameters*, REFIID, void** device)` is called once,
  after the title has created its window, with a null adapter, a 0x40-byte parameter block starting with the
  version `0x20004`, and the device interface id `5405c344-d457-444e-b4dd-2366e45aee39`.
- *observed*: methods that return a structure are called C-style: the result pointer is the first argument and
  the object the second (device slot 26, called with node mask 1 and heap types 1-3, is the desktop
  `GetCustomHeapProperties`). All other methods take the object first.
- *observed*: the device's vtable follows the desktop `ID3D12Device` order only in places: slot 8
  `CreateCommandQueue` (desktop `D3D12_COMMAND_QUEUE_DESC`, desktop `ID3D12CommandQueue` id), 9
  `CreateCommandAllocator`, 26 `GetCustomHeapProperties`, 36 `CreateFence` (desktop `ID3D12Fence` id). Slot 48
  also creates command queues (used for compute queues, with a flags value that has no desktop meaning), and slot
  73 creates command lists from a 16-byte descriptor (first dword the list type), an allocator, an initial
  state and the console command-list id `c3827890-4024-474c-98cf-645b51dc4874`.
- *observed*: fences and allocators keep the desktop method order for the slots titles call (fence 7
  `GetDevice`, 8 `GetCompletedValue`, 10 `Signal`; allocator 8 `Reset`).
- *observed*: the console command list has well over 130 slots; slot 9 is `Close`. Right after creating a
  list, titles call slot 132 with `(0x700000, 0, 0, 0xffffffffff00)` and slot 131 with
  `(0xc3c0, 0x80020000, 0, 0xffffffffff00)`, without checking a result. The values look like hardware register
  offsets, values and masks; what the calls do cannot be established from call sites or desktop documentation.
- *observed*: titles query the device for `IDXGIDevice` and walk adapter and output through it; the console
  output's display-mode query is at slot 21 and fills 32-byte entries, which is not the desktop
  `IDXGIOutput` order.
- *observed*: the interface methods use the C calling convention: structures of up to eight bytes (descriptor
  handles) come back in `rax`, larger ones through a result pointer passed before the object.
- *observed*: titles reach `IDXGIDevice` through the device, then `GetAdapter` (slot 7), `EnumOutputs`
  (adapter slot 7) and a display-mode query at output slot 21, `(format, flags, count*, modes*)` with 32-byte
  entries in the `DXGI_MODE_DESC1` layout; titles pick 120 Hz when a mode has it.
- *observed*: frame pacing as in public sample code: device slot 78 `SetFrameIntervalX(output, length_us,
  period, flags)`, slot 79 `ScheduleFrameEventX(type, offset_us, output, flags)`.
- *observed*: device slots 14 `CreateDescriptorHeap`, 15 `GetDescriptorHandleIncrementSize`, 16
  `CreateRootSignature`, 17–24 the desktop view creation and descriptor copy methods, and 25
  `GetResourceAllocationInfo` keep the desktop meanings. A descriptor heap's CPU memory comes from `XMemAlloc`
  with object type 4 and allocator id 0x80: titles place it with an allocation hook and require the heap's CPU
  start to be their address. Titles ask for shader-visible heaps of 1,245,184 descriptors.
- *observed*: slot 70 is also called for textures (a texture description, a view description and the texture's
  address in title memory, laid out as the texture layout library reported). Implementation: a host texture of
  the same shape is created and filled from that memory with buffer-to-texture copies, once per address and
  shape. Partial: memory written after that is not copied again; textures the GPU writes are not supported.
- *observed*: device slot 67 takes (description, pipeline interface id, out) and returns an `HRESULT`. The
  description holds the root signature and (pointer, size) pairs: 28 bytes of what look like register settings
  (an 8x8 thread group among them), 20 bytes not understood, and native GPU code behind a short header. It
  creates compute pipelines from native code given directly. Not supported yet: the code seen samples and
  stores images and reduces values across lanes (DPP, `v_readlane`), which the translation lacks; such calls
  report themselves.
- *observed*: titles create constant buffer views of 128 bytes; the desktop requires multiples of 256 and removes
  the device otherwise. Views are widened to the next multiple of 256.
- *observed*: titles ask for the descriptor increment of heap type 4, which the desktop does not have; what the
  type is has not been established (the host answers 0, logged as an error).
- *observed*: slot 70 makes a shader-resource view from a `D3D12_RESOURCE_DESC` and a
  `D3D12_SHADER_RESOURCE_VIEW_DESC` followed by the resource's GPU address, into a destination descriptor;
  titles flush the destination's cache lines afterwards.
- *observed*: slot 127 has the shape of the desktop `CreateCommittedResource3` (heap properties, heap flags,
  description, initial state, clear value, protected session, castable formats). Titles create their display
  buffers with it (`ALLOW_DISPLAY`) inside an allocation hook that serves allocator id 0x80 with 64 KiB pages
  from an arena sized for those buffers, and then require the arena to be used up: the console runtime takes a
  committed resource's memory from `XMemAlloc`.
- *observed*: slot 133 `CreatePlacedResourceX(gpu_address, desc, state, clear, riid, out)` places a resource
  at an address in memory the title allocated; resource descriptions may carry flag bits above the desktop's
  (0x20000, 0x100000, 0x200000), and root signatures flag bits 0x1000000 and 0x2000000.
- *observed*: slot 62 creates compute pipelines from a `D3D12_COMPUTE_PIPELINE_STATE_DESC`, two null pointers,
  the interface id and the out pointer.
- *assumed*: command-list slots 131 and 132 program GPU state by register offset, value and mask; they are
  recorded and not applied, since the host driver owns its GPU's state.
- *observed*: device slots that keep the desktop arguments: 27 `CreateCommittedResource`, 39
  `CreateQueryHeap` (titles create occlusion query heaps and readback buffers of 8 bytes per query), 41
  `CreateCommandSignature` (titles pass a null root signature).
- *observed*: command signatures use the desktop argument types and also type 105 with a 12-byte stride, seen
  next to a desktop dispatch (type 2) and mesh dispatch (type 10) signature of the same stride. What type 105
  does is not established; such signatures are created (with a desktop stand-in of the same stride) and
  executing one will have to report itself.
- *observed*: device slot 61 takes an 84-byte console sampler description and a destination descriptor
  address (titles build the description from their own packed sampler state). *assumed* fields: dwords 1-3
  min, mag and mip filter (1 point, 2 linear), 6-8 address modes U/V/W in desktop values, 9-10 min and max
  LOD, 12 LOD bias, 13 comparison function (desktop values, 0 none), 16 maximum anisotropy, 17-20 border
  colour. Dwords 0, 4, 5, 11, 14 and 15 are not understood and ignored.
- *observed*: device slot 74 takes the address of a reservation with physical pages mapped, the page count
  (64 KiB pages) and the out pointer, and returns an `HRESULT`; titles call it for their texture pool. The
  object behaves as a heap (interface slots 0-2, 7 `GetDevice`, 8 `GetDesc` with a result pointer). Host
  heaps over that memory are opened per piece of storage only when something is placed or mapped there.
- *observed*: device slot 86 takes a `D3D12_PIPELINE_STATE_STREAM_DESC` (size, stream), the interface id and
  the out pointer, as the desktop `CreatePipelineState`. Streams use the desktop subobject layout; the first
  ones titles create carry root signature, vertex, pixel and empty hull/domain/geometry shaders, blend, sample
  mask, rasterizer, depth-stencil (type 21), sample description, input layout, topology and target formats.
  Their shaders carry only native code and are translated (see Graphics shaders); hull, domain and geometry
  shaders with native code, and pipelines mixing native and desktop shaders, report themselves.
- Implementation:
  - GPU addresses are title addresses, as on the console. Each piece of title memory resources are placed in
    gets a host heap opened over the same memory (from the physical-page section, or from the committed
    address) and a buffer spanning it; title GPU addresses translate into it, placed buffers are placed in it,
    and a placed buffer's `Map` yields the title's memory.
  - Descriptor heaps take their CPU memory from `XMemAlloc` as above; title CPU handles translate to host heaps
    by offset. Shader-visible heaps above the host's limit of 1,000,000 descriptors are created at the limit
    (partial: descriptors past it are refused).
  - Committed resources are host committed resources; the matching `XMemAlloc` is made so titles' accounting
    holds.
  - Root signature flag bits above the desktop's are dropped. Compute pipelines whose shaders keep their DXIL
    run from that program; shaders with only native GPU code, and shaders whose DXIL the host refuses, run
    translated from their native code (see Graphics shaders). Recording and executing commands for
    translated pipelines is not implemented yet.
  - Every other slot, and any interface query other than the device's own and `IDXGIDevice`, reports itself.

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
| `0b9d556e-ab70-43f4-baa0-9999ac4cb040` (`af406016-e850-4aa8-a88d-2f3dcb9dac7e`) package | 3 `XPackageGetCurrentProcessPackageIdentifier` (33-byte buffer), 4 `XPackageIsPackagedProcess`, 5 `XPackageCreateInstallationMonitor`, 6 `XPackageCloseInstallationMonitorHandle`, 7 `XPackageGetInstallationProgress`, 9 `XPackageRegisterInstallationProgressChanged`, 39 `XPackageEnumeratePackages` |
| `41a4e10c-5a7e-41d9-8c37-37bde62a07d6` (`f4faf4d4-2d04-4fce-b3e0-474a713a3e84`) persistent local storage | 4 `XPersistentLocalStorageGetPath`, 5 `XPersistentLocalStorageGetSpaceInfo` |
| `35f07670-706e-4bfb-9476-090798c5ebf3` (`03f0fe74-fdd9-4e5c-b630-f9339c47acc5`) display | 4 `XDisplayTryEnableHdrMode` |

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

- Packages: the process is packaged; installation monitors report a finished installation of the package's
  size on disk (*documented* `XPackageInstallationProgress` layout); progress callbacks never fire.
  Enumerating content packages finds none (the runtime never reports add-on content); enumerating game
  packages is not implemented and reports itself. The package identifier is an opaque name derived from
  manifest ids.
- Persistent local storage: a directory in the title's data directory; the quota is the manifest's
  `<PersistentLocalStorage><SizeMB>`; space info follows the *documented* struct (available, free on disk,
  used, total).
- Display: *documented* `XDisplayHdrModeResult` values (1 enabled, 2 disabled; *observed*: titles compare
  with 1). HDR is reported disabled because presentation is standard dynamic range.

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

## Fast storage (`XCRDAPI.dll`)

- *observed*: `DStorageGetFactory(REFIID, void**)`, the public desktop signature; the console factory has its
  own interface id `62959d11-d45f-4884-a0a3-f7862ea243ae`.
- *observed* factory slots: 4 `OpenFile(const wchar_t* path, REFIID, void**)` (wide path; titles check it
  with `GetFileAttributesW` first), 5 `CreateStatusArray(uint32_t capacity, const char* name, REFIID,
  void**)`, 8 takes a 32 MiB size before any queue exists (staging buffer size), 10 `CreateQueue(desc,
  REFIID, void**)` with a descriptor laid out like the public `DSTORAGE_QUEUE_DESC` (source type, capacity,
  priority, name) followed by a field titles set to 1.
- *observed*: the queue descriptor is not the desktop layout past the source type: the name pointer is at offset
  16 and the field titles set to 1 at offset 24.
- *observed* queue slots: 3 `EnqueueRequest(const Request*)`, 4 `EnqueueStatus(status array, index)`. Request
  layout (from how titles fill it): options at 0 (bit 22: the source is memory; bit 0: set for the titles'
  format 3), destination address at 8, destination size at 0x18, source (file object or memory address) at
  0x20, file offset at 0x28, source size at 0x30. Requests with bit 0 carry zlib streams (RFC 1950 headers
  seen), whose inflated size is the destination size.
- *assumed*: queue slots 5 `EnqueueSignal(fence, value)` and 6 `Submit()`, status array slots 3
  `IsComplete(index)` and 4 `GetHResult(index)`, in the desktop order (not yet called by titles).
- Implementation: requests are carried out when enqueued (reads through the title's path translation, zlib
  inflated by the project's decoder), so they complete earlier than on the console; a status entry gets the
  first failure since the previous one. Partial: requests are processed one at a time on the calling thread
  (slow: titles stream thousands at start-up); other request kinds and queue slots report themselves.

## Graphics shaders

- *observed*: console shader containers have the desktop container layout, but their `DXIL` part holds a
  console block (`XDAT`) with a zlib-compressed `FSSM` payload: the console GPU's native machine code
  (RDNA-family instruction encodings: `s_waitcnt`, exports, `s_endpgm`, `s_code_end` padding) and metadata.
  Inside the block sits a DXIL program header followed either by LLVM bitcode (shaders in some packaged
  libraries) or by a 4-byte `NOBC` marker (titles' own shaders).
- *documented*: the container digest is the DXBC checksum (MD5 with a modified final block); the
  implementation reproduces console containers' digests byte for byte and the host compiler's.
- *observed*: in graphics shaders (and some compute shaders) the `DXIL` part starts with a desktop program
  header whose bitcode offset skips the console block; the "program" it points at may be just the `NOBC`
  marker. Such containers carry only native code even though the part looks like a desktop one. Graphics
  shaders seen so far have console block version 3 (compute: 5) and an extra 12-byte `PRIV` part.
- *observed*: the console block starts with `XDAT` and a version; from byte 16 follow (offset, size, kind)
  entries, offsets relative to `XDAT`, up to the first entry's data. Kind 10 is the DXIL bitcode, kind 16 the
  zlib stream of the native program block (`FSSM`). Its recorded size has been seen to run a few bytes past the
  part; the stream itself ends inside it. The DXIL program header in front of the console block is a valid
  desktop program header (its bitcode offset skips the block), so such containers also pass as desktop ones.
- *observed*: in the `FSSM` block, the dword at 0xC0 is the offset of the code record, whose machine code starts
  0x28 bytes in; the dword at 0xF8 equals the number of root signature parameters (3, 4 and 1 in the shaders
  compared with their root signatures). The native program sits in the console block entry of kind 16 (block
  version 5) or 7 (version 3). The thread group size is in `PSV0` as on the desktop (runtime information
  bytes 40-51).
- *observed* register inputs: compute programs read `s[0:1]` first (the root table address: root parameters
  packed in root-signature order, constants 4 bytes each, root descriptors 8 bytes) and a workgroup id in `s4`.
  Vertex shaders read a single root descriptor directly in `s[8:9]`; pixel shaders take `M0` from `s10`, the
  register after ten user SGPRs, as the primitive mask interpolation uses.
- *assumed*: compute programs have four user SGPRs with the workgroup ids (x, y, z) after them; lane registers
  `v0`-`v2` hold the thread id in the group; graphics root data starts at `s8`; vertex shaders get the vertex id in
  `v0` and the instance id in `v3`; a program is wave64 when it writes `EXEC` with 64-bit scalar operations
  (wave32 otherwise). Not yet confirmed by running translated code against known results.
- *documented*: the native code is RDNA 2 (public AMD instruction set reference); decoding follows it.
- *documented*: host bytecode (DXBC, shader model 5.1) follows the desktop runtime's tokenized program format;
  a container needs input and output signature parts besides `SHEX`, and the DXBC checksum.
- Implementation, desktop programs: containers with bitcode are rebuilt as desktop containers (plain DXIL
  program, desktop parts kept, `RTS0` flags masked, digest recomputed) and run on the host.
- Implementation, native programs (compute only): the native code is translated straight into DXBC, without a
  shader compiler. One host thread runs one lane; scalar registers are recomputed per lane; lane masks keep the
  lane's own bit; control flow becomes a loop around a switch over block addresses; workgroup barriers leave
  the loop and synchronise at the top level. Memory is reached through a region table: title GPU addresses
  (which equal CPU addresses) are looked up in a constant buffer of (base, size, view) entries and accessed
  through raw views (u0-u63, space 8). Scalar and vector ALU, scalar loads, buffer loads and stores
  (untyped, and typed for the 8/16/32-bit integer, normalised and float formats seen), global and flat
  memory, atomics and group shared memory (32 KiB, the host's limit) are translated. Not translated, and
  reported as such: cross-lane operations (DPP, lane reads with a lane mask), image instructions, and anything
  else not listed.
- Implementation, native graphics programs (vertex and pixel): the same translation, with the 16 user SGPRs
  from the system constants. Parameter exports (`PARAMn`) become `TEXCOORDn` outputs in register n + 1 and the
  first position export `SV_Position`; a pixel shader's attribute n is the input `TEXCOORDn`, interpolated by
  the host (`v_interp_p1`/`p2` read it, `v_interp_mov` reads it without interpolation); colour exports become
  `SV_Target` outputs (packed 16-bit exports are taken as half floats), the depth export `SV_Depth`. Pixel lanes
  no longer in `EXEC` at an export, and lanes that export nothing, are discarded. Other position exports
  (point size, clip distances) are dropped. Pipelines use the translated root signature.
- *observed* host behaviour: the host driver refuses (`E_OUTOFMEMORY`) translated programs that read temporary
  registers on paths where they were never written, and accepts the same programs with every such register
  set to zero first; translated programs therefore start by zeroing every register they use.
- *observed*: on the host GPU used for development, one packaged compute program is refused
  (`E_OUTOFMEMORY`), and a sandboxed (low-integrity) process that has seen this refusal leaves the driver's
  shader cache in a state that makes every later process using it fail fast inside the driver. Mitigations:
  the title process's profile path points at a private host cache directory in its data directory (the
  driver and the graphics runtime locate their shader caches from it); a refusal marks that cache for
  discarding before the next run, and the refused shader's hash is remembered there so later runs translate
  it without asking the host again.
- *observed*: titles do not write GPU command packets themselves; draws and dispatches go through the
  console D3D12 interfaces.

## Texture layout (`xg_xs.dll`)

- *observed*: titles import `XGSuggestSwizzleMode(const desc*, uint32_t, uint32_t* mode)` and
  `XGCreateTextureComputer(const desc*, computer**)`, both checked as `HRESULT`s. The 56-byte description has the
  desktop `D3D12_RESOURCE_DESC` layout (dimension, alignment, width, height, depth or array size, mip levels,
  format, sample description, layout, flags) with the console's flag bits.
- *observed*: titles fill textures through the computer: slot 8 `CopyIntoSubresource(destination base, plane,
  subresource, source, source row pitch, source slice pitch)`, called per subresource; the result is not
  checked.
- Implementation: the layout a title follows is the one this library reports, so it is chosen to be the
  desktop's copyable footprint layout (subresources in order, each at a multiple of 512 bytes, rows 256 bytes
  apart; core/libs/texture_layout.h); the swizzle mode reported is 0 (*assumed* linear). Other computer slots
  report themselves.

## Libraries not started

Import names seen, contracts open: the audio hardware interfaces (`AcpHal.dll`, `xapu.dll`, `chad.dll`).
