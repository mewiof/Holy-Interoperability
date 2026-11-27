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

## Processor

- *observed*: titles execute vendor-specific instructions of the console processor that some desktop processors
  lack: `CLZERO` (zeroing caches lines of fresh buffers) and `MONITORX`/`MWAITX` (waiting on fence values).
  *documented* (vendor programmer's manual): `CLZERO` zeroes the naturally aligned 64-byte line containing rAX;
  `MWAITX` may end early for implementation-specific reasons.
- Implementation: an illegal-instruction handler carries them out (`MONITORX` as nothing, `MWAITX` as yielding
  the processor) and resumes after the instruction; title code is not modified. Each use costs an exception.

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
  Which chunks are committed is kept in a bitmap per range, so a buffer that is already committed costs no host
  call (*measured*: asking the host about every buffer kept a loading thread in the kernel for most of the first
  half minute); the title's `VirtualFree` is interposed so that decommitted or released memory is forgotten, and
  a read the kernel still refuses for lack of access is retried after asking the host.
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
- *observed*: titles keep page numbers packed as `number >> 4` in 21-bit fields and rebuild them as
  `value << 4` before mapping them: page numbers are frame numbers in 4 KiB units, so every 64 KiB page's number
  is a multiple of 16 (as the desktop's user physical page frame numbers count 4 KiB frames). Numbers that are
  not are refused. Handing out consecutive numbers made such titles map 16 different pages as one.
- Implementation: a page gets its storage when first mapped: a 64 KiB slice of a
  reserved pagefile-backed section laid out like the region it is mapped into (one section per chunk of up to
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
  zero. Commit charge is taken per page when it gets storage, and per 64 MiB block of a section when the GPU
  first needs memory in it (the graphics library opens heaps over committed blocks of a view of the section:
  *measured* host behaviour, a heap over a committed stretch of a view of a reserved section works and the GPU
  and CPU see each other's writes), not at mapping time and not per section.
  Storage a freed page leaves behind is cleared before another page takes it, so freshly allocated pages read
  as zero (*assumed*, as the system's own pages do). A freed page that is still mapped somewhere keeps its
  storage until its last mapping goes, so a new page never shows through an old mapping.
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
- *observed*: methods that return a structure are called both ways: with the result pointer first and the
  object second (device slot 26, called with node mask 1 and heap types 1-3, is the desktop
  `GetCustomHeapProperties`), and with the object first and the result pointer second, the compiler's order for
  member functions (`ID3D12Resource::GetDesc`, slot 10, from titles' C++ code). Implementation: such methods tell
  the object from the result buffer by the object's identity (`lib::struct_return_arguments`). Taking one order
  for the other made `GetDesc` copy the title's uninitialised result buffer over the resource object, which the
  title then called through (a jump through garbage) and gave its unfilled description to device slot 70. All
  other methods take the object first.
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
  shape; textures in pages mapped one by one (no host heap backs them as a whole) are copied from the title's
  addresses through the CPU. Partial: memory written after that is not copied again; textures the GPU writes are
  not supported.
- *observed*: device slot 67 takes (description, pipeline interface id, out) and returns an `HRESULT`. The
  description holds the root signature and (pointer, size) pairs: 28 bytes of compute registers (dword 3 the
  thread group width and height, the low half of dword 4 its depth (its high half, 0x10 seen, is not understood), 6 `COMPUTE_PGM_RSRC2` in the public RDNA layout: user SGPRs, group
  id and group information enables, LDS size), 20 bytes not understood, and native GPU code. It creates compute
  pipelines from native code given directly; they are translated.
- *observed*: titles create constant buffer views of 128 bytes; the desktop requires multiples of 256 and removes
  the device otherwise. Views are widened to the next multiple of 256.
- *observed*: titles also create constant buffer views at addresses that are not 256-byte aligned (32 bytes
  past a multiple seen), which the desktop does not allow. The host descriptor is made a null view; translated
  native code reads constant buffers through the title's descriptor memory and keeps the exact address.
  Partial: rebuilt desktop programs reading such a view read zeros (logged once).
- *observed*: titles ask for the descriptor increment of heap type 4, which the desktop does not have; what the
  type is has not been established (it is answered like resource views, 32 bytes).
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
    gets a host heap opened over the same memory (over a view of the physical-page section kept for the heap,
    or over the committed address) and a buffer spanning it; title GPU addresses translate into it, placed
    buffers are placed in it, and a placed buffer's `Map` yields the title's memory.
  - *measured* host behaviour (development host): a heap opened from a section handle
    (`OpenExistingHeapFromFileMapping`) leaves the whole section's commit charged to the system after the
    process ends, whether the heap was released or not (1 GiB a section here; only a restart of the host gives
    it back); a heap opened over a mapped view of the same section (`OpenExistingHeapFromAddress`) does not.
    The project only opens heaps over addresses.
  - *measured* host behaviour (development host): heaps opened over existing memory are cross-adapter heaps
    (flags shared and shared cross-adapter), and mapping tiles of a reserved buffer into one
    (`UpdateTileMappings`) removes the device (`DXGI_ERROR_INVALID_CALL`), with or without the cross-adapter flag on
    the buffer. Title memory therefore cannot be given to the GPU as reserved buffers whose tiles follow the
    physical pages; pages that are not contiguous in one section stay separate pieces of GPU memory.
  - Committed textures report the memory the runtime took for them (`XMemAlloc`, as on the console) as their
    GPU address, and views the title makes at that address find the committed texture.
  - *observed*: titles commit stretches of a reservation and leave the rest reserved. *observed* host
    behaviour: `OpenExistingHeapFromAddress` refuses an allocation that is not committed throughout, while
    `OpenExistingHeapFromAddress1` (`ID3D12Device13`) opens a heap over a committed stretch given with its size;
    such allocations get a heap per committed stretch.
  - Descriptor heaps take their CPU memory from `XMemAlloc` as above; title CPU handles translate to host heaps
    by offset. Shader-visible heaps above the host's limit of 1,000,000 descriptors are created at the limit
    (partial: descriptors past it are refused). *measured* (the host's debug layer): the host's shader-visible
    heaps may only be written by the CPU, and titles copy descriptors out of theirs; each such heap therefore
    has a CPU-readable shadow: descriptors are written into the shadow and copied to the visible heap, and
    copies read shadows. Heaps over committed title memory end where the page protection changes (the host
    refuses mixed protection; titles hand thread stacks, with their guard pages, to the GPU too).
  - Committed resources: the matching `XMemAlloc` is made so titles' accounting holds. Buffers in heaps the CPU
    reaches (upload, readback, custom heaps with CPU access) live in that memory, a host buffer placed in the heap
    over it: their GPU address and the pointer `Map` gives are that memory, as on the console, so views, root
    views and native code reach them (an earlier implementation gave the host's GPU address, also as `Map`'s
    pointer). Textures, and buffers only the GPU uses, are host committed resources (partial: such a buffer's GPU
    address is the host's, which views and native code do not reach).
  - *assumed* (slots 0-25 keep the desktop order, observed for others): command list slots 15, 16 and 17 are
    `CopyBufferRegion`, `CopyTextureRegion` and `CopyResource`; the host resources behind the title's take the
    copies (placed buffers at their offset into the host buffer). Partial: copies into title memory under a
    texture the title views there do not reach its host copy.
  - Root signature flag bits above the desktop's are dropped. Compute pipelines whose shaders keep their DXIL
    run from that program; shaders with only native GPU code, and shaders whose DXIL the host refuses, run
    translated from their native code (see Graphics shaders); their draws and dispatches receive root
    arguments as described under "Root arguments of native code".
  - Every other slot, and any interface query other than the device's own and `IDXGIDevice`, reports itself.

### Command recording and submission

- *observed*: the console command list keeps the desktop slot order for slots 0-25 (9 `Close`, 12
  `DrawInstanced`, 13 `DrawIndexedInstanced`, 14 `Dispatch`, 20 `IASetPrimitiveTopology`, 21 `RSSetViewports`,
  22 `RSSetScissorRects`, 23 `OMSetBlendFactor`, 24 `OMSetStencilRef`, 25 `SetPipelineState`). From desktop
  slot 28 to 48 the slots are the desktop's plus four (32 `SetDescriptorHeaps`, 33-46 root signatures, tables,
  constants and root views, 47 `IASetIndexBuffer`, 48 `IASetVertexBuffers`, 50 `OMSetRenderTargets`, 51
  `ClearDepthStencilView`, 52 `ClearRenderTargetView`); `OMSetDepthBounds` (desktop 62) is slot 70. Slot 126 is
  `RSSetShadingRate(rate, combiners[2])`, slot 139 a reset `(allocator, pipeline state, 0)`, slot 146 the enhanced
  `Barrier(group count, groups)` with desktop `D3D12_BARRIER_GROUP`s.
- *observed*: barrier sync, access and layout values are the desktop's, except layout 31 (one past the desktop's
  last), which titles move display buffers to right before presenting them; it is taken as the present layout.
- *observed*: the list object exposes a command-buffer write cursor at offset 0x18 and its limit at 0x30. While
  the cursor is below the limit titles write commands into the buffer themselves (advancing the cursor, setting
  bytes at 0x140/0x141) instead of calling the method. Implementation: both are kept zero, so every command
  reaches a method; the first 0x400 bytes of the object are a zeroed area titles may treat as theirs.
- *observed*: slot 71 `(address, 32-bit value, 0, 0)` writes progress markers into title memory, slot 74
  `(address, 7, value, 1)` a frame counter the CPU polls, slot 72 `(address, 0, 0, 1)` an 8-byte GPU timestamp
  (*assumed* meanings). Implementation: `WriteBufferImmediate` (marker-out for slot 74) and timestamp queries
  resolved into title memory.
- *observed*: slot 142 is called with all ones before draws (*assumed*: the sample mask; other values report
  themselves); slot 75 takes one byte titles derive from a pipeline property (meaning not established; recorded,
  not applied).
- *observed*: command allocator slot 9 registers a title function through which the console allocator obtains
  command-buffer memory (size as the 4th argument, log2 alignment as the 5th, context as the 7th). Host
  allocators manage their own memory; the function is recorded and never called.
- *observed*: queue slot 16 `Signal(fence, value)`, 18 `GetTimestampFrequency` (both two slots later than on the
  desktop), 32 `PresentX(plane count, planes, parameters)`, 35 `ExecuteCommandListsX(count, lists, frame token,
  flags)` (up to 256 lists).
- *observed*: queue slot 31 takes `(count, mappings, pool address, pool page count, 0)`; the result is not
  checked. Each mapping is `{64 KiB-aligned address, run count, runs}` (24 bytes), each run
  `{kind, pool page, page count}` (12 bytes): kind 1 maps `count` pages of the pool (pool address + page × 64 KiB,
  a physical-page reservation the title made a heap of with device slot 74) at the next pages from the mapping's
  address on; kind 2 unmaps `count` pages there (titles then return the pool pages to their free list). Titles use
  it to move texture pool pages into and out of a streaming address range; the page numbers are pool page
  indices, not physical page numbers. *assumed*: it takes effect in queue order. Implementation: the pages are
  remapped when the call is made, through the memory library (`XMemMapPhysicalPages` with the physical pages
  behind the pool pages). GPU addresses are resolved when commands are recorded, so recorded work keeps the old
  pages; partial: work recorded before the call but submitted after it would see the new ones on the console.
- *observed*: titles set primitive topology 17 (0x11) with slot 20, a value the desktop does not define: the
  console GPU's rectangle list (*documented* in public RDNA register references as `DI_PT_RECTLIST`): each three
  vertices are three corners of a rectangle, the GPU adds the fourth. *observed* host behaviour: given to the host
  as is, the host device is removed as hung (`DXGI_ERROR_DEVICE_HUNG`) within milliseconds of the list's next
  submitted work, whatever that work is (an immediate write to the host's own memory is enough); device-removed
  data shows the list stopped before its first operation. Implementation: rectangle lists are drawn as triangle
  lists with a variant of the pipeline (made at its first such draw) whose generated geometry program passes each
  triangle on and adds the fourth corner as v1 + v2 - v0 for every vertex output, a strip of four vertices
  (attributes varying affinely over the rectangle complete the same way; checked on the host GPU by a unit test).
  The geometry program is built from the vertex program's output signature. Partial: NGG pipelines and vertex
  programs without a readable output signature still draw one half of each rectangle. Other topologies the
  desktop does not define report themselves.
- *observed*: titles reset a command list and submit it again while its previous submission is still executing;
  the desktop forbids submitting a list before the GPU has finished its previous submission (the debug layer
  refuses it). Implementation: title lists record into host lists from a pool; a reset whose host list may still
  be executing takes one the GPU is done with (queues signal a fence after each submission), and a recording
  submitted twice waits for its first submission.
- Implementation: draws with a pipeline whose vertex program is an NGG program (see Graphics shaders) are two
  host passes: a dispatch of the translated program writes the draw's vertex and primitive records into scratch
  memory owned by the host list (returned to a pool when the list is reset), then an indexed triangle-list draw
  of those records. The title's index buffer and topology are restored afterwards. Partial: indexed draws with
  such pipelines are not drawn (logged).
- *observed*: fence slot 12 (past the desktop `ID3D12Fence1`) returns the address of the fence's current value;
  titles poll it, reading all ones as a removed device, and read what the GPU wrote once the value shows the work
  done. Implementation: a cell owned by the fence (released with its last reference), kept at the host fence's
  completed value by one thread for all cells, woken by any watched fence's next completion; each fence's event
  is set again only when its value moved. *Measured* (sample title): the allocator often hands a new fence the
  address of one just released; cells found by the fence's address (an earlier implementation) then gave the new
  fence the old one's value, so work looked done at once and its results read as zero (about one read in 70).

### Frame tokens and presentation

- *observed* (public sample code shape): device slot 80 `WaitFrameEventX(type, timeout_ms, target, flags, token*)`.
  Titles call it with type 0, a zero timeout and flags 2, require success and keep the token; and with type 2, an
  infinite timeout and an earlier frame's token. *assumed*: flag bit 1 asks for the current frame's token without
  waiting; event 2 of a frame is reached once its present has been carried out. Implementation: tokens count up;
  presents signal a host fence with their token.
- *observed*: planes have the public `D3D12XBOX_PRESENT_PLANE_PARAMETERS` layout (token, resource count, resources,
  source rectangles, destination placement, colour space). Implementation: a flip-model swap chain on the
  title's window (the largest visible top-level window of the process); the first plane's display buffer is
  copied into it and presented with sync interval 0 (titles pace their frames with frame events, so the
  present does not also wait for the display). A present reaches "presented" (event 2) at the first boundary
  of the frame interval the title set (slot 78) after its GPU work is done, as a console flip happens at the
  interval; a thread carries presents out in order. Titles that wait for an earlier frame's present are so held
  to the interval (30 frames a second with a 33,333 µs interval).
- *observed*: device slot 81 takes `(frame token, type mask, count*, records*)` and returns an `HRESULT`: titles
  pass the token of a frame a few frames back, the mask `0x11100`, a count of 6 and room for six 56-byte records,
  and use the records only when the call succeeds with a nonzero count (dword 0 of a record is one bit of the
  mask, byte 8 a flag, bytes 0x18-0x1F a 64-bit value). *assumed*: timing statistics of a frame's events.
  Implementation: none are kept; the call succeeds with no records. Presents 1, 30 and 300 (and every `HOLY_CAPTURE_EVERY`th when set) are also written to
  `<data>/frames/present-<n>.bmp`. Partial: one plane; colour space and placement not applied.

### Resources

- *observed*: in the pipeline block of a native graphics pipeline description, bytes 16-23 list one DXGI format per
  render target from target 0 on, ended by a zero byte; in all 11,233 pipelines a title created, the list was
  contiguous, and dword 3 was 0 or 1 whatever its length (not the target count; meaning not established).
- *observed*: the render state block of a native graphics pipeline description comes in two layouts, told apart by
  where the constant 0x060201BC is: dword 0x1D (dword 1 is 4), or dword 0x32 (dword 1 is 6; 477 of 11,233
  pipelines), the later layout holding the same registers 0x15 dwords further on: the target mask (four bits a
  target) after the constant, the mode control (`PA_SU_SC_MODE_CNTL`) at 0x28, `DB_DEPTH_CONTROL` at 0x2E (values
  such as 0x7007F0: depth off; 0x7007E2: depth test GEQUAL; 0x7007A2: EQUAL; 0x7007E7: written, with stencil) and
  the blend controls from 0x34 (offsets for the first layout). Other layouts are refused. No field names the depth
  format, which the host needs in the pipeline: draws with a depth target bound use a variant of the pipeline with
  the depth test, writes and function of `DB_DEPTH_CONTROL` and the target's format (made at first such draw).
  The stencil test too: dwords 0x2F-0x31 are non-zero only where `DB_DEPTH_CONTROL` enables it, holding
  `DB_STENCIL_CONTROL` (operations of both faces) and `DB_STENCILREFMASK` of front and back faces (test and write
  masks; the reference is 0, titles set it with the command list's stencil reference). Partial: the desktop has one
  pair of masks for both faces (the front's are used) and no stencil operations ONES, REPLACE_OP or the logical
  ones (logged, kept).
- *measured* host behaviour: the host's vertex index (`SV_VertexID`) does not include a draw's first vertex (a point
  drawn from vertex 5 has index 0). *observed*: native vertex programs add s2 to their vertex index; draws without
  indices pass the first vertex there (indexed draws: the base vertex).
- *observed*: device slot 108 is `GetResourceAllocationInfo1`; titles retry without a small alignment when the
  alignment returned differs and treat sizes of 4 GiB or more as fatal.
- *observed*: titles set texture layout 0x105 on 3D textures (a console swizzle mode, *assumed*); console layouts
  and flag bits above the desktop's are dropped for the host. Depth textures may ask for unordered access, and
  packed 16-bit formats (`B5G6R5`, `B5G5R5A1`, `B4G4R4A4`) for typed unordered access; the desktop allows neither,
  so the first are made without unordered access, with a twin texture of the same format family that has it
  (unordered-access views are of the twin; a barrier into the unordered-access layout copies the depth texture into
  the twin, one out of it copies the twin back), and the second as `R8G8B8A8_UNORM` textures with matching views
  when the host GPU does not support them for the usage asked. Only the depth-only families (16 and 32 bits) have
  formats with unordered access; depth textures with stencil get no twin (logged). *measured* host behaviour:
  creating a texture of the 32/8/24 family with unordered access is an invalid call for which the host removes
  the device.
- *observed* host behaviour (development host): with the system's commit exhausted (about 0.1 GiB left), the host
  removes the device (`DXGI_ERROR_INVALID_CALL`) while titles create placed resources and the runtime opens heaps
  over title memory, with no command list in flight (device-removed data lists none) and after no particular call
  (`HOLY_D3D_CHECK`). Seen in three runs, each at the moment the system's commit ran out; not reproduced in
  isolation. Device removals in runs where the system's commit is nearly used up say nothing about the runtime.
- *observed*: the initial state titles pass to `CreatePlacedResourceX` for textures and to slot 127 is a barrier
  layout (3 and 6 seen, then named as the layout before the first barrier). Implementation: host textures are
  created in that layout (`CreateCommittedResource3`); each placement gets its own host texture.
- *observed*: slot 70 may be called with a description buffer the title left unwritten (dimension zero in the
  title's own texture record); a null view of the requested kind is made. The same is done, and logged, for
  descriptions no texture can have (sizes above 65,536, more than 17 levels, sample counts outside 1-16, formats
  past 255).
- *documented* (format numbers public in open-source texture tooling): the console adds DXGI formats 116 and 117
  (10:10:10:2 float, 7e3 and 6e4), 118 (16-bit depth with 8-bit stencil) with its depth (119) and stencil (120)
  views, and 190 (R4G4). Implementation: host resources, views and clear values use the nearest desktop format
  (16-bit float RGBA, the 24/8 depth family, R8G8); host textures never live in title memory, so only precision
  and size differ.
- *observed*: device slot 55 derives a pipeline from a base pipeline and 0x14C-byte entries: a type dword and a
  desktop `D3D12_DEPTH_STENCIL_DESC` (0), `D3D12_RASTERIZER_DESC` (1) or `D3D12_BLEND_DESC` (2). Type 5 (written
  next to depth-stencil entries with zero depth fields) is not established and not applied. Implementation:
  every graphics pipeline keeps its desktop pipeline stream ("recipe"), which derived pipelines copy and patch.
- *observed*: graphics pipelines from native code (slot 57) take a description of a root signature, (pointer,
  size) of a render-state block and of a pipeline block, then three stage slots of (code, code size, metadata,
  metadata size), each possibly zero: a slot can be empty before a used one (a pipeline with only a vertex program
  in the second slot seen). Stages come in no fixed order; the stage is told
  by the code (position exports: vertex; colour/depth/null exports or interpolation: pixel). The render-state
  block has been seen at 0x17C, 0x184 and 0x18C bytes with the same dwords up to the blend controls (dword 0x1E
  `CB_TARGET_MASK`, 0x28 `PA_SU_SC_MODE_CNTL`, 0x34-0x3B `CB_BLEND0-7_CONTROL`, public RDNA register layouts).
  Pipeline block dword 9 byte 2 holds the primitive topology type in bits 0-3; bit 4 is set in a few pipelines
  and not understood (not translated). A pipeline without a pixel program counts one render target of format 0
  (`DXGI_FORMAT_UNKNOWN`), which is not written. Stage metadata
  dword 3's high half is the number of 32-byte records of kind 0x41 that follow: one per resource binding the
  stage reads (dword 2 its offset in dwords in the descriptor table, dword 3 the register, dword 4 the view
  dimension). The pipeline block holds one byte-sized DXGI format per render target (bytes 16-23).
- *observed*: native graphics pipelines may carry a program with neither exports nor interpolation next to a
  vertex program (most of six instructions: depth-only passes; some read and write memory only). Implementation:
  it is the pixel program, writing no target; a pixel program without any export keeps its pixels (lanes are
  discarded only by programs that export). *observed*: pipeline block dword 3, the target count elsewhere, is
  0x10f or 0x15f in some pipelines with six formats in bytes 16-21; *assumed*: their targets are the leading
  formats (the field's meaning there is not established).

### Root arguments of native code

- *observed*: graphics stages read root arguments from user SGPRs starting at s8, packed in parameter order: a
  descriptor table as the low half of its GPU address (native code sets the high half to zero, so shader-visible
  heaps live below 4 GiB), constants as their values; the sampler heap's base follows (code indexes it with
  sampler indices times 16). Vertex shaders read 16-byte buffer descriptors of the bound vertex buffers from a
  table at s[0:1] (40-bit address) and add s2 to the vertex index. *assumed*: s3 holds the start instance;
  compute programs read the same packing from a root table at s[0:1].
- Implementation: shader-visible heaps get GPU addresses from 0x40000000; descriptor tables are 32 bytes per
  resource view and 16 per sampler. Per draw or dispatch of a translated pipeline, an upload ring holds the system
  constants (user SGPRs) and the vertex buffer table: system memory the host allocates (write-back, which the GPU
  caches; measured below), at title addresses of a range reserved below 2^40. The page table is memory of the same
  kind. Title memory is reached through one memory map
  for every call: each host heap over title memory has memory slots (raw views of 512 MiB of its buffer, in a block
  of 65536 descriptors past the title's own in every shader-visible host heap) and a page table maps each 64 KiB
  page of title addresses to a slot and offset (`shader/translate.h`). Heaps' pages enter the table when the heaps
  are opened (committed memory; physical pages: every title address showing storage the heap covers, kept current
  as the title maps and unmaps pages); descriptor heaps' GPU ranges when the heaps are made; buffers the bound tables, root views and vertex buffers name are located before the call if their page
  is not in the table yet. Partial: memory nothing has located (reached only through pointers held in memory)
  reads as zero. A host list takes the ring's memory in chunks and gives them back when it is reset (the GPU is
  then done with it), so a recording's root arguments stay its own however much is recorded after it and however
  often it is submitted (a sample check records 75 MiB past one dispatch).
- *measured* host behaviour (development host; a desktop compute program reading 16-byte elements, GB/s): the GPU
  does not keep memory of heaps opened over process memory (`OpenExistingHeapFromAddress`) or over a section
  (`OpenExistingHeapFromFileMapping`) in its second-level cache, while it does keep memory the host allocates:

  | Working set | Heap over process memory | Heap over a section | Host write-back heap | Upload heap | Video memory |
  | --- | --- | --- | --- | --- | --- |
  | 1 MiB, read once | 23 | 23 | 218 | 218 | 512 |
  | 16 MiB, read once | 10-14 | 10 | 165-197 | 186-189 | 1,928 |
  | 64-256 MiB, read once | 10-13 | 10-13 | 10-13 | 11-13 | 405-472 |
  | 4 KiB, read 64 times per thread | 2,675 | 2,682 | 4,145 | 4,145 | 4,161 |
  | 64 KiB, read 64 times per thread | 200 | 405 | 4,080 | 4,049 | 4,112 |

  Title memory reaches the GPU through heaps of the first two kinds, so whatever misses the first-level caches
  (vertices, structured data, tables beyond a few KiB) crosses the bus each time: about 20 times slower than video
  memory. Measured through the runtime (sample title): a native compute program and the same program in HLSL
  reading title memory take the same time (47 against 46-52 ms for 4 GiB of reads), so the memory, not the
  translation, sets the cost. Textures are not affected (host copies in video memory).
- Implementation (opt-in, `HOLY_BUFFER_COPIES` in MiB): pages that translated calls read through bound buffers
  (vertex buffers, buffers of descriptor tables, root views) get copies in video memory, and their page table
  entries name cache lines (`shader/translate.h`); a copy is made on a queue of the runtime's own the submitting
  queue waits for, and the page reads its copy from the next submission on. Title memory stays what is written:
  translated stores and atomics write it and flag the line, after which translated reads take title memory and
  the page leaves the cache; other GPU writes into title memory (copies, query results, immediate writes,
  unordered-access views and root views of desktop programs) take their pages out before the submission that
  records them; the CPU's writes are found by write watching before each submission (results shared with the
  texture scan, whose resets would hide them) and the copies refreshed first, and pages written in three of the
  last eight submissions leave. Memory the host does not watch (mapped views: physical pages) is not copied.
  *Measured* (sample title): a native program reading 256 times 16 bytes per thread over 1 MiB takes 0.39 ms
  from the copies against 0.87 ms from title memory (2.2 times), with the same results; the CPU's writes, a
  native store read back in the same program and a buffer copy into the memory are all seen. The translated
  access path (page table, cache line and flag reads, a guard per dword) is now the larger part of the cost.
- *measured* host behaviour (development host): memory a heap was opened over stays charged to the process after
  the title releases it, for as long as the heap lives (16 rounds of 64 MiB placed, released and freed left
  1,026 MiB charged). The runtime therefore lets go of heaps over title memory before the title's `VirtualFree`
  and `XMemFree` release it (`HolySetReleaseHook`); measured: no growth over the same rounds. Titles release memory
  once the GPU is done with it, as on the console. Host copies of textures filled from released memory leave the
  registry with it (a texture viewed there later is a new one, filled then). Their host textures, and the heaps
  over the memory, are let go of once the GPU finished all work submitted to any queue before the release (each
  queue's fence is signalled after every submission), as host objects must outlive the GPU's use of them; a title
  releasing memory the GPU still reads keeps it charged until then. *Measured* (sample title): with the heap
  released at once instead, the host did not remove the device either. A heap still recorded over
  memory whose allocation is gone or changed (released where the runtime does not hear of it) is let go of and
  opened again when an address in it is next located. Partial: memory released that way and allocated again at
  the same base is not told apart.
- *measured* host behaviour (development host): memory a heap was opened over (`OpenExistingHeapFromAddress`)
  refuses protection changes (`VirtualProtect` fails with error 87), and a heap cannot be opened over read-only
  memory (`E_INVALIDARG`): write tracking of title memory by page protection does not work where the GPU reaches it.
  Write watching does: a heap opens over memory reserved with `MEM_WRITE_WATCH`, and `GetWriteWatch` reports the
  CPU's and the kernel's (`ReadFile`) writes to it, heap or not (measured). xmem reserves title memory with write
  watching; host copies of textures in it are copied again before a submission when their pages were written
  (asked at most every 16 ms), and remapped physical pages, released memory and writes libraries announce mark them
  too. *measured* (development host): `GetWriteWatch` with reset costs about 0.55 us per MiB of the range asked
  about (1.2 ms over 2.25 GiB with 1 GiB touched), about 0.2 us for a small range, and the kernel touches the
  whole output array each call (room for a million pages: 0.5 ms); so textures are asked about stretch by
  stretch (gaps under 256 KiB joined), not over the span between the first and the last. *measured*: protection changes are refused on the view of a section a heap was opened over, but not on
  another view of the same section, and a write to a read-only view faults. The GPU reaches physical pages through
  views of its own, so textures in physical pages are watched through the title's views by page protection (the
  first write makes the page writable again and the texture dirty; kernel writes the runtime announces drop the
  watch first). Partial: CPU writes to memory titles reserve themselves and GPU writes under a texture are not
  seen.
- *measured* host behaviour: a buffer placed in a heap takes whole 64 KiB units of it, so title memory of less
  than 64 KiB cannot be reached by the GPU at all; `XMemAlloc` therefore takes whole 64 KiB units for page
  allocations (the title sees the size it asked for). Memory a title's own allocation hooks hand out is as the
  title made it (smaller pieces are logged).

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

### Speech synthesis

- *observed*: interface `{d722b373-8c4d-4692-9e51-d6ad9b37aa7d}` (queried through provider
  `{3e241977-6237-41e9-8559-102c2d9983f1}`: the two ids differ) is the documented `XSpeechSynthesizer` API, by
  the client library's thunks (slot, argument count): 8 `EnumerateInstalledVoices(context, callback)`,
  9 `Create(handle*)`, 10 `CloseHandle`, 11 `SetDefaultVoice`, 12 `SetCustomVoice(handle, voice id)`,
  13 `CreateStreamFromText(handle, text, stream*)`, 14 `CloseStreamHandle`, 15 `GetStreamDataSize(stream, size*)`,
  16 `GetStreamData(stream, size, buffer, used*)`, 21 `CreateStreamFromSsml(handle, ssml, stream*)`.
  *documented*: voice information `{description, display name, gender (0 female, 1 male), voice id, language
  (BCP-47)}`; the callback returns whether to continue; stream data is a WAVE container; an invalid stream
  handle gives `HRESULT_FROM_WIN32(ERROR_NOT_FOUND)`.
- Implementation: the host's speech engine (SAPI 5, part of Windows) enumerates voices and synthesizes
  16-bit mono PCM at 22.05 kHz into a WAVE container when a stream is created. Unknown voice ids and invalid
  synthesizer handles also give `ERROR_NOT_FOUND` (*assumed*). *observed* host behaviour: at low integrity (the
  sandbox) the host engine refuses to speak (`0x80045040`); synthesis then returns that error and no stream.
  Voice enumeration and synthesizers work in the sandbox.

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
- *documented* (public GameInput.h): readings (`IGameInputReading`, v0 order: 3 `GetInputKind`, 4
  `GetSequenceNumber`, 5 `GetTimestamp`, 6 `GetDevice`, 22 `GetGamepadState`), devices (`IGameInputDevice`: 3
  `GetDeviceInfo`, 4 `GetDeviceStatus`, 5 `GetBatteryState`, 10 `SetRumbleState`), the gamepad state (buttons, then
  triggers and sticks as floats), the device information layout, the device callback's arguments and the
  enumeration kinds (0 none, 1 asynchronous, 2 before the registration returns).
- Implementation: one gamepad, always connected: the host's first XInput controller, and the keyboard while a
  window of the process has the focus (mapping in [USAGE.md](USAGE.md)). Current readings are the latest state;
  a next reading exists when the state changed since the reference reading (states between polls are not
  kept). Device enumeration reports the gamepad; it never disconnects. Rumble goes to the XInput controller.
  Partial: reading callbacks register and never fire; keyboard and mouse readings report `0x838A0003`; one
  gamepad only.

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
  inflated by the project's decoder straight into the destination, Adler-32 checked), so they complete earlier
  than on the console; a status entry gets the first failure since the previous one. Partial: requests are
  processed one at a time on the calling thread; other request kinds and queue slots report themselves.

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
- *documented*: the native code is RDNA 2 (public AMD instruction set reference); decoding follows it. Operand
  124 is M0 and 125 NULL (an earlier version of the decoder had them swapped).
- *documented*: typed buffer loads use the RDNA 2 unified buffer formats (1-77: 8/16/32-bit, packed 10/11-bit and
  2-10-10-10 layouts, each as unorm, snorm, uscaled, sscaled, uint, sint and float where defined); absent
  components read as 0 and w as one.
- *observed*: compute programs may enable the thread group information SGPR (after the group ids): waves in the
  group in bits 0-5, the wave's index in bits 6-11 (*documented* layout in open-source drivers for the hardware).
- *documented*: host bytecode (DXBC, shader model 5.1) follows the desktop runtime's tokenized program format;
  a container needs input and output signature parts besides `SHEX`, and the DXBC checksum.
- Implementation, desktop programs: containers with bitcode are rebuilt as desktop containers (plain DXIL
  program, desktop parts kept, `RTS0` flags masked, digest recomputed) and run on the host.
- Implementation, native programs: the native code is translated straight into DXBC, without a
  shader compiler. One host thread runs one lane; scalar registers are recomputed per lane; lane masks keep the
  lane's own bit; control flow becomes a loop around a switch over block addresses; workgroup barriers leave
  the loop and synchronise at the top level. Memory is reached through the memory map: title GPU addresses
  (which equal CPU addresses) are looked up in a two-level page table (t0, space 8: a 1 GiB window, then the
  64 KiB page) giving a memory slot and offset, and accessed through the slot's raw view (u0-u65535, space 8). Scalar and vector ALU, scalar loads, buffer loads and stores
  (untyped, and typed for the 8/16/32-bit integer, normalised and float formats seen), global and flat
  memory, atomics and group shared memory (32 KiB, the host's limit), image loads, stores, samples and gathers
  (through the console-format descriptors in title memory: the host heap index in dword 0), cross-lane
  operations through group shared memory, f16 class tests and i16 compares are translated. Anything else
  reports itself.
- *documented* (public RDNA 2 buffer descriptor layout): word 3 holds the destination selects (bits 0-11, three
  bits a component: 0 zero, 1 one, 4-7 the loaded x-w) and the data format (bits 12-18).
  `buffer_load_format_*` take their format from there: titles' vertex programs fetch vertex data that way.
  Implementation: a switch over the format table picks component widths and numeric type, and one decoder
  handles every format (a decoder per format made programs too large for the host's shader compiler, which ran
  out of memory); format 0 and formats outside the table read as zero.
- *observed*: pixel programs use DPP `quad_perm` operands (the console compiler's derivatives: a value minus the
  quad's first lane's, then read from lanes 1 and 2, with `|x|` modifiers on both sources for widths).
  Implementation: a host pixel shader has no exchange memory, but fine derivatives carry a quad neighbour's value
  as a difference. Each value is exchanged as two floats holding 16 and 17 bits (the low half and the source
  lane's `EXEC` bit) so every step is exact (checked on the host GPU by a unit test that renders four quads);
  the quad position (console lane order: 0 top left, 1 top right, 2 bottom left, 3 bottom right) comes from
  `SV_Position`. Like implicit-derivative sampling this requires the four lanes of a quad to reach the
  instruction together. DPP source-1 negate/absolute modifiers are applied in every stage; `s_bfm_b64` is
  translated.
- *observed*: pixel programs also carry the console compiler's whole-wave reductions: `s_or_saveexec` with all
  lanes, the identity of the operation for lanes that were inactive, DPP `quad_perm`, `row_half_mirror` and
  `row_mirror` steps (fetching inactive lanes), `v_permlanex16_b32` and the operation once more, then
  `v_readlane_b32` of lane 31; and DPP8, row shifts, lane reads and permutes. Implementation: a pixel program's
  wave is taken to hold its quad alone, as lanes 0-3 (a packing the console GPU may also choose, so programs
  cannot rely on others). Reads from lanes in the quad are exact (through fine derivatives); a lane outside it has
  no pixel and reads as the identity of the operation consuming it where there is one (the reductions are then
  exact over the quad), as an invalid source for moves, and as the lane's own value for lane reads and permutes
  without such an operation. Vertex programs' wave is the lane alone.
- Implementation, lane masks: compute and graphics programs keep only the lane's own bit in lane masks (EXEC,
  VCC, compare results), each lane following its own control flow. `s_bcnt1` of such a mask counts 1,
  `v_mbcnt` of a mask held in a register counts 0 lanes below, and `v_readfirstlane_b32` reads the lane's own
  value: the wave of one lane the masks describe. That is consistent with the console compiler's wave-coalesced
  atomics (one atomic for the wave's active lanes, its result broadcast with `v_readfirstlane`, each lane's slot
  offset by `v_mbcnt` of EXEC) and makes scalarising loops finish. A constant mask counts by lane index
  (`v_mbcnt` of -1 is the lane's index). Data movement between lanes (DPP, lane reads and writes, permutes,
  `ds_swizzle`, `ds_bpermute`) uses real lanes in compute programs, through the exchange.
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
- *observed*: vertex programs of native graphics pipelines may be NGG (primitive) programs: they group vertex
  ids (`v_lshrrev_b32 vN, k, v5`: groups of 2^k), request output space with `s_sendmsg` 9 (`GS_ALLOC_REQ`),
  share data through group memory and export primitives to target 20, each as three 9-bit vertex indices
  within the group at bits 0, 10 and 20 with bit 31 marking a null primitive. *documented* (open-source drivers
  for the hardware): inputs `s2` the group information, `s3` the merged wave information (the wave's index in
  the group in bits 24-27), `v5` the vertex id, `v8` the instance id; user SGPRs from `s8`. Implementation: such
  a program is translated into a compute program, a group per vertex group and instance, that writes each
  vertex's position and parameters (a record of 16 bytes for each, in export order) and each primitive's three
  indices (null primitives degenerate); a generated vertex program reads the records for the host draw.
- *documented*: half-precision interpolation (`v_interp_p1ll_f16`, `v_interp_p1lv_f16`, `v_interp_p2_f16`) names
  the attribute in bits 0-5 of the source-0 field, the channel in bits 6-7 and the high half in bit 8.
  Implementation: an attribute read so holds pairs of halves; the pixel program takes each channel's halves as
  two interpolated inputs (`HALFPARAMn`, after the attributes read whole) that the generated NGG vertex program
  unpacks from the records. After a vertex program that is not NGG, the vertex program is translated for the
  pixel program that follows: it outputs the attributes read whole and, for those read as halves, each exported
  channel's halves as floats in the same registers.
- *documented* image operations: cube images take face coordinates and the face (open-source drivers form them
  from `v_cube*`: each minor axis over twice the major one, plus 1.5); the translation turns them back into a
  direction for a host cube view (cube arrays: slice 0 is read). Multisampled loads take the sample after x and y;
  `image_sample_d` takes the x then y gradients before the coordinates; unnormalised coordinates (texels) are
  divided by the size of level 0; addresses may be given as a list of registers (NSA encoding); D16 data packs
  two components a register. Partial: compare with gradients samples level 0; `image_get_lod` only in pixel
  programs; gathers with an explicit level read level 0.
- *documented*: programs may read data placed after their code through the program counter (`s_getpc_b64`, then
  scalar or buffer loads from it). *observed*: such tables also sit right after `s_endpgm`, before `s_code_end`.
  Implementation: the program's words are an immediate constant buffer of the translation and such loads read it;
  only instructions control flow reaches are translated (from the entry, following branches).
- *observed*: programs call subroutines: `s_getpc_b64` into an SGPR pair, `s_add_u32` of a literal to its low half,
  `s_swappc_b64` (the return address into another pair), and the subroutine returns with `s_setpc_b64`.
  Implementation: the addresses the idiom forms are blocks of the translation, as are return points; `s_setpc_b64`
  and `s_swappc_b64` jump through the block dispatcher. Partial: addresses formed otherwise are not found.
- *documented* (RDNA 2): image atomics (`image_atomic_*` 15-26: swap, compare-swap, add, sub, signed and unsigned
  min and max, and, or, xor) on 32-bit texels, the previous value returned with GLC. Implementation: typed
  atomics through integer UAV arrays over the same heap (spaces 21 and up). *documented*: with dmask 0x3 they
  work on 64-bit texels (the value in two registers, low dword first); *observed*: titles' pixel programs keep
  64-bit maxima (a depth above an id) that way. Host bytecode (shader model 5.1) has neither 64-bit atomics nor
  typed atomics on 64-bit texels: the texel's two 32-bit channels are read, combined and written, through a
  rasterizer-ordered view in pixel programs (exclusive and in primitive order per pixel: exact for lanes that
  access their own pixel, as observed), not atomically elsewhere. Containers then declare the features they need
  (raster-ordered views, typed UAV loads of additional formats) in an `SFI0` part. LDS D16 loads and stores (0xA0-0xA7:
  bytes and shorts into or from one half of a register), 96-bit LDS loads and stores, and `ds_append` /
  `ds_consume` (the dword at M0's base plus the offset, by one a lane: see lane masks) are translated, as are SDWA
  output modifiers (clamp, omod), the 16-bit pack conversions (`v_cvt_pknorm_*`, `v_cvt_pk_*16_*32`) and
  `v_pk_fmac_f16`.
- Implementation, translation cache: translations are stored in `<data>/translated/<translator build>` under a
  hash of the native code and the parameters that shape the translation, and reused by later runs; directories
  of other translator builds are deleted in the background when a run first uses the cache (they are never hit
  again). Translations of more than 256 KiB are compiled one at a time (*observed* host behaviour: the driver
  refuses some such programs with `E_OUTOFMEMORY` when several compile at once, and accepts them alone), with
  one retry. The private host cache is discarded only after such a refusal.
- *observed*: titles create pipelines by the tens of thousands while loading (over 80,000 in the first minutes,
  about 63,000 of them distinct once translated), and draw with a small part of them; creation is cheap on the
  console. *measured* on the development host: a compiled translated pipeline holds about 45 KB of driver
  memory, and compiling all of them takes minutes of processor time.
- Implementation, pipelines (`core/libs/d3d12_xs/pipeline_cache.h`): a title pipeline holds a recipe (the host
  stream, its programs as shared content-addressed programs; programs the translation cache holds are reloaded
  from it instead of being kept in memory). Title pipelines with equal content share one host pipeline, released
  with the last of them. A host pipeline is compiled when a title pipeline is first used, unless it was used in
  an earlier run (`<data>/pipelines-used.bin`, by content key): those compile on worker threads as soon as the
  title creates them. `HOLY_PIPELINES=eager` compiles every pipeline at creation, `lazy` only at first use.

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
- *observed*: computer slot 10 takes `(tile count*, packed mip info*, tile shape*, subresource tiling count*
  (in/out, 16 seen), first subresource tiling, subresource tilings*)`, the arguments of the desktop
  `ID3D12Device::GetResourceTiling` after its resource; titles stream tiled textures in 64 KiB pages.
  Implementation: the host's answer for a reserved resource of the same description (console flag bits
  dropped).

## Audio processor heap (`AcpHal.dll`)

- *observed* (call sites, how results are checked): `HRESULT ApuCreateHeap(uint32_t cached_bytes, uint32_t
  non_cached_bytes)`, called once at audio start-up (a failure is fatal to titles; a cached size of several hundred
  MiB and a non-cached one of about 2 MiB seen); `HRESULT ApuAlloc(void** address, uint32_t* apu_address,
  uint32_t bytes, uint32_t alignment, uint32_t flags)` (`apu_address` may be null; 2 KiB alignment seen; flags 1
  with large requests, 2 with a request that just fits the non-cached size: *assumed* 1 the cached heap, 2 the
  non-cached one); `HRESULT ApuFree(void* address)`; `bool ApuIsVirtualAddressValid(const void* address,
  uint32_t bytes)`; `uint32_t ApuMapVirtualAddress(const void* address)` (the 32-bit address the audio hardware
  uses) and `void* ApuMapApuAddress(uint32_t)`.
- Implementation: both heaps in one reservation, committed as handed out; an audio address is the offset in it
  plus a fixed base (never 0); first-fit allocation per heap; a range is valid within one allocation.
- *observed*: titles then call `HRESULT AcpHalAllocateShapeContexts(contexts*)`: six 32-bit context counts the
  title fills in, followed by six pointers the call writes (arrays of 64 bytes a context; titles clear them
  themselves); `AcpHalReleaseShapeContexts()`; `HRESULT AcpHalCreate(IAcpHal** out)`, whose object takes slot 0
  `(4 KiB, 4 KiB)` once, slot 2 `(command, cookie, 0xFFFFFFFF, payload, extra)` to send commands (a failure of
  the first is fatal to titles), and slots 1 and 5 when audio shuts down.
- *observed*: `xapu.dll`: `HRESULT XApuConnect(const config*, void** memory, handle*)` (six dwords
  `{1, 0x100, 2, size, 0, 1}`; dword 3 is the size of the memory block the call returns, which titles lay out as
  0x280-byte records followed by 0x1E00-byte buffers; a failure is fatal), `XApuEnqueueCommand(handle, 64-byte
  command, flags)`, `uint32_t XApuGetResultCount(handle, uint32_t)` and `XApuDequeueResult(handle, result*)`
  (titles dequeue as many results as counted), `XApuDisconnect(handle)`.
- *observed*: `chad.dll`: `HRESULT XDspConnectWithMaximumStreamLimit(memory, bytes, streams, 0x80, 1, handle*)`
  (a block of title memory, 0x60000 bytes and 30 streams seen; titles require exactly `S_OK` and stop
  otherwise), `XDspActivate(handle, ...)` (four arguments) and `XDspSubmitCommand(handle, command)`, both
  counted as successful when 0, `XDspDeactivate(handle)`, `XDspDisconnect(handle)`.
- Implementation: a silent audio device. Shape contexts and the processor's memory are audio processor memory;
  connections, activations and commands are accepted and not carried out (nothing is decoded, mixed or played),
  so the audio processor never produces results. Other control processor slots report themselves.
