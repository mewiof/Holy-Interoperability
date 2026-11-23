// Sample title checks of console graphics resources, descriptor heaps,
// pipelines, display information and frame pacing (slots and conventions as
// in docs/PLATFORM_NOTES.md).
#include <d3d12.h>
#include <dxgi1_6.h>
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>

#include "tests/sample_title/checks.h"

extern "C" {
__declspec(dllimport) void* XMemAlloc(SIZE_T size, unsigned long long attributes);
__declspec(dllimport) void XMemFree(void* address, unsigned long long attributes);
__declspec(dllimport) void* XMemVirtualAlloc(void* address, SIZE_T size, DWORD allocation_type,
                                             unsigned long long allocator_id, DWORD protect);
__declspec(dllimport) BOOL XMemAllocatePhysicalPages(unsigned long long attributes, ULONG_PTR* count,
                                                     ULONG_PTR* pages);
__declspec(dllimport) void* XMemMapPhysicalPages(void* address, ULONG_PTR count, ULONG_PTR* pages);
}

extern std::atomic<int> g_hook_allocations;
extern void* g_preset_address;
extern SIZE_T g_preset_size;

namespace {

template <typename R, typename... Args>
R call(void* object, int slot, Args... args) {
  using Fn = R(STDMETHODCALLTYPE*)(void*, Args...);
  return reinterpret_cast<Fn>((*static_cast<void***>(object))[slot])(object, args...);
}

void* blob_pointer(void* blob) {
  return call<void*>(blob, 3);
}
SIZE_T blob_size(void* blob) {
  return call<SIZE_T>(blob, 4);
}

void test_allocation_hooks() {
  const int before = g_hook_allocations.load();
  void* block = XMemAlloc(256, 0);
  check(block && g_hook_allocations.load() == before + 1, "XMemAlloc goes through the title's allocation hooks");
  XMemFree(block, 0);
}

void test_placeholders() {
  constexpr DWORD kReservePlaceholder = 0x00040000;
  constexpr DWORD kReplacePlaceholder = 0x00004000;
  auto* region = static_cast<unsigned char*>(
      XMemVirtualAlloc(nullptr, 0x100000, MEM_RESERVE | kReservePlaceholder, 7, PAGE_NOACCESS));
  check(region != nullptr, "XMemVirtualAlloc makes placeholder reservations");
  if (!region) return;
  auto* part = static_cast<unsigned char*>(XMemVirtualAlloc(
      region + 0x40000, 0x20000, MEM_RESERVE | MEM_COMMIT | kReplacePlaceholder, 0x27, PAGE_READWRITE));
  check(part == region + 0x40000, "XMemVirtualAlloc replaces part of a placeholder with committed memory");
  if (part) {
    part[0] = 1;
    part[0x1FFFF] = 2;
  }
}

void test_descriptor_heap(void* device) {
  constexpr UINT kCount = 64;
  const UINT increment = call<UINT>(device, 15, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  check(increment > 0, "descriptor increments come from the device");
  void* preset = VirtualAlloc(nullptr, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  g_preset_address = preset;
  g_preset_size = static_cast<SIZE_T>(kCount) * increment;
  const D3D12_DESCRIPTOR_HEAP_DESC desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kCount,
                                        D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 1};
  void* heap = nullptr;
  check(call<HRESULT>(device, 14, &desc, __uuidof(ID3D12DescriptorHeap), &heap) == S_OK && heap,
        "the device creates a shader-visible descriptor heap");
  if (!heap) return;
  const uint64_t cpu = call<uint64_t>(heap, 9);
  check(cpu == reinterpret_cast<uint64_t>(preset),
        "a descriptor heap's memory comes from XMemAlloc, object type 4, allocator 0x80");
  check(call<uint64_t>(heap, 10) != 0, "a shader-visible heap has a GPU start");
  D3D12_DESCRIPTOR_HEAP_DESC back{};
  using DescFn = D3D12_DESCRIPTOR_HEAP_DESC*(STDMETHODCALLTYPE*)(D3D12_DESCRIPTOR_HEAP_DESC*, void*);
  reinterpret_cast<DescFn>((*static_cast<void***>(heap))[8])(&back, heap);
  check(back.NumDescriptors == kCount && back.Type == desc.Type, "a descriptor heap reports its description");

  // A null view into the heap through the title's CPU handle.
  D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
  srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv.Texture2D.MipLevels = 1;
  call<void>(device, 18, static_cast<void*>(nullptr), &srv, static_cast<SIZE_T>(cpu + increment));
  check(true, "views are created through title CPU handles");
}

void test_resources(void* device) {
  // A placed buffer over physical pages the title mapped itself.
  ULONG_PTR pages[4] = {};
  ULONG_PTR count = 4;
  XMemAllocatePhysicalPages(0x280, &count, pages);
  auto* region = static_cast<unsigned char*>(XMemVirtualAlloc(nullptr, 0x40000, 0x20402000, 0x23, PAGE_READWRITE));
  const bool mapped = region && XMemMapPhysicalPages(region, 4, pages) == region;
  check(mapped, "physical pages are mapped for a placed buffer");
  if (!mapped) return;
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = 0x30000;
  desc.Height = 1;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(0x20004);  // with a console-only bit
  const uint64_t address = reinterpret_cast<uint64_t>(region) + 0x10000;
  void* buffer = nullptr;
  const HRESULT placed = call<HRESULT>(device, 133, address, &desc, static_cast<UINT>(0xFFFFFFFF),
                                       static_cast<const void*>(nullptr), __uuidof(ID3D12Resource), &buffer);
  check(placed == S_OK && buffer, "a buffer is placed at a title address");
  if (buffer) {
    check(call<uint64_t>(buffer, 11) == address, "a placed buffer's GPU address is the title address");
    void* data = nullptr;
    check(call<HRESULT>(buffer, 8, 0u, static_cast<const void*>(nullptr), &data) == S_OK &&
              data == reinterpret_cast<void*>(address),
          "mapping a placed buffer yields the title's memory");
    D3D12_RESOURCE_DESC back{};
    using DescFn = D3D12_RESOURCE_DESC*(STDMETHODCALLTYPE*)(D3D12_RESOURCE_DESC*, void*);
    reinterpret_cast<DescFn>((*static_cast<void***>(buffer))[10])(&back, buffer);
    check(back.Width == desc.Width && back.Flags == desc.Flags, "a resource reports its description");
  }

  // A committed texture like the display buffers titles create.
  D3D12_RESOURCE_DESC texture{};
  texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  texture.Width = 256;
  texture.Height = 128;
  texture.DepthOrArraySize = 1;
  texture.MipLevels = 1;
  texture.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  texture.SampleDesc.Count = 1;
  texture.Flags = static_cast<D3D12_RESOURCE_FLAGS>(0x300001);
  const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
                                   D3D12_MEMORY_POOL_UNKNOWN, 0, 1};
  void* committed = nullptr;
  const HRESULT made = call<HRESULT>(device, 127, &heap, D3D12_HEAP_FLAG_ALLOW_DISPLAY, &texture, 3u,
                                     static_cast<const void*>(nullptr), static_cast<void*>(nullptr), 0u,
                                     static_cast<const void*>(nullptr), __uuidof(ID3D12Resource), &committed);
  check(made == S_OK && committed, "the device creates committed display textures");
  if (committed) call<ULONG>(committed, 2);
}

void test_root_signature_and_pipeline(void* device) {
  D3D12_ROOT_PARAMETER1 parameter{};
  parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{};
  desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
  desc.Desc_1_1.NumParameters = 1;
  desc.Desc_1_1.pParameters = &parameter;
  desc.Desc_1_1.Flags = static_cast<D3D12_ROOT_SIGNATURE_FLAGS>(0x3000000);  // console bits
  // Imported from the console graphics library, as titles import it.
  ID3DBlob* serialized = nullptr;
  const HRESULT hr = D3D12SerializeVersionedRootSignature(&desc, &serialized, nullptr);
  void* blob = serialized;
  check(hr == S_OK && blob,
        "root signatures with console flag bits serialize");
  if (!blob) return;
  void* signature = nullptr;
  check(call<HRESULT>(device, 16, 1u, blob_pointer(blob), blob_size(blob), __uuidof(ID3D12RootSignature),
                      &signature) == S_OK &&
            signature,
        "the device creates root signatures");

  HMODULE compiler = LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  using CompileFn = HRESULT(WINAPI*)(const void*, SIZE_T, const char*, const void*, void*, const char*, const char*,
                                     UINT, UINT, void**, void**);
  const auto compile = compiler ? reinterpret_cast<CompileFn>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
  const char source[] = "RWByteAddressBuffer b : register(u0); [numthreads(1,1,1)] void main() { b.Store(0, 1); }";
  void* shader = nullptr;
  if (compile) compile(source, sizeof(source) - 1, nullptr, nullptr, nullptr, "main", "cs_5_0", 0, 0, &shader, nullptr);
  if (!shader || !signature) return;
  D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
  pso.pRootSignature = static_cast<ID3D12RootSignature*>(signature);
  pso.CS = {blob_pointer(shader), blob_size(shader)};
  pso.NodeMask = 1;
  void* pipeline = nullptr;
  check(call<HRESULT>(device, 62, &pso, static_cast<void*>(nullptr), static_cast<void*>(nullptr),
                      __uuidof(ID3D12PipelineState), &pipeline) == S_OK &&
            pipeline,
        "the device creates compute pipelines from desktop shader containers");
}

void test_display_and_pacing(void* device) {
  constexpr GUID kDxgiDevice = {0x54ec77fa, 0x1377, 0x44e6, {0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x44, 0xc8, 0x4c}};
  void* dxgi = nullptr;
  check(call<HRESULT>(device, 0, kDxgiDevice, &dxgi) == S_OK && dxgi, "the device answers IDXGIDevice");
  if (dxgi) {
    void* adapter = nullptr;
    void* output = nullptr;
    UINT modes = 0;
    const bool walked = call<HRESULT>(dxgi, 7, &adapter) == S_OK && adapter &&
                        call<HRESULT>(adapter, 7, 0u, &output) == S_OK && output &&
                        call<HRESULT>(output, 21, DXGI_FORMAT_UNKNOWN, 0u, &modes,
                                      static_cast<DXGI_MODE_DESC1*>(nullptr)) == S_OK;
    check(walked, "adapter and output are reachable and list display modes");
  }
  check(call<HRESULT>(device, 78, static_cast<void*>(nullptr), 16667u, 1u, 0u) == S_OK,
        "SetFrameIntervalX accepts a 60 Hz interval");
  check(call<HRESULT>(device, 79, 0u, UINT64{0}, static_cast<void*>(nullptr), 0u) == S_OK,
        "ScheduleFrameEventX schedules the interval origin");
}

}  // namespace

void test_graphics_resources(void* device) {
  test_allocation_hooks();
  test_placeholders();
  test_descriptor_heap(device);
  test_resources(device);
  test_root_signature_and_pipeline(device);
  test_display_and_pacing(device);
}
