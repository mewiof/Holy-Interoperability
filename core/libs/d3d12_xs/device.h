// Objects of the console D3D12 implementation.
#pragma once

#include <d3d12.h>
#include <windows.h>

#include <atomic>

#include "core/libs/d3d12_xs/bindings.h"
#include "core/libs/vtable.h"


namespace holy::d3d12 {

inline constexpr char kLibrary[] = "d3d12_xs.dll";
inline constexpr int kDeviceSlots = 160;
inline constexpr int kQueueSlots = 64;
inline constexpr int kCommandListSlots = 200;
inline constexpr int kResourceSlots = 32;

struct Device {
  void* const* vtable;
  const lib::VtableInfo* info;
  std::atomic<long> refs{1};
  ID3D12Device* host = nullptr;
  ID3D12RootSignature* translated_root = nullptr;  // shared by translated pipelines
};

struct CommandQueue {
  void* const* vtable;
  const lib::VtableInfo* info;
  std::atomic<long> refs{1};
  Device* device = nullptr;
  ID3D12CommandQueue* host = nullptr;
  int type = 0;
};

struct DescriptorHeap;

// Observed: titles read the console list object's fields at 0x18 and 0x30 as
// a command-buffer write cursor and limit: while the cursor is below the
// limit they write commands into that buffer themselves (advancing the
// cursor, and setting bytes at 0x140 and 0x141) instead of calling the
// method. The host model keeps both zero, so every command reaches a method,
// and keeps its own fields past a zeroed area the title may treat as its own.
inline constexpr size_t kTitleListArea = 0x400;

struct CommandList {
  void* const* vtable;
  uint8_t title_area[kTitleListArea - sizeof(void*)] = {};  // zero: cursor (0x18) == limit (0x30)
  const lib::VtableInfo* info;
  std::atomic<long> refs{1};
  Device* device = nullptr;
  ID3D12GraphicsCommandList* host = nullptr;
  ID3D12GraphicsCommandList2* host2 = nullptr;  // WriteBufferImmediate
  ID3D12GraphicsCommandList7* host7 = nullptr;  // enhanced barriers (null if the host has none)
  D3D12_COMMAND_LIST_TYPE type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  DescriptorHeap* heaps[2] = {};  // bound shader-visible heaps: CBV/SRV/UAV, sampler
  RootBinding graphics, compute;
  const void* pipeline = nullptr;  // the title's current pipeline (a Wrapper)
  bool translated = false;         // it runs translated native code
  D3D12_VERTEX_BUFFER_VIEW vertex_buffers[16] = {};  // title addresses
};

struct Resource {
  void* const* vtable;
  const lib::VtableInfo* info;
  std::atomic<long> refs{1};
  ID3D12Resource* host = nullptr;
  uint64_t title_va = 0;  // the resource's GPU address as the title sees it
  uint64_t host_va = 0;   // the same place in the host's GPU address space
  D3D12_RESOURCE_DESC desc{};
  void* runtime_memory = nullptr;  // XMemAlloc'd as the console runtime does
};

Device* create_device(const void* parameters);

// The root signature of pipelines translated from native code (every stage
// sees all of it), by parameter:
enum TranslatedRootParameter : uint32_t {
  kRootRegions = 0,   // descriptor table: region views
  kRootSystem = 1,    // root CBV: system constants (shader/translate.h)
  kRootImages = 2,    // descriptor table: the bound CBV/SRV/UAV heap from index 0
  kRootSamplers = 3,  // descriptor table: the bound sampler heap from index 0
  kTranslatedRootParameters = 4,
};
ID3D12RootSignature* translated_root_signature(Device* device);
// A title resource description as the host takes it: flag bits the desktop
// does not define are dropped, and texture layouts other than the desktop's
// (observed: titles pass 0x105 for some 3D textures; the value is assumed to
// name a console swizzle mode) become D3D12_TEXTURE_LAYOUT_UNKNOWN, since
// host textures are laid out by the host and filled by copies.
//
// Formats the desktop does not offer for the usage a title asks for (typed
// unordered access of the 16-bit packed formats) are replaced by a wider
// format with the same channels (8-bit RGBA). Host textures never live in
// title memory, so only precision differs; views of such textures use the
// replacement (host_view_format).
D3D12_RESOURCE_DESC desktop_resource_desc(const D3D12_RESOURCE_DESC& desc);
DXGI_FORMAT host_view_format(const void* resource, DXGI_FORMAT format);
// `state` is a legacy resource state, or with `layout` a barrier layout.
HRESULT create_committed_resource(Device* device, const D3D12_HEAP_PROPERTIES* properties, D3D12_HEAP_FLAGS flags,
                                  const D3D12_RESOURCE_DESC* desc, UINT state, bool layout,
                                  const D3D12_CLEAR_VALUE* clear, void** out);
// A host committed resource in `initial` layout (textures) through the
// desktop CreateCommittedResource3; layouts the desktop does not have become
// the common layout.
HRESULT create_host_committed(Device* device, const D3D12_HEAP_PROPERTIES& heap, D3D12_HEAP_FLAGS flags,
                              const D3D12_RESOURCE_DESC& desc, UINT initial, const D3D12_CLEAR_VALUE* clear,
                              ID3D12Resource** out);
HRESULT create_placed_resource_x(Device* device, uint64_t address, const D3D12_RESOURCE_DESC* desc,
                                 D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear, void** out,
                                 const void* caller);

// The console IDXGIDevice a title obtains from the device.
void* dxgi_device();
HRESULT create_command_list(Device* device, const uint32_t* desc, void* allocator, void* initial_state, void** out);

// `console_flags`: the descriptor's flags field has console meaning and is
// not passed to the host.
HRESULT create_command_queue(Device* device, const D3D12_COMMAND_QUEUE_DESC* desc, bool console_flags,
                             void** out);

}  // namespace holy::d3d12
