// The console D3D12 device, backed by a host Direct3D 12 device.
//
// Observed: D3D12XboxCreateDevice(adapter, const parameters*, REFIID, void**)
// is called with a null adapter, a 0x40-byte parameter block whose first
// dword is 0x20004 (a version), and the console device's interface id
// {5405c344-d457-444e-b4dd-2366e45aee39}. The device's vtable only partly
// follows the desktop ID3D12Device order (slot 8 creates a command queue as on
// the desktop; slot 48 also creates one, unlike the desktop), so every slot is
// established from the title's calls; unbound slots report themselves.
//
// Observed calling convention: the C one for interface methods. Structures of
// up to eight bytes come back in rax; larger ones through a result pointer
// passed first, with the object second. All other methods take the object
// first.
#include "core/libs/d3d12_xs/device.h"

#include <d3d12.h>

#include <intrin.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include "core/libs/d3d12_xs/descriptors.h"
#include "core/libs/d3d12_xs/forward.h"
#include "core/libs/d3d12_xs/frame_pacing.h"
#include "core/libs/d3d12_xs/shader_container.h"
#include "core/libs/d3d12_xs/shader_dump.h"
#include "core/libs/d3d12_xs/views.h"
#include "core/libs/support.h"

namespace holy::d3d12 {
namespace {

const lib::VtableInfo kDeviceInfo{kLibrary, "ID3D12DeviceX"};

// {5405c344-d457-444e-b4dd-2366e45aee39}, observed as the device's id.
constexpr GUID kDeviceIid = {0x5405c344, 0xd457, 0x444e, {0xb4, 0xdd, 0x23, 0x66, 0xe4, 0x5a, 0xee, 0x39}};

// Only the device's own interface is provided. Observed: titles also ask the
// device for IDXGIDevice and walk adapter and outputs through it; the console
// DXGI objects do not keep the desktop method order (an output's display
// mode list is at slot 21, with 32-byte entries), so they are not provided
// yet and the query reports itself.
HRESULT STDMETHODCALLTYPE device_query_interface(Device* self, REFIID iid, void** out) {
  if (!out) return E_POINTER;
  *out = nullptr;
  // {54ec77fa-1377-44e6-8c32-88fd5f44c84c}
  constexpr GUID kDxgiDevice = {0x54ec77fa, 0x1377, 0x44e6, {0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x44, 0xc8, 0x4c}};
  if (IsEqualGUID(iid, kDxgiDevice)) {
    *out = dxgi_device();
    return S_OK;
  }
  if (IsEqualGUID(iid, kDeviceIid) || IsEqualGUID(iid, __uuidof(IUnknown))) {
    self->refs.fetch_add(1);
    *out = self;
    return S_OK;
  }
  char symbol[96];
  std::snprintf(symbol, sizeof(symbol), "ID3D12DeviceX::QueryInterface(%08lx-%04x-%04x)", iid.Data1, iid.Data2,
                iid.Data3);
  HolyUnimplementedFrom(kLibrary, symbol, _ReturnAddress());
  return E_NOINTERFACE;
}
ULONG STDMETHODCALLTYPE device_add_ref(Device* self) {
  return static_cast<ULONG>(self->refs.fetch_add(1) + 1);
}
ULONG STDMETHODCALLTYPE device_release(Device* self) {
  // The device lives as long as the process: titles keep raw pointers to it
  // in many places, and the console never destroys it during a run.
  return static_cast<ULONG>(self->refs.fetch_sub(1) - 1);
}

// Slot 26 (observed: called with node mask 1 and heap types 1, 2 and 3, the
// result pointer first): GetCustomHeapProperties, as on the desktop.
D3D12_HEAP_PROPERTIES* STDMETHODCALLTYPE get_custom_heap_properties(D3D12_HEAP_PROPERTIES* result, Device* self,
                                                                   UINT node_mask, D3D12_HEAP_TYPE type) {
  *result = self->host->GetCustomHeapProperties(node_mask, type);
  return result;
}

HRESULT STDMETHODCALLTYPE create_queue(Device* self, const D3D12_COMMAND_QUEUE_DESC* desc, REFIID, void** queue) {
  return create_command_queue(self, desc, false, queue);
}

HRESULT STDMETHODCALLTYPE create_queue_x(Device* self, const D3D12_COMMAND_QUEUE_DESC* desc, REFIID, void** queue) {
  return create_command_queue(self, desc, true, queue);
}

// --- fences --------------------------------------------------------------------
//
// Observed: device slot 36 is called as CreateFence(initial value, flags,
// riid, out) with the desktop ID3D12Fence interface id; fences keep the
// desktop ID3D12Fence1 method order and forward to host fences. Flags are not
// passed on (titles ask for shared fences, which a single process does not
// need).
const lib::VtableInfo kFenceInfo{kLibrary, "ID3D12FenceX"};
Device* g_device = nullptr;

HRESULT STDMETHODCALLTYPE get_device_of_child(Wrapper*, REFIID, void** device) {
  if (!device) return E_POINTER;
  g_device->refs.fetch_add(1);
  *device = g_device;
  return S_OK;
}

const auto kFenceVtable = [] {
  auto table = make_forwarding_vtable(12);
  table[7] = reinterpret_cast<void*>(&get_device_of_child);
  return table;
}();

HRESULT STDMETHODCALLTYPE create_fence(Device* self, UINT64 initial, D3D12_FENCE_FLAGS flags, REFIID, void** fence) {
  if (!fence) return E_POINTER;
  *fence = nullptr;
  ID3D12Fence* host = nullptr;
  const HRESULT hr = self->host->CreateFence(initial, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&host));
  (void)flags;
  if (FAILED(hr)) return hr;
  *fence = wrap(host, &kFenceInfo, kFenceVtable.data());
  return S_OK;
}

// --- command allocators ----------------------------------------------------------
//
// Observed: device slot 9 is CreateCommandAllocator(type, riid, out), as on
// the desktop; allocators keep the desktop order (slot 8 Reset).
const lib::VtableInfo kAllocatorInfo{kLibrary, "ID3D12CommandAllocatorX"};

const auto kAllocatorVtable = [] {
  auto table = make_forwarding_vtable(9);
  table[7] = reinterpret_cast<void*>(&get_device_of_child);
  return table;
}();

HRESULT STDMETHODCALLTYPE create_command_allocator(Device* self, D3D12_COMMAND_LIST_TYPE type, REFIID,
                                                   void** allocator) {
  if (!allocator) return E_POINTER;
  *allocator = nullptr;
  ID3D12CommandAllocator* host = nullptr;
  const HRESULT hr = self->host->CreateCommandAllocator(type, IID_PPV_ARGS(&host));
  if (FAILED(hr)) return hr;
  *allocator = wrap(host, &kAllocatorInfo, kAllocatorVtable.data());
  return S_OK;
}

HRESULT STDMETHODCALLTYPE create_command_list_x(Device* self, const uint32_t* desc, void* allocator,
                                                void* initial_state, REFIID, void** list) {
  return create_command_list(self, desc, allocator, initial_state, list);
}

HRESULT STDMETHODCALLTYPE set_frame_interval_x(Device*, void*, UINT length_us, UINT period, UINT flags) {
  return set_frame_interval(length_us, period, flags);
}

HRESULT STDMETHODCALLTYPE schedule_frame_event_x(Device*, UINT type, UINT64 offset_us, void*, UINT flags) {
  return schedule_frame_event(type, offset_us, flags);
}

// Slot 15: GetDescriptorHandleIncrementSize(type), as on the desktop.
UINT STDMETHODCALLTYPE get_descriptor_handle_increment_size(Device* self, D3D12_DESCRIPTOR_HEAP_TYPE type) {
  return self->host->GetDescriptorHandleIncrementSize(type);
}

HRESULT STDMETHODCALLTYPE create_placed_resource(Device* self, D3D12_GPU_VIRTUAL_ADDRESS address,
                                                 const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES state,
                                                 const D3D12_CLEAR_VALUE* clear, REFIID, void** resource) {
  return create_placed_resource_x(self, address, desc, state, clear, resource, _ReturnAddress());
}

HRESULT STDMETHODCALLTYPE create_descriptor_heap_x(Device* self, const D3D12_DESCRIPTOR_HEAP_DESC* desc, REFIID,
                                                   void** heap) {
  return create_descriptor_heap(self, desc, heap);
}

// Slot 25: GetResourceAllocationInfo(visible_mask, count, descs), as on the
// desktop; the 16-byte result comes back through the result pointer. Flag bits
// the desktop does not define are dropped.
D3D12_RESOURCE_ALLOCATION_INFO* STDMETHODCALLTYPE get_resource_allocation_info(
    D3D12_RESOURCE_ALLOCATION_INFO* result, Device* self, UINT, UINT count, const D3D12_RESOURCE_DESC* descs) {
  std::vector<D3D12_RESOURCE_DESC> host(descs, descs + count);
  for (D3D12_RESOURCE_DESC& desc : host) desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(desc.Flags & 0x1FF);
  *result = self->host->GetResourceAllocationInfo(0, count, host.data());
  if (count && HolyLogEnabled(lib::kDebug)) {
    LIB_LOG(lib::kDebug, "d3d12", "allocation info dim %d %llux%u fmt %d mips %u flags 0x%x -> 0x%llx align 0x%llx",
            descs[0].Dimension, static_cast<unsigned long long>(descs[0].Width), descs[0].Height, descs[0].Format,
            descs[0].MipLevels, descs[0].Flags, static_cast<unsigned long long>(result->SizeInBytes),
            static_cast<unsigned long long>(result->Alignment));
  }
  return result;
}

HRESULT STDMETHODCALLTYPE create_committed_resource_x(Device* self, const D3D12_HEAP_PROPERTIES* properties,
                                                      D3D12_HEAP_FLAGS flags, const D3D12_RESOURCE_DESC* desc,
                                                      UINT state, const D3D12_CLEAR_VALUE* clear, void*, UINT,
                                                      const void*, REFIID, void** resource) {
  return create_committed_resource(self, properties, flags, desc, state, clear, resource);
}

// --- root signatures -----------------------------------------------------------------
//
// Observed: device slot 16 is CreateRootSignature(node mask, blob, size, riid,
// out), as on the desktop, with blobs from D3D12SerializeVersionedRootSignature.
const lib::VtableInfo kRootSignatureInfo{kLibrary, "ID3D12RootSignatureX"};

const auto kRootSignatureVtable = [] {
  auto table = make_forwarding_vtable(7);
  table[7] = reinterpret_cast<void*>(&get_device_of_child);
  return table;
}();

HRESULT STDMETHODCALLTYPE create_root_signature(Device* self, UINT, const void* blob, SIZE_T size, REFIID,
                                                void** signature) {
  if (!signature) return E_POINTER;
  *signature = nullptr;
  ID3D12RootSignature* host = nullptr;
  const HRESULT hr = self->host->CreateRootSignature(0, blob, size, IID_PPV_ARGS(&host));
  if (FAILED(hr)) {
    LIB_LOG(lib::kError, "d3d12", "root signature of %llu bytes refused by the host (0x%08lX)",
            static_cast<unsigned long long>(size), static_cast<unsigned long>(hr));
    return hr;
  }
  *signature = wrap(host, &kRootSignatureInfo, kRootSignatureVtable.data());
  return S_OK;
}

// --- pipeline states ------------------------------------------------------------------
const lib::VtableInfo kPipelineInfo{kLibrary, "ID3D12PipelineStateX"};

const auto kPipelineVtable = [] {
  auto table = make_forwarding_vtable(9);
  table[7] = reinterpret_cast<void*>(&get_device_of_child);
  return table;
}();

// Observed: device slot 62 takes a D3D12_COMPUTE_PIPELINE_STATE_DESC, two
// null pointers, the interface id and the out pointer. Shaders that keep
// their DXIL run from a rebuilt desktop container; shaders that carry only
// native GPU code are not supported yet and report themselves.
HRESULT STDMETHODCALLTYPE create_compute_pipeline_state_x(Device* self, const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc,
                                                          void*, void*, REFIID, void** pipeline) {
  if (!pipeline || !desc) return E_POINTER;
  *pipeline = nullptr;
  std::vector<uint8_t> shader;
  const ShaderFormat format = rebuild_desktop_container(desc->CS.pShaderBytecode, desc->CS.BytecodeLength, shader);
  if (format == ShaderFormat::NativeOnly || format == ShaderFormat::Unknown) {
    dump_shader(desc->CS.pShaderBytecode, desc->CS.BytecodeLength, "compute");
    HolyUnimplementedFrom(kLibrary, "compute pipeline with a native-only shader", _ReturnAddress());
    return E_NOTIMPL;
  }
  D3D12_COMPUTE_PIPELINE_STATE_DESC host = *desc;
  host.pRootSignature = host_of<ID3D12RootSignature>(desc->pRootSignature);
  host.CS = {shader.data(), shader.size()};
  host.NodeMask = 0;
  host.CachedPSO = {};
  host.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
  ID3D12PipelineState* state = nullptr;
  const HRESULT hr = self->host->CreateComputePipelineState(&host, IID_PPV_ARGS(&state));
  if (FAILED(hr)) {
    dump_shader(desc->CS.pShaderBytecode, desc->CS.BytecodeLength, "compute");
    LIB_LOG(lib::kError, "d3d12", "the host refused a compute pipeline (0x%08lX)", static_cast<unsigned long>(hr));
    return hr;
  }
  *pipeline = wrap(state, &kPipelineInfo, kPipelineVtable.data());
  return S_OK;
}

const auto kDeviceVtable = [] {
  auto table = lib::make_reporting_vtable<kDeviceSlots>();
  table[0] = reinterpret_cast<void*>(&device_query_interface);
  table[1] = reinterpret_cast<void*>(&device_add_ref);
  table[2] = reinterpret_cast<void*>(&device_release);
  table[8] = reinterpret_cast<void*>(&create_queue);
  table[9] = reinterpret_cast<void*>(&create_command_allocator);
  table[14] = reinterpret_cast<void*>(&create_descriptor_heap_x);
  table[15] = reinterpret_cast<void*>(&get_descriptor_handle_increment_size);
  table[16] = reinterpret_cast<void*>(&create_root_signature);
  table[17] = reinterpret_cast<void*>(&create_constant_buffer_view);
  table[18] = reinterpret_cast<void*>(&create_shader_resource_view);
  table[19] = reinterpret_cast<void*>(&create_unordered_access_view);
  table[20] = reinterpret_cast<void*>(&create_render_target_view);
  table[21] = reinterpret_cast<void*>(&create_depth_stencil_view);
  table[22] = reinterpret_cast<void*>(&create_sampler);
  table[23] = reinterpret_cast<void*>(&copy_descriptors);
  table[24] = reinterpret_cast<void*>(&copy_descriptors_simple);
  table[25] = reinterpret_cast<void*>(&get_resource_allocation_info);
  table[26] = reinterpret_cast<void*>(&get_custom_heap_properties);
  table[36] = reinterpret_cast<void*>(&create_fence);
  table[48] = reinterpret_cast<void*>(&create_queue_x);
  table[62] = reinterpret_cast<void*>(&create_compute_pipeline_state_x);
  table[70] = reinterpret_cast<void*>(&create_shader_resource_view_at);
  table[73] = reinterpret_cast<void*>(&create_command_list_x);
  table[78] = reinterpret_cast<void*>(&set_frame_interval_x);
  table[79] = reinterpret_cast<void*>(&schedule_frame_event_x);
  table[127] = reinterpret_cast<void*>(&create_committed_resource_x);
  table[133] = reinterpret_cast<void*>(&create_placed_resource);
  return table;
}();

}  // namespace

Device* create_device(const void* parameters) {
  uint32_t words[16] = {};
  if (parameters) std::memcpy(words, parameters, sizeof(words));
  LIB_LOG(lib::kInfo, "d3d12",
          "device parameters: %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x",
          words[0], words[1], words[2], words[3], words[4], words[5], words[6], words[7], words[8], words[9],
          words[10], words[11], words[12], words[13], words[14], words[15]);

  ID3D12Device* host = nullptr;
  const HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&host));
  if (FAILED(hr)) {
    LIB_LOG(lib::kError, "d3d12", "the host has no Direct3D 12 device (0x%08lX)", static_cast<unsigned long>(hr));
    return nullptr;
  }
  auto* device = new Device();
  device->vtable = kDeviceVtable.data();
  device->info = &kDeviceInfo;
  device->host = host;
  g_device = device;
  LIB_LOG(lib::kInfo, "d3d12", "host Direct3D 12 device created");
  return device;
}

}  // namespace holy::d3d12
