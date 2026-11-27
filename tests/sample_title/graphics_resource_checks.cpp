// Sample title checks of console graphics resources, descriptor heaps,
// pipelines, display information and frame pacing (slots and conventions as
// in docs/PLATFORM_NOTES.md).
#include <d3d12.h>
#include <dxgi1_6.h>
#include <windows.h>
#include <psapi.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "tests/sample_title/checks.h"

extern "C" {
__declspec(dllimport) void* XMemAlloc(SIZE_T size, unsigned long long attributes);
__declspec(dllimport) void XMemFree(void* address, unsigned long long attributes);
__declspec(dllimport) void* XMemVirtualAlloc(void* address, SIZE_T size, DWORD allocation_type,
                                             unsigned long long allocator_id, DWORD protect);
__declspec(dllimport) BOOL XMemAllocatePhysicalPages(unsigned long long attributes, ULONG_PTR* count,
                                                     ULONG_PTR* pages);
__declspec(dllimport) void* XMemMapPhysicalPages(void* address, ULONG_PTR count, ULONG_PTR* pages);
__declspec(dllimport) BOOL XMemFreePhysicalPages(ULONG_PTR count, ULONG_PTR* pages);
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
  const uint64_t gpu = call<uint64_t>(heap, 10);
  check(gpu != 0 && gpu + static_cast<uint64_t>(kCount) * increment <= 0x100000000ull,
        "a shader-visible heap's GPU range lies below 4 GiB, as native code addresses it");
  check(increment == 32 && call<UINT>(device, 15, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER) == 16,
        "resource descriptors take 32 bytes and samplers 16, as native code indexes them");
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
    // GetDesc with the result pointer first, and in the member function order
    // (the object first) that titles' C++ code uses; the object must survive
    // both.
    D3D12_RESOURCE_DESC back{}, member{};
    using DescFn = D3D12_RESOURCE_DESC*(STDMETHODCALLTYPE*)(void*, void*);
    const auto get_desc = reinterpret_cast<DescFn>((*static_cast<void***>(buffer))[10]);
    get_desc(&back, buffer);
    get_desc(buffer, &member);
    check(back.Width == desc.Width && back.Flags == desc.Flags && member.Width == desc.Width &&
              member.Flags == desc.Flags && call<uint64_t>(buffer, 11) == address,
          "a resource reports its description, with the result pointer first or second");
  }

  // A placed depth texture that also asks for unordered access (which the
  // desktop does not allow), in the unordered-access layout (3), as titles
  // place shadow maps.
  D3D12_RESOURCE_DESC depth{};
  depth.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  depth.Width = 64;
  depth.Height = 64;
  depth.DepthOrArraySize = 6;
  depth.MipLevels = 1;
  depth.Format = DXGI_FORMAT_R16_TYPELESS;
  depth.SampleDesc.Count = 1;
  depth.Flags = static_cast<D3D12_RESOURCE_FLAGS>(0x80300006);  // depth, unordered access, console bits
  void* shadow = nullptr;
  check(call<HRESULT>(device, 133, address, &depth, static_cast<UINT>(3), static_cast<const void*>(nullptr),
                      __uuidof(ID3D12Resource), &shadow) == S_OK &&
            shadow,
        "a depth texture asking for unordered access is placed in the unordered-access layout");
  if (shadow) {
    // An unordered-access view of it (of its twin on the host).
    D3D12_DESCRIPTOR_HEAP_DESC views_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 1};
    void* views = nullptr;
    call<HRESULT>(device, 14, &views_desc, __uuidof(ID3D12DescriptorHeap), &views);
    if (views) {
      D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
      uav.Format = DXGI_FORMAT_R16_UNORM;
      uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
      uav.Texture2DArray.ArraySize = 6;
      call<void>(device, 19, shadow, static_cast<void*>(nullptr), &uav, call<uint64_t>(views, 9));
      check(true, "depth textures that ask for unordered access get unordered-access views");
      call<ULONG>(views, 2);
    }
    call<ULONG>(shadow, 2);
  }

  // A committed texture like the display buffers titles create: render target
  // and unordered access, created in the unordered-access layout (3).
  D3D12_RESOURCE_DESC texture{};
  texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  texture.Width = 256;
  texture.Height = 128;
  texture.DepthOrArraySize = 1;
  texture.MipLevels = 1;
  texture.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  texture.SampleDesc.Count = 1;
  texture.Flags = static_cast<D3D12_RESOURCE_FLAGS>(0x300005);
  const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
                                   D3D12_MEMORY_POOL_UNKNOWN, 0, 1};
  void* committed = nullptr;
  const HRESULT made = call<HRESULT>(device, 127, &heap, D3D12_HEAP_FLAG_ALLOW_DISPLAY, &texture, 3u,
                                     static_cast<const void*>(nullptr), static_cast<void*>(nullptr), 0u,
                                     static_cast<const void*>(nullptr), __uuidof(ID3D12Resource), &committed);
  check(made == S_OK && committed, "the device creates committed display textures");
  if (committed) call<ULONG>(committed, 2);

  // Packed 16-bit render targets with console flag bits: the size (slot 25,
  // result pointer first) is a real one (titles stop at sizes of 4 GiB and
  // more), and the texture can be made.
  D3D12_RESOURCE_DESC packed = texture;
  packed.Width = 2560;
  packed.Height = 1440;
  packed.Format = DXGI_FORMAT_B4G4R4A4_UNORM;
  packed.Flags = static_cast<D3D12_RESOURCE_FLAGS>(0xC00001);
  D3D12_RESOURCE_ALLOCATION_INFO info{};
  using InfoFn = D3D12_RESOURCE_ALLOCATION_INFO*(STDMETHODCALLTYPE*)(void*, void*, UINT, UINT, const D3D12_RESOURCE_DESC*);
  reinterpret_cast<InfoFn>((*static_cast<void***>(device))[25])(&info, device, 1u, 1u, &packed);
  void* packed_texture = nullptr;
  const HRESULT packed_made = call<HRESULT>(device, 127, &heap, D3D12_HEAP_FLAG_NONE, &packed, 2u,  // render target
                                            static_cast<const void*>(nullptr), static_cast<void*>(nullptr), 0u,
                                            static_cast<const void*>(nullptr), __uuidof(ID3D12Resource),
                                            &packed_texture);
  check(info.SizeInBytes > 0 && info.SizeInBytes < (1ull << 32) && packed_made == S_OK && packed_texture,
        "packed 16-bit render targets have a size and can be made");
  if (packed_texture) call<ULONG>(packed_texture, 2);
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

void put32(std::vector<uint8_t>& out, uint32_t value) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(value >> (8 * i)));
}

// A console shader container that carries only native code (layout in
// docs/PLATFORM_NOTES.md): PSV0 with the thread group size, and a DXIL part
// whose console block holds a zlib stream (one stored block) with the native
// program block.
std::vector<uint8_t> native_only_container(const std::vector<uint32_t>& code) {
  std::vector<uint8_t> block(0x128 + 4 * code.size(), 0);
  std::memcpy(block.data(), "FSSM", 4);
  const uint32_t record = 0x100, root_parameters = 2;
  std::memcpy(block.data() + 0xC0, &record, 4);
  std::memcpy(block.data() + 0xF8, &root_parameters, 4);
  std::memcpy(block.data() + 0x128, code.data(), 4 * code.size());
  uint32_t a = 1, b = 0;
  for (uint8_t byte : block) {
    a = (a + byte) % 65521;
    b = (b + a) % 65521;
  }
  std::vector<uint8_t> stream = {0x78, 0x01, 0x01};
  const auto length = static_cast<uint16_t>(block.size());
  stream.insert(stream.end(), {static_cast<uint8_t>(length), static_cast<uint8_t>(length >> 8),
                               static_cast<uint8_t>(~length), static_cast<uint8_t>(~length >> 8)});
  stream.insert(stream.end(), block.begin(), block.end());
  const uint32_t adler = (b << 16) | a;
  stream.insert(stream.end(), {static_cast<uint8_t>(adler >> 24), static_cast<uint8_t>(adler >> 16),
                               static_cast<uint8_t>(adler >> 8), static_cast<uint8_t>(adler)});

  std::vector<uint8_t> dxil = {'X', 'D', 'A', 'T'};
  for (uint32_t v : {5u, 0u, 1u, 32u, static_cast<uint32_t>(stream.size()), 16u, 0u}) put32(dxil, v);
  dxil.insert(dxil.end(), stream.begin(), stream.end());
  while (dxil.size() % 4) dxil.push_back(0);
  std::vector<uint8_t> psv(56, 0);
  const uint32_t info_size = 52, threads = 64, one = 1;
  std::memcpy(psv.data(), &info_size, 4);
  std::memcpy(psv.data() + 40, &threads, 4);
  std::memcpy(psv.data() + 44, &one, 4);
  std::memcpy(psv.data() + 48, &one, 4);

  std::vector<uint8_t> out = {'D', 'X', 'B', 'C'};
  out.resize(20, 0);
  put32(out, 1);
  put32(out, 0);
  put32(out, 2);
  put32(out, 40);
  put32(out, static_cast<uint32_t>(40 + 8 + psv.size()));
  out.insert(out.end(), {'P', 'S', 'V', '0'});
  put32(out, static_cast<uint32_t>(psv.size()));
  out.insert(out.end(), psv.begin(), psv.end());
  out.insert(out.end(), {'D', 'X', 'I', 'L'});
  put32(out, static_cast<uint32_t>(dxil.size()));
  out.insert(out.end(), dxil.begin(), dxil.end());
  const auto total = static_cast<uint32_t>(out.size());
  std::memcpy(out.data() + 24, &total, 4);
  return out;
}

void test_native_pipeline(void* device) {
  // s_mov_b32 s5, 7; v_mov_b32 v1, s5; s_endpgm
  const std::vector<uint8_t> container = native_only_container({0xBE850387u, 0x7E020205u, 0xBF810000u});
  D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
  pso.CS = {container.data(), container.size()};
  void* pipeline = nullptr;
  check(call<HRESULT>(device, 62, &pso, static_cast<void*>(nullptr), static_cast<void*>(nullptr),
                      __uuidof(ID3D12PipelineState), &pipeline) == S_OK &&
            pipeline,
        "the device creates compute pipelines from shaders that carry only native code");
  if (pipeline) call<ULONG>(pipeline, 2);
}

// Device objects created through slots that keep (or nearly keep) the
// desktop arguments.
void test_device_objects(void* device) {
  D3D12_QUERY_HEAP_DESC query_desc{D3D12_QUERY_HEAP_TYPE_OCCLUSION, 16, 1};
  void* query_heap = nullptr;
  check(call<HRESULT>(device, 39, &query_desc, __uuidof(ID3D12QueryHeap), &query_heap) == S_OK && query_heap,
        "the device creates query heaps");
  if (query_heap) call<ULONG>(query_heap, 2);

  D3D12_HEAP_PROPERTIES readback{};
  readback.Type = D3D12_HEAP_TYPE_READBACK;
  D3D12_RESOURCE_DESC buffer{};
  buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  buffer.Width = 16 * 8;
  buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
  buffer.SampleDesc.Count = 1;
  buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  void* resource = nullptr;
  check(call<HRESULT>(device, 27, &readback, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST,
                      static_cast<const D3D12_CLEAR_VALUE*>(nullptr), __uuidof(ID3D12Resource), &resource) == S_OK &&
            resource,
        "the device creates committed resources with the desktop arguments");
  if (resource) call<ULONG>(resource, 2);

  D3D12_INDIRECT_ARGUMENT_DESC arguments[2]{};
  arguments[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
  D3D12_COMMAND_SIGNATURE_DESC signature_desc{sizeof(D3D12_DRAW_ARGUMENTS), 1, arguments, 0};
  void* signature = nullptr;
  check(call<HRESULT>(device, 41, &signature_desc, static_cast<void*>(nullptr), __uuidof(ID3D12CommandSignature),
                      &signature) == S_OK &&
            signature,
        "the device creates command signatures");
  if (signature) call<ULONG>(signature, 2);
  arguments[1].Type = static_cast<D3D12_INDIRECT_ARGUMENT_TYPE>(105);  // console type, 12-byte arguments
  D3D12_COMMAND_SIGNATURE_DESC console_desc{12, 1, &arguments[1], 0};
  void* console_signature = nullptr;
  check(call<HRESULT>(device, 41, &console_desc, static_cast<void*>(nullptr), __uuidof(ID3D12CommandSignature),
                      &console_signature) == S_OK &&
            console_signature,
        "command signatures accept the console's 12-byte argument type");
  if (console_signature) call<ULONG>(console_signature, 2);

  // Console sampler description into a sampler heap.
  D3D12_DESCRIPTOR_HEAP_DESC sampler_heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 4, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 1};
  void* sampler_heap = nullptr;
  call<HRESULT>(device, 14, &sampler_heap_desc, __uuidof(ID3D12DescriptorHeap), &sampler_heap);
  if (sampler_heap) {
    const uint64_t start = call<uint64_t>(sampler_heap, 9);
    uint32_t sampler[21] = {0, 2, 2, 2, 0, 0, 3, 3, 3, 0, 0x41400000u, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0};
    call<void>(device, 61, sampler, start);
    check(true, "console sampler descriptions create samplers");
    call<ULONG>(sampler_heap, 2);
  }

  // A heap over a reservation with physical pages mapped.
  ULONG_PTR pages[4] = {};
  ULONG_PTR count = 4;
  void* pool = XMemVirtualAlloc(nullptr, 0x40000, 0x20402000, 0x23, PAGE_READWRITE);
  if (pool && XMemAllocatePhysicalPages(0x380, &count, pages) && XMemMapPhysicalPages(pool, 4, pages) == pool) {
    void* heap = nullptr;
    check(call<HRESULT>(device, 74, pool, 4u, &heap) == S_OK && heap, "the device makes heaps over title memory");
    if (heap) {
      // GetDesc: a 48-byte result, with the result pointer first and the
      // object second, and the other way round (the member function order).
      using GetDescFn = D3D12_HEAP_DESC*(STDMETHODCALLTYPE*)(void*, void*);
      const auto get_desc = reinterpret_cast<GetDescFn>((*static_cast<void***>(heap))[8]);
      D3D12_HEAP_DESC desc{}, member{};
      get_desc(&desc, heap);
      get_desc(heap, &member);
      check(desc.SizeInBytes == 0x40000 && member.SizeInBytes == 0x40000,
            "a heap over title memory reports its size, with the result pointer first or second");
      call<ULONG>(heap, 2);
    }
  } else {
    check(false, "physical pages for a heap over title memory map");
  }
}

// A graphics pipeline from a pipeline state stream of desktop shaders.
void test_pipeline_stream(void* device) {
  HMODULE compiler = LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  using CompileFn = HRESULT(WINAPI*)(const void*, SIZE_T, const char*, const void*, void*, const char*, const char*,
                                     UINT, UINT, void**, void**);
  const auto compile = compiler ? reinterpret_cast<CompileFn>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
  const char vs_source[] = "float4 main(uint id : SV_VertexID) : SV_Position { return float4(id, 0, 0, 1); }";
  const char ps_source[] = "float4 main() : SV_Target { return float4(1, 0, 0, 1); }";
  void* vs = nullptr;
  void* ps = nullptr;
  if (compile) {
    compile(vs_source, sizeof(vs_source) - 1, nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0, &vs, nullptr);
    compile(ps_source, sizeof(ps_source) - 1, nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &ps, nullptr);
  }
  D3D12_VERSIONED_ROOT_SIGNATURE_DESC root_desc{};
  root_desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
  ID3DBlob* serialized = nullptr;
  D3D12SerializeVersionedRootSignature(&root_desc, &serialized, nullptr);
  void* root = nullptr;
  if (serialized) {
    call<HRESULT>(device, 16, 1u, blob_pointer(serialized), blob_size(serialized), __uuidof(ID3D12RootSignature),
                  &root);
  }
  if (!vs || !ps || !root) {
    check(false, "pipeline stream ingredients are available");
    return;
  }
  struct alignas(void*) RootSubobject {
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE;
    void* value;
  };
  struct alignas(void*) ShaderSubobject {
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;
    D3D12_SHADER_BYTECODE value;
  };
  struct alignas(void*) TopologySubobject {
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY;
    D3D12_PRIMITIVE_TOPOLOGY_TYPE value = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  };
  struct alignas(void*) FormatsSubobject {
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS;
    D3D12_RT_FORMAT_ARRAY value{{DXGI_FORMAT_R8G8B8A8_UNORM}, 1};
  };
  struct Stream {
    RootSubobject root;
    ShaderSubobject vs;
    ShaderSubobject ps;
    TopologySubobject topology;
    FormatsSubobject formats;
  } stream{};
  stream.root.value = root;
  stream.vs = {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS, {blob_pointer(vs), blob_size(vs)}};
  stream.ps = {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, {blob_pointer(ps), blob_size(ps)}};
  D3D12_PIPELINE_STATE_STREAM_DESC desc{sizeof(stream), &stream};
  void* pipeline = nullptr;
  check(call<HRESULT>(device, 86, &desc, __uuidof(ID3D12PipelineState), &pipeline) == S_OK && pipeline,
        "the device creates graphics pipelines from pipeline state streams");
  if (pipeline) call<ULONG>(pipeline, 2);

  // The same stream with vertex and pixel shaders that carry only native
  // code: exp param0 v0..v3; exp pos0 v0..v3 done; s_endpgm, and
  // v_interp_mov_f32 v2, p0, attr0.x; exp mrt0 v2 done; s_endpgm.
  const std::vector<uint8_t> native_vs =
      native_only_container({0xF800020Fu, 0x03020100u, 0xF80008CFu, 0x03020100u, 0xBF810000u});
  const std::vector<uint8_t> native_ps = native_only_container({0xC80A0002u, 0xF8000801u, 0x00000002u, 0xBF810000u});
  stream.vs.value = {native_vs.data(), native_vs.size()};
  stream.ps.value = {native_ps.data(), native_ps.size()};
  void* native = nullptr;
  check(call<HRESULT>(device, 86, &desc, __uuidof(ID3D12PipelineState), &native) == S_OK && native,
        "graphics pipelines translate vertex and pixel shaders that carry only native code");
  if (native) call<ULONG>(native, 2);
}

// Constant buffer views narrower than the desktop's 256-byte granularity.
void test_small_constant_buffer_view(void* device) {
  D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 1};
  void* heap = nullptr;
  call<HRESULT>(device, 14, &heap_desc, __uuidof(ID3D12DescriptorHeap), &heap);
  D3D12_HEAP_PROPERTIES upload{};
  upload.Type = D3D12_HEAP_TYPE_UPLOAD;
  D3D12_RESOURCE_DESC buffer{};
  buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  buffer.Width = 0x10000;
  buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
  buffer.SampleDesc.Count = 1;
  buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  void* resource = nullptr;
  call<HRESULT>(device, 27, &upload, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_GENERIC_READ,
                static_cast<const D3D12_CLEAR_VALUE*>(nullptr), __uuidof(ID3D12Resource), &resource);
  if (!heap || !resource) {
    check(false, "a buffer and a descriptor heap for constant buffer views exist");
    return;
  }
  const uint64_t address = call<uint64_t>(resource, 11);  // GetGPUVirtualAddress
  D3D12_CONSTANT_BUFFER_VIEW_DESC view{address, 128};
  call<void>(device, 17, &view, call<uint64_t>(heap, 9));
  D3D12_QUERY_HEAP_DESC query_desc{D3D12_QUERY_HEAP_TYPE_OCCLUSION, 1, 1};
  void* query = nullptr;
  check(call<HRESULT>(device, 39, &query_desc, __uuidof(ID3D12QueryHeap), &query) == S_OK && query,
        "a 128-byte constant buffer view leaves the device usable");
  if (query) call<ULONG>(query, 2);
  call<ULONG>(resource, 2);
  call<ULONG>(heap, 2);
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

namespace {

// Queue slot 31: page mapping updates in queue order.
void test_page_mapping_update(void* device) {
  const D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 1};
  void* queue = nullptr;
  call<HRESULT>(device, 8, &queue_desc, __uuidof(ID3D12CommandQueue), &queue);
  ULONG_PTR pages[2] = {};
  ULONG_PTR count = 2;
  auto* pool = static_cast<unsigned char*>(XMemVirtualAlloc(nullptr, 0x20000, 0x20402000, 0x23, PAGE_READWRITE));
  auto* range = static_cast<unsigned char*>(XMemVirtualAlloc(nullptr, 0x20000, 0x20402000, 0x23, PAGE_READWRITE));
  const bool ready = queue && pool && range && XMemAllocatePhysicalPages(0x280, &count, pages) &&
                     XMemMapPhysicalPages(pool, 2, pages) == pool;
  check(ready, "a page pool and a streaming range exist");
  if (!ready) return;
  pool[0x10] = 0xAB;
  pool[0x10000 + 0x10] = 0xCD;
  struct Run {
    uint32_t kind, page, count;
  };
  struct Mapping {
    uint64_t address;
    uint32_t runs, reserved;
    const Run* run;
  };
  // Pool page 1 into the second page of the range, then page 0 into the first.
  const Run second_page{1, 1, 1}, first_page{1, 0, 1};
  const Mapping both[2] = {{reinterpret_cast<uint64_t>(range) + 0x10000, 1, 0, &second_page},
                           {reinterpret_cast<uint64_t>(range), 1, 0, &first_page}};
  call<HRESULT>(queue, 31, 2u, both, reinterpret_cast<uint64_t>(pool), 2u, 0u);
  check(range[0x10] == 0xAB && range[0x10000 + 0x10] == 0xCD, "queue page mapping updates map pool pages");
  range[0x20] = 0x5A;
  check(pool[0x20] == 0x5A, "a mapped pool page is the same memory as the pool");
  const Run unmap{2, 0, 2};
  const Mapping release{reinterpret_cast<uint64_t>(range), 1, 0, &unmap};
  call<HRESULT>(queue, 31, 1u, &release, reinterpret_cast<uint64_t>(pool), 2u, 0u);
  MEMORY_BASIC_INFORMATION info{};
  VirtualQuery(range, &info, sizeof(info));
  check(info.State != MEM_COMMIT && pool[0x10] == 0xAB, "queue page mapping updates unmap pages");
  XMemMapPhysicalPages(pool, 2, nullptr);
  XMemFreePhysicalPages(2, pages);
  call<ULONG>(queue, 2);
}

}  // namespace

// Committed title memory the GPU used goes back to the host when the title
// releases it: the runtime lets go of its GPU heap over the memory first.
void test_released_memory(void* device) {
  constexpr SIZE_T kBytes = 64ull << 20;
  constexpr int kRounds = 16;
  PROCESS_MEMORY_COUNTERS_EX before{}, after{};
  PERFORMANCE_INFORMATION system_before{}, system_after{};
  K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&before), sizeof(before));
  K32GetPerformanceInfo(&system_before, sizeof(system_before));
  int placed = 0;
  for (int i = 0; i < kRounds; ++i) {
    void* memory = VirtualAlloc(nullptr, kBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!memory) break;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = kBytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    void* buffer = nullptr;
    if (call<HRESULT>(device, 133, reinterpret_cast<uint64_t>(memory), &desc, static_cast<UINT>(0xFFFFFFFF),
                      static_cast<const void*>(nullptr), __uuidof(ID3D12Resource), &buffer) == S_OK &&
        buffer) {
      ++placed;
      call<ULONG>(buffer, 2);
    }
    VirtualFree(memory, 0, MEM_RELEASE);
  }
  K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&after), sizeof(after));
  K32GetPerformanceInfo(&system_after, sizeof(system_after));
  check(placed == kRounds, "buffers are placed in committed title memory, round after round");
  const int64_t process_growth = static_cast<int64_t>(after.PrivateUsage) - static_cast<int64_t>(before.PrivateUsage);
  const int64_t system_growth = (static_cast<int64_t>(system_after.CommitTotal) -
                                 static_cast<int64_t>(system_before.CommitTotal)) *
                                static_cast<int64_t>(system_after.PageSize);
  std::printf("    (released memory: process commit %+lld MiB, system commit %+lld MiB over %d rounds of %llu MiB)\n",
              static_cast<long long>(process_growth >> 20), static_cast<long long>(system_growth >> 20), kRounds,
              static_cast<unsigned long long>(kBytes >> 20));
  check(process_growth < static_cast<int64_t>(4 * kBytes) && system_growth < static_cast<int64_t>(4 * kBytes),
        "committed title memory the GPU used goes back to the host when released");
}

// Textures viewed in title memory the title then releases: the runtime's host
// copies of them go once the GPU is done with the work submitted before (here
// at once: it is idle), read from the runtime's live count of host textures
// filled from title memory. The memory is held until all are viewed, so every
// texture is at another address.
void test_released_texture_memory(void* device) {
  using CounterFn = volatile int64_t* (*)(const char*);
  const HMODULE runtime = GetModuleHandleW(L"holyrt.dll");
  const auto counter = runtime ? reinterpret_cast<CounterFn>(GetProcAddress(runtime, "HolyCounter")) : nullptr;
  volatile int64_t* live = counter ? counter("d3d12.host_textures_from_memory") : nullptr;
  void* heap = nullptr;
  D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
  call<HRESULT>(device, 14, &heap_desc, __uuidof(ID3D12DescriptorHeap), &heap);
  if (!live || !heap) {
    check(false, "the objects for viewing textures in released memory exist");
    return;
  }
  D3D12_RESOURCE_DESC texture{};
  texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  texture.Width = 64;
  texture.Height = 64;
  texture.DepthOrArraySize = 1;
  texture.MipLevels = 1;
  texture.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  texture.SampleDesc.Count = 1;
  struct {
    D3D12_SHADER_RESOURCE_VIEW_DESC srv;
    uint64_t address;
  } view{};
  view.srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  view.srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  view.srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  view.srv.Texture2D.MipLevels = 1;
  const uint64_t descriptor = call<uint64_t>(heap, 9);
  auto view_at = [&](void* memory) {
    view.address = reinterpret_cast<uint64_t>(memory);
    call<void>(device, 70, &texture, &view, descriptor);
  };
  const int64_t before = *live;
  constexpr int kRounds = 8;
  void* blocks[kRounds] = {};
  for (void*& block : blocks) {
    block = VirtualAlloc(nullptr, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (block) view_at(block);
  }
  const int64_t held = *live - before;
  for (void* block : blocks) {
    if (block) VirtualFree(block, 0, MEM_RELEASE);
  }
  // The next view applies the releases.
  void* last = VirtualAlloc(nullptr, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  if (last) view_at(last);
  const int64_t after = *live - before;
  if (last) VirtualFree(last, 0, MEM_RELEASE);
  std::printf("    (host textures from title memory: %+lld after %d views, %+lld once the memory was released and "
              "another viewed)\n",
              static_cast<long long>(held), kRounds, static_cast<long long>(after));
  check(held == kRounds && after == 1, "host copies of textures in memory the title released are let go of");
  call<ULONG>(heap, 2);
}

// The host's commit left, in GiB: printed with GPU results, since with it used
// up the host refuses pipelines and memory and such checks read zeros.
double host_commit_left() {
  PERFORMANCE_INFORMATION host{};
  K32GetPerformanceInfo(&host, sizeof(host));
  return static_cast<double>(host.CommitLimit - host.CommitTotal) * static_cast<double>(host.PageSize) / (1ull << 30);
}

// A native compute program reads a texture the title viewed in its memory,
// all four channels, through the descriptor in the bound heap, and writes them
// to an address passed as root constants, in memory a buffer is placed in
// (the memory map): the whole path of translated work, from recording to the
// GPU. The texture's memory, by `kind`: plain committed memory, written before
// the view; memory from XMemVirtualAlloc, and physical pages the title mapped,
// both written after the view was made and the work submitted a frame later
// (the host copy of the texture follows the write: tracked writes, writes
// through the title's view of physical pages); and a physical page the title
// maps (and writes) only after viewing the texture there.
enum class TextureMemory { Plain, Reserved, Physical, Unmapped };

// The command list interface titles ask for.
constexpr GUID kListIid = {0xc3827890, 0x4024, 0x474c, {0x98, 0xcf, 0x64, 0x5b, 0x51, 0xdc, 0x48, 0x74}};

void test_compute_reads_a_texture_view(void* device, void* queue, TextureMemory kind) {
  const bool after = kind != TextureMemory::Plain;
  void* allocator = nullptr;
  void* list = nullptr;
  void* fence = nullptr;
  void* heap = nullptr;
  void* signature = nullptr;
  void* pipeline = nullptr;
  void* placed = nullptr;
  const uint32_t list_desc[4] = {0, 2, 1, 0};
  call<HRESULT>(device, 9, D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), &allocator);
  if (allocator) call<HRESULT>(device, 73, list_desc, allocator, static_cast<void*>(nullptr), kListIid, &list);
  call<HRESULT>(device, 36, UINT64{0}, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), &fence);
  D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 16,
                                       D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 1};
  call<HRESULT>(device, 14, &heap_desc, __uuidof(ID3D12DescriptorHeap), &heap);
  // Root arguments: a table with the texture view, then the output address.
  D3D12_DESCRIPTOR_RANGE1 range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_NONE, 0};
  D3D12_ROOT_PARAMETER1 parameters[2] = {};
  parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  parameters[0].DescriptorTable = {1, &range};
  parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  parameters[1].Constants = {0, 0, 2};
  D3D12_VERSIONED_ROOT_SIGNATURE_DESC root_desc{};
  root_desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
  root_desc.Desc_1_1.NumParameters = 2;
  root_desc.Desc_1_1.pParameters = parameters;
  ID3DBlob* serialized = nullptr;
  if (SUCCEEDED(D3D12SerializeVersionedRootSignature(&root_desc, &serialized, nullptr)) && serialized) {
    call<HRESULT>(device, 16, 1u, blob_pointer(serialized), blob_size(serialized), __uuidof(ID3D12RootSignature),
                  &signature);
  }
  // s_load_dwordx4 s[4:7], s[0:1]: the root table (s4 the descriptor table's
  // GPU address, s[5:6] the output address); s_mov_b32 s8, s4; s9 = 0;
  // s_load_dwordx8 s[12:19], s[8:9]: the texture's descriptor;
  // v0 = v1 = 0; image_load v[2:5], v[0:1], s[12:19] dmask:0xf dim:2D;
  // s[20:23] = a buffer at s[5:6] of 64 bytes;
  // buffer_store_dwordx4 v[2:5], off, s[20:23], 0; v6 = 0x12345678;
  // buffer_store_dword v6, off, s[20:23], 0 offset:16 (a marker: the program
  // ran, whatever the texel); s_endpgm.
  const std::vector<uint8_t> container = native_only_container(
      {0xF4080100u, 0xFA000000u, 0xBF8CC07Fu, 0xBE880304u, 0xBE890380u, 0xF40C0304u, 0xFA000000u, 0xBF8CC07Fu,
       0x7E000280u, 0x7E020280u, 0xF0001F08u, 0x00030200u, 0xBF8C3F70u, 0xBE940305u, 0xBE950306u, 0xB0160040u,
       0xBE970380u, 0xE0780000u, 0x80050200u, 0x7E0C02FFu, 0x12345678u, 0xE0700010u, 0x80050600u,
       0xBF810000u});
  D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
  pso.CS = {container.data(), container.size()};
  call<HRESULT>(device, 62, &pso, static_cast<void*>(nullptr), static_cast<void*>(nullptr),
                __uuidof(ID3D12PipelineState), &pipeline);
  volatile uint8_t* memory = nullptr;
  volatile float* output = nullptr;
  const bool physical = kind == TextureMemory::Physical || kind == TextureMemory::Unmapped;
  ULONG_PTR page = 0;
  if (physical) {
    ULONG_PTR count = 1;
    XMemAllocatePhysicalPages(0x280, &count, &page);
    void* region = XMemVirtualAlloc(nullptr, 0x10000, 0x20402000, 0x23, PAGE_READWRITE);
    if (count == 1 && region &&
        (kind == TextureMemory::Unmapped || XMemMapPhysicalPages(region, 1, &page) == region)) {
      memory = static_cast<volatile uint8_t*>(region);
    }
    output = static_cast<volatile float*>(VirtualAlloc(nullptr, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
  } else {
    memory = static_cast<volatile uint8_t*>(
        kind == TextureMemory::Reserved ? XMemVirtualAlloc(nullptr, 0x20000, MEM_RESERVE | MEM_COMMIT, 0x23, PAGE_READWRITE)
                                        : VirtualAlloc(nullptr, 0x20000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (memory) output = reinterpret_cast<volatile float*>(memory + 0x10000);
  }
  if (!list || !fence || !heap || !signature || !pipeline || !memory || !output) {
    check(false, "the objects for a compute program reading a texture view exist");
    return;
  }
  if (kind != TextureMemory::Unmapped) {
    for (uint32_t i = 0; i < 0x4000; ++i) memory[i] = 0x40;
  }
  // Other texels when written after the view (the address may be the one an
  // earlier texture of the same shape had: only a fresh copy shows these).
  const uint8_t base_byte = kind == TextureMemory::Plain      ? 0x10
                            : kind == TextureMemory::Reserved ? 0x50
                            : kind == TextureMemory::Physical ? 0x90
                                                              : 0xC0;
  const uint8_t texel_bytes[4] = {base_byte, static_cast<uint8_t>(base_byte + 0x10),
                                  static_cast<uint8_t>(base_byte + 0x20), 0xFF};
  auto write_texel = [memory, &texel_bytes] {
    for (int i = 0; i < 4; ++i) memory[i] = texel_bytes[i];
  };
  if (!after) write_texel();
  const uint64_t output_address = reinterpret_cast<uint64_t>(output);
  D3D12_RESOURCE_DESC buffer{};
  buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  buffer.Width = 0x10000;
  buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
  buffer.SampleDesc.Count = 1;
  buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  buffer.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  call<HRESULT>(device, 133, output_address, &buffer, static_cast<UINT>(0xFFFFFFFF), static_cast<const void*>(nullptr),
                __uuidof(ID3D12Resource), &placed);

  D3D12_RESOURCE_DESC texture{};
  texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  texture.Width = 64;
  texture.Height = 64;
  texture.DepthOrArraySize = 1;
  texture.MipLevels = 1;
  texture.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  texture.SampleDesc.Count = 1;
  struct {
    D3D12_SHADER_RESOURCE_VIEW_DESC srv;
    uint64_t address;
  } view{};
  view.srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  view.srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  view.srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  view.srv.Texture2D.MipLevels = 1;
  view.address = reinterpret_cast<uint64_t>(memory);
  call<void>(device, 70, &texture, &view, call<uint64_t>(heap, 9));
  if (kind == TextureMemory::Unmapped) XMemMapPhysicalPages(const_cast<uint8_t*>(memory), 1, &page);
  if (after) {
    write_texel();
    Sleep(40);
  }
  const uint64_t table = call<uint64_t>(heap, 10);
  const uint32_t constants[2] = {static_cast<uint32_t>(output_address), static_cast<uint32_t>(output_address >> 32)};
  void* heaps[] = {heap};
  bool recorded = call<HRESULT>(list, 139, allocator, static_cast<void*>(nullptr), UINT64{0}) == S_OK;
  call<void>(list, 32, 1u, static_cast<void* const*>(heaps));
  call<void>(list, 33, signature);
  call<void>(list, 35, 0u, table);
  call<void>(list, 39, 1u, 2u, static_cast<const void*>(constants), 0u);
  call<void>(list, 25, pipeline);
  call<void>(list, 14, 1u, 1u, 1u);
  recorded = recorded && call<HRESULT>(list, 9) == S_OK;
  void* lists[] = {list};
  call<void>(queue, 35, 1u, static_cast<void* const*>(lists), UINT64{0}, 0u);
  call<HRESULT>(queue, 16, fence, UINT64{1});
  const auto* value = call<const volatile uint64_t*>(fence, 12);
  for (int i = 0; value && *value < 1 && i < 5000; ++i) Sleep(1);
  auto close_to = [](float a, float b) { return a > b - 0.002f && a < b + 0.002f; };
  const float texel[4] = {output[0], output[1], output[2], output[3]};
  // With the host's commit nearly used up, physical pages cannot be committed
  // and read as zero: said with the result, so such a failure tells.
  PERFORMANCE_INFORMATION host{};
  K32GetPerformanceInfo(&host, sizeof(host));
  const double commit_left =
      static_cast<double>(host.CommitLimit - host.CommitTotal) * static_cast<double>(host.PageSize) / (1ull << 30);
  uint32_t marker = 0;
  std::memcpy(&marker, const_cast<const float*>(output + 4), 4);
  std::printf("    (texel read on the GPU: %.3f %.3f %.3f %.3f; marker %08x; fence %llu; host commit left %.1f GiB)\n",
              texel[0], texel[1], texel[2], texel[3], marker, static_cast<unsigned long long>(value ? *value : 0),
              commit_left);
  check(recorded && value && *value == 1 && close_to(texel[0], texel_bytes[0] / 255.0f) &&
            close_to(texel[1], texel_bytes[1] / 255.0f) && close_to(texel[2], texel_bytes[2] / 255.0f) &&
            close_to(texel[3], texel_bytes[3] / 255.0f),
        kind == TextureMemory::Plain      ? "a native compute program reads all four channels of a texture viewed in title memory"
        : kind == TextureMemory::Reserved ? "a texture written after its view shows the new texels to the GPU"
        : kind == TextureMemory::Physical ? "a texture in physical pages written through the title's view after its view "
                                            "shows the new texels to the GPU"
                                          : "a texture viewed before its physical page was mapped shows what the title "
                                            "wrote there");

  if (placed) call<ULONG>(placed, 2);
  call<ULONG>(pipeline, 2);
  call<ULONG>(signature, 2);
  call<ULONG>(heap, 2);
  call<ULONG>(fence, 2);
  call<ULONG>(list, 2);
  call<ULONG>(allocator, 2);
  if (physical) {
    VirtualFree(const_cast<float*>(output), 0, MEM_RELEASE);
  } else {
    VirtualFree(const_cast<uint8_t*>(memory), 0, MEM_RELEASE);
  }
}

// A draw with native vertex and pixel programs into a render target, as
// titles draw their loading screens: the vertex program fetches position and
// colour through the vertex buffer table (s[0:1]) by vertex index, the pixel
// program exports the colour, flat. A native compute program then reads the
// target through a view and stores the pixel at (0, 0).
void test_native_draw(void* device, void* queue) {
  void* allocator = nullptr;
  void* list = nullptr;
  void* fence = nullptr;
  void* heap = nullptr;
  void* targets = nullptr;
  void* graphics_root = nullptr;
  void* compute_root = nullptr;
  void* draw_pipeline = nullptr;
  void* read_pipeline = nullptr;
  void* target = nullptr;
  void* placed = nullptr;
  const uint32_t list_desc[4] = {0, 2, 1, 0};
  call<HRESULT>(device, 9, D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), &allocator);
  if (allocator) call<HRESULT>(device, 73, list_desc, allocator, static_cast<void*>(nullptr), kListIid, &list);
  call<HRESULT>(device, 36, UINT64{0}, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), &fence);
  D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 16,
                                       D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 1};
  call<HRESULT>(device, 14, &heap_desc, __uuidof(ID3D12DescriptorHeap), &heap);
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 1};
  call<HRESULT>(device, 14, &rtv_desc, __uuidof(ID3D12DescriptorHeap), &targets);

  // Root signatures: none for the draw; a table and two constants for the read.
  D3D12_VERSIONED_ROOT_SIGNATURE_DESC empty{};
  empty.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
  ID3DBlob* serialized = nullptr;
  if (SUCCEEDED(D3D12SerializeVersionedRootSignature(&empty, &serialized, nullptr)) && serialized) {
    call<HRESULT>(device, 16, 1u, blob_pointer(serialized), blob_size(serialized), __uuidof(ID3D12RootSignature),
                  &graphics_root);
  }
  D3D12_DESCRIPTOR_RANGE1 range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_NONE, 0};
  D3D12_ROOT_PARAMETER1 parameters[2] = {};
  parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  parameters[0].DescriptorTable = {1, &range};
  parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  parameters[1].Constants = {0, 0, 2};
  D3D12_VERSIONED_ROOT_SIGNATURE_DESC read_desc{};
  read_desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
  read_desc.Desc_1_1.NumParameters = 2;
  read_desc.Desc_1_1.pParameters = parameters;
  serialized = nullptr;
  if (SUCCEEDED(D3D12SerializeVersionedRootSignature(&read_desc, &serialized, nullptr)) && serialized) {
    call<HRESULT>(device, 16, 1u, blob_pointer(serialized), blob_size(serialized), __uuidof(ID3D12RootSignature),
                  &compute_root);
  }

  // Vertex program: v_add_nc_u32 v0, s2, v0 (the first vertex, as console
  // code adds it); s_load_dwordx4 s[4:7], s[0:1] (vertex buffer 0's
  // descriptor); buffer_load_dwordx4 v[1:4], v0, s[4:7] idxen (position);
  // buffer_load_dwordx4 v[5:8], v0, s[4:7] idxen offset:16 (colour);
  // exp param0 v[5:8]; exp pos0 v[1:4] done; s_endpgm.
  const std::vector<uint8_t> native_vs = native_only_container(
      {0x4A000002u, 0xF4080100u, 0xFA000000u, 0xBF8CC07Fu, 0xE0382000u, 0x80010100u, 0xE0382010u, 0x80010500u,
       0xBF8C3F70u, 0xF800020Fu, 0x08070605u, 0xF80008CFu, 0x04030201u, 0xBF810000u});
  // Pixel program: v_interp_mov_f32 v2..v5, p0, attr0.xyzw; exp mrt0 v[2:5] done; s_endpgm.
  const std::vector<uint8_t> native_ps = native_only_container(
      {0xC80A0002u, 0xC80E0102u, 0xC8120202u, 0xC8160302u, 0xF800080Fu, 0x05040302u, 0xBF810000u});
  struct alignas(void*) RootSubobject {
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE;
    void* value;
  };
  struct alignas(void*) ShaderSubobject {
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;
    D3D12_SHADER_BYTECODE value;
  };
  struct alignas(void*) TopologySubobject {
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY;
    D3D12_PRIMITIVE_TOPOLOGY_TYPE value = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  };
  struct alignas(void*) FormatsSubobject {
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS;
    D3D12_RT_FORMAT_ARRAY value{{DXGI_FORMAT_R8G8B8A8_UNORM}, 1};
  };
  struct Stream {
    RootSubobject root;
    ShaderSubobject vs;
    ShaderSubobject ps;
    TopologySubobject topology;
    FormatsSubobject formats;
  } stream{};
  stream.root.value = graphics_root;
  stream.vs = {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS, {native_vs.data(), native_vs.size()}};
  stream.ps = {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, {native_ps.data(), native_ps.size()}};
  D3D12_PIPELINE_STATE_STREAM_DESC stream_desc{sizeof(stream), &stream};
  if (graphics_root) call<HRESULT>(device, 86, &stream_desc, __uuidof(ID3D12PipelineState), &draw_pipeline);
  const std::vector<uint8_t> read_cs = native_only_container(
      {0xF4080100u, 0xFA000000u, 0xBF8CC07Fu, 0xBE880304u, 0xBE890380u, 0xF40C0304u, 0xFA000000u, 0xBF8CC07Fu,
       0x7E000280u, 0x7E020280u, 0xF0001F08u, 0x00030200u, 0xBF8C3F70u, 0xBE940305u, 0xBE950306u, 0xB0160040u,
       0xBE970380u, 0xE0780000u, 0x80050200u, 0xBF810000u});
  D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
  pso.CS = {read_cs.data(), read_cs.size()};
  call<HRESULT>(device, 62, &pso, static_cast<void*>(nullptr), static_cast<void*>(nullptr),
                __uuidof(ID3D12PipelineState), &read_pipeline);

  // The render target.
  D3D12_HEAP_PROPERTIES local{};
  local.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC texture{};
  texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  texture.Width = 64;
  texture.Height = 64;
  texture.DepthOrArraySize = 1;
  texture.MipLevels = 1;
  texture.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  texture.SampleDesc.Count = 1;
  texture.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  // Slot 127 (the shape of CreateCommittedResource3), as titles create their
  // targets: the initial state is a layout.
  call<HRESULT>(device, 127, &local, D3D12_HEAP_FLAG_NONE, &texture, static_cast<UINT>(D3D12_BARRIER_LAYOUT_RENDER_TARGET),
                static_cast<const D3D12_CLEAR_VALUE*>(nullptr), static_cast<void*>(nullptr), 0u,
                static_cast<const void*>(nullptr), __uuidof(ID3D12Resource), &target);

  // Vertices (position, colour) of a triangle covering the target, and the
  // output, in title memory.
  auto* memory = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x20000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
  if (!list || !fence || !heap || !targets || !compute_root || !draw_pipeline || !read_pipeline || !target || !memory) {
    check(false, "the objects for a native draw exist");
    return;
  }
  // Three vertices of another colour first: the draw starts at vertex 3.
  const float vertices[6][8] = {{-1, -1, 0, 1, 1, 0, 1, 1},
                                {-1, 3, 0, 1, 1, 0, 1, 1},
                                {3, -1, 0, 1, 1, 0, 1, 1},
                                {-1, -1, 0, 1, 0.25f, 0.5f, 0.75f, 1},
                                {-1, 3, 0, 1, 0.25f, 0.5f, 0.75f, 1},
                                {3, -1, 0, 1, 0.25f, 0.5f, 0.75f, 1}};
  std::memcpy(memory, vertices, sizeof(vertices));
  auto* output = reinterpret_cast<volatile float*>(memory + 0x10000);
  const uint64_t output_address = reinterpret_cast<uint64_t>(output);
  D3D12_RESOURCE_DESC buffer{};
  buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  buffer.Width = 0x10000;
  buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
  buffer.SampleDesc.Count = 1;
  buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  buffer.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  call<HRESULT>(device, 133, output_address, &buffer, static_cast<UINT>(0xFFFFFFFF), static_cast<const void*>(nullptr),
                __uuidof(ID3D12Resource), &placed);

  const uint64_t rtv = call<uint64_t>(targets, 9);
  call<void>(device, 20, target, static_cast<const D3D12_RENDER_TARGET_VIEW_DESC*>(nullptr), rtv);
  D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
  srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv.Texture2D.MipLevels = 1;
  call<void>(device, 18, target, &srv, call<uint64_t>(heap, 9));

  bool recorded = call<HRESULT>(list, 139, allocator, static_cast<void*>(nullptr), UINT64{0}) == S_OK;
  call<void>(list, 25, draw_pipeline);
  call<void>(list, 34, graphics_root);
  call<void>(list, 20, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  const D3D12_VERTEX_BUFFER_VIEW vertex_buffer{reinterpret_cast<uint64_t>(memory), sizeof(vertices), 32};
  call<void>(list, 48, 0u, 1u, &vertex_buffer);
  const D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle{static_cast<SIZE_T>(rtv)};
  call<void>(list, 50, 1u, &rtv_handle, FALSE, static_cast<const D3D12_CPU_DESCRIPTOR_HANDLE*>(nullptr));
  const D3D12_VIEWPORT viewport{0, 0, 64, 64, 0, 1};
  const D3D12_RECT scissor{0, 0, 64, 64};
  call<void>(list, 21, 1u, &viewport);
  call<void>(list, 22, 1u, &scissor);
  call<void>(list, 12, 3u, 1u, 3u, 0u);
  D3D12_TEXTURE_BARRIER barrier{};
  barrier.SyncBefore = D3D12_BARRIER_SYNC_RENDER_TARGET;
  barrier.SyncAfter = D3D12_BARRIER_SYNC_COMPUTE_SHADING;
  barrier.AccessBefore = D3D12_BARRIER_ACCESS_RENDER_TARGET;
  barrier.AccessAfter = D3D12_BARRIER_ACCESS_SHADER_RESOURCE;
  barrier.LayoutBefore = D3D12_BARRIER_LAYOUT_RENDER_TARGET;
  barrier.LayoutAfter = D3D12_BARRIER_LAYOUT_SHADER_RESOURCE;
  barrier.pResource = static_cast<ID3D12Resource*>(target);
  barrier.Subresources.IndexOrFirstMipLevel = 0xFFFFFFFFu;
  D3D12_BARRIER_GROUP group{};
  group.Type = D3D12_BARRIER_TYPE_TEXTURE;
  group.NumBarriers = 1;
  group.pTextureBarriers = &barrier;
  call<void>(list, 146, 1u, static_cast<const D3D12_BARRIER_GROUP*>(&group));
  const uint32_t constants[2] = {static_cast<uint32_t>(output_address), static_cast<uint32_t>(output_address >> 32)};
  void* heaps[] = {heap};
  call<void>(list, 32, 1u, static_cast<void* const*>(heaps));
  call<void>(list, 33, compute_root);
  call<void>(list, 35, 0u, call<uint64_t>(heap, 10));
  call<void>(list, 39, 1u, 2u, static_cast<const void*>(constants), 0u);
  call<void>(list, 25, read_pipeline);
  call<void>(list, 14, 1u, 1u, 1u);
  recorded = recorded && call<HRESULT>(list, 9) == S_OK;
  void* lists[] = {list};
  call<void>(queue, 35, 1u, static_cast<void* const*>(lists), UINT64{0}, 0u);
  call<HRESULT>(queue, 16, fence, UINT64{1});
  const auto* value = call<const volatile uint64_t*>(fence, 12);
  for (int i = 0; value && *value < 1 && i < 5000; ++i) Sleep(1);
  auto close_to = [](float a, float b) { return a > b - 0.01f && a < b + 0.01f; };
  const float pixel[4] = {output[0], output[1], output[2], output[3]};
  std::printf("    (pixel the draw left: %.3f %.3f %.3f %.3f; fence %llu; host commit left %.1f GiB)\n", pixel[0],
              pixel[1], pixel[2], pixel[3], static_cast<unsigned long long>(value ? *value : 0), host_commit_left());
  check(recorded && value && *value == 1 && close_to(pixel[0], 0.25f) && close_to(pixel[1], 0.5f) &&
            close_to(pixel[2], 0.75f) && close_to(pixel[3], 1.0f),
        "a draw from vertex 3 with native programs fetching vertices through the vertex buffer table colours its "
        "target");

  if (placed) call<ULONG>(placed, 2);
  call<ULONG>(target, 2);
  call<ULONG>(read_pipeline, 2);
  call<ULONG>(draw_pipeline, 2);
  call<ULONG>(compute_root, 2);
  call<ULONG>(graphics_root, 2);
  call<ULONG>(targets, 2);
  call<ULONG>(heap, 2);
  call<ULONG>(fence, 2);
  call<ULONG>(list, 2);
  call<ULONG>(allocator, 2);
  VirtualFree(memory, 0, MEM_RELEASE);
}

// What recording a draw of native programs costs the CPU, as titles record
// them in a frame: root constants, a root constant buffer and the draw, with a
// table of 32 buffer views bound; then with the pipeline (one of two) and the
// table set again before each draw. Measured, printed, and held to a bound
// well above what a frame of thousands of draws can afford (a regression
// check); the list then runs, so every draw is one the GPU takes.
void test_draw_recording_cost(void* device, void* queue) {
  void* allocator = nullptr;
  void* list = nullptr;
  void* fence = nullptr;
  void* heap = nullptr;
  void* targets = nullptr;
  void* root = nullptr;
  void* pipeline = nullptr;
  void* other_pipeline = nullptr;  // the same programs, another rasterizer state
  void* target = nullptr;
  const uint32_t list_desc[4] = {0, 2, 1, 0};
  call<HRESULT>(device, 9, D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), &allocator);
  if (allocator) call<HRESULT>(device, 73, list_desc, allocator, static_cast<void*>(nullptr), kListIid, &list);
  call<HRESULT>(device, 36, UINT64{0}, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), &fence);
  constexpr UINT kViews = 32;
  D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kViews,
                                       D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 1};
  call<HRESULT>(device, 14, &heap_desc, __uuidof(ID3D12DescriptorHeap), &heap);
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 1};
  call<HRESULT>(device, 14, &rtv_desc, __uuidof(ID3D12DescriptorHeap), &targets);

  // Root signature: a table of buffer views, a constant buffer, constants.
  D3D12_DESCRIPTOR_RANGE1 range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kViews, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_NONE, 0};
  D3D12_ROOT_PARAMETER1 parameters[3] = {};
  parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  parameters[0].DescriptorTable = {1, &range};
  parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
  parameters[1].Descriptor = {0, 0, D3D12_ROOT_DESCRIPTOR_FLAG_NONE};
  parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  parameters[2].Constants = {1, 0, 4};
  D3D12_VERSIONED_ROOT_SIGNATURE_DESC root_desc{};
  root_desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
  root_desc.Desc_1_1.NumParameters = 3;
  root_desc.Desc_1_1.pParameters = parameters;
  ID3DBlob* serialized = nullptr;
  if (SUCCEEDED(D3D12SerializeVersionedRootSignature(&root_desc, &serialized, nullptr)) && serialized) {
    call<HRESULT>(device, 16, 1u, blob_pointer(serialized), blob_size(serialized), __uuidof(ID3D12RootSignature),
                  &root);
  }
  // The programs of test_native_draw.
  const std::vector<uint8_t> native_vs = native_only_container(
      {0x4A000002u, 0xF4080100u, 0xFA000000u, 0xBF8CC07Fu, 0xE0382000u, 0x80010100u, 0xE0382010u, 0x80010500u,
       0xBF8C3F70u, 0xF800020Fu, 0x08070605u, 0xF80008CFu, 0x04030201u, 0xBF810000u});
  const std::vector<uint8_t> native_ps = native_only_container(
      {0xC80A0002u, 0xC80E0102u, 0xC8120202u, 0xC8160302u, 0xF800080Fu, 0x05040302u, 0xBF810000u});
  struct alignas(void*) RootSubobject {
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE;
    void* value;
  };
  struct alignas(void*) ShaderSubobject {
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;
    D3D12_SHADER_BYTECODE value;
  };
  struct alignas(void*) TopologySubobject {
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY;
    D3D12_PRIMITIVE_TOPOLOGY_TYPE value = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  };
  struct alignas(void*) FormatsSubobject {
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS;
    D3D12_RT_FORMAT_ARRAY value{{DXGI_FORMAT_R8G8B8A8_UNORM}, 1};
  };
  struct Stream {
    RootSubobject root;
    ShaderSubobject vs;
    ShaderSubobject ps;
    TopologySubobject topology;
    FormatsSubobject formats;
  } stream{};
  stream.root.value = root;
  stream.vs = {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS, {native_vs.data(), native_vs.size()}};
  stream.ps = {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, {native_ps.data(), native_ps.size()}};
  D3D12_PIPELINE_STATE_STREAM_DESC stream_desc{sizeof(stream), &stream};
  if (root) call<HRESULT>(device, 86, &stream_desc, __uuidof(ID3D12PipelineState), &pipeline);
  struct alignas(void*) RasterizerSubobject {
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER;
    D3D12_RASTERIZER_DESC value{D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE, FALSE, 0, 0.0f, 0.0f, TRUE, FALSE,
                                FALSE, 0, D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF};
  };
  struct OtherStream {
    Stream base;
    RasterizerSubobject rasterizer;
  } other{stream, {}};
  D3D12_PIPELINE_STATE_STREAM_DESC other_desc{sizeof(other), &other};
  if (root) call<HRESULT>(device, 86, &other_desc, __uuidof(ID3D12PipelineState), &other_pipeline);

  D3D12_HEAP_PROPERTIES local{};
  local.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC texture{};
  texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  texture.Width = 64;
  texture.Height = 64;
  texture.DepthOrArraySize = 1;
  texture.MipLevels = 1;
  texture.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  texture.SampleDesc.Count = 1;
  texture.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  call<HRESULT>(device, 127, &local, D3D12_HEAP_FLAG_NONE, &texture, static_cast<UINT>(D3D12_BARRIER_LAYOUT_RENDER_TARGET),
                static_cast<const D3D12_CLEAR_VALUE*>(nullptr), static_cast<void*>(nullptr), 0u,
                static_cast<const void*>(nullptr), __uuidof(ID3D12Resource), &target);
  // Vertices, constant buffers and the viewed buffers in title memory.
  constexpr SIZE_T kMemory = 0x100000;
  auto* memory = static_cast<uint8_t*>(VirtualAlloc(nullptr, kMemory, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
  if (!list || !fence || !heap || !targets || !root || !pipeline || !other_pipeline || !target || !memory) {
    check(false, "the objects for measuring draw recording exist");
    return;
  }
  const float vertices[3][8] = {{-1, -1, 0, 1, 0.25f, 0.5f, 0.75f, 1},
                                {-1, 3, 0, 1, 0.25f, 0.5f, 0.75f, 1},
                                {3, -1, 0, 1, 0.25f, 0.5f, 0.75f, 1}};
  std::memcpy(memory, vertices, sizeof(vertices));
  const auto base = reinterpret_cast<uint64_t>(memory);
  struct {
    D3D12_SHADER_RESOURCE_VIEW_DESC srv;
    uint64_t address;
  } view{};
  view.srv.Format = DXGI_FORMAT_R32_TYPELESS;
  view.srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
  view.srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  view.srv.Buffer.NumElements = 1024;
  view.srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
  D3D12_RESOURCE_DESC viewed{};
  viewed.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  viewed.Width = 4096;
  viewed.Height = viewed.DepthOrArraySize = viewed.MipLevels = 1;
  viewed.SampleDesc.Count = 1;
  viewed.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  const uint64_t first_view = call<uint64_t>(heap, 9);
  for (UINT i = 0; i < kViews; ++i) {
    view.address = base + 0x10000 + i * 0x1000ull;
    call<void>(device, 70, &viewed, &view, first_view + i * 32ull);
  }
  const uint64_t rtv = call<uint64_t>(targets, 9);
  call<void>(device, 20, target, static_cast<const D3D12_RENDER_TARGET_VIEW_DESC*>(nullptr), rtv);

  constexpr uint32_t kDraws = 20000;
  bool recorded = call<HRESULT>(list, 139, allocator, static_cast<void*>(nullptr), UINT64{0}) == S_OK;
  void* heaps[] = {heap};
  call<void>(list, 32, 1u, static_cast<void* const*>(heaps));
  call<void>(list, 25, pipeline);
  call<void>(list, 34, root);
  call<void>(list, 36, 0u, call<uint64_t>(heap, 10));
  call<void>(list, 20, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  const D3D12_VERTEX_BUFFER_VIEW vertex_buffer{base, sizeof(vertices), 32};
  call<void>(list, 48, 0u, 1u, &vertex_buffer);
  const D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle{static_cast<SIZE_T>(rtv)};
  call<void>(list, 50, 1u, &rtv_handle, FALSE, static_cast<const D3D12_CPU_DESCRIPTOR_HANDLE*>(nullptr));
  const D3D12_VIEWPORT viewport{0, 0, 64, 64, 0, 1};
  const D3D12_RECT scissor{0, 0, 64, 64};
  call<void>(list, 21, 1u, &viewport);
  call<void>(list, 22, 1u, &scissor);
  LARGE_INTEGER frequency{}, start{}, end{};
  QueryPerformanceFrequency(&frequency);
  QueryPerformanceCounter(&start);
  for (uint32_t i = 0; i < kDraws; ++i) {
    const uint32_t constants[4] = {i, i * 3, i * 5, i * 7};
    call<void>(list, 40, 2u, 4u, static_cast<const void*>(constants), 0u);
    call<void>(list, 42, 1u, base + 0x8000 + (i % 64) * 256ull);
    call<void>(list, 12, 3u, 1u, 0u, 0u);
  }
  QueryPerformanceCounter(&end);
  const double per_draw_us =
      static_cast<double>(end.QuadPart - start.QuadPart) * 1e6 / static_cast<double>(frequency.QuadPart) / kDraws;
  const uint64_t table = call<uint64_t>(heap, 10);
  QueryPerformanceCounter(&start);
  for (uint32_t i = 0; i < kDraws; ++i) {
    call<void>(list, 25, (i & 1) ? other_pipeline : pipeline);
    call<void>(list, 36, 0u, table);
    const uint32_t constants[4] = {i, i * 3, i * 5, i * 7};
    call<void>(list, 40, 2u, 4u, static_cast<const void*>(constants), 0u);
    call<void>(list, 42, 1u, base + 0x8000 + (i % 64) * 256ull);
    call<void>(list, 12, 3u, 1u, 0u, 0u);
  }
  QueryPerformanceCounter(&end);
  recorded = recorded && call<HRESULT>(list, 9) == S_OK;
  const double per_changing_draw_us =
      static_cast<double>(end.QuadPart - start.QuadPart) * 1e6 / static_cast<double>(frequency.QuadPart) / kDraws;
  void* lists[] = {list};
  call<void>(queue, 35, 1u, static_cast<void* const*>(lists), UINT64{0}, 0u);
  call<HRESULT>(queue, 16, fence, UINT64{1});
  const auto* value = call<const volatile uint64_t*>(fence, 12);
  for (int i = 0; value && *value < 1 && i < 10000; ++i) Sleep(1);
  std::printf("    (recording a draw of native programs: %.2f us of CPU time; %.2f us with the pipeline and table set "
              "again; over %u draws each; fence %llu)\n",
              per_draw_us, per_changing_draw_us, kDraws, static_cast<unsigned long long>(value ? *value : 0));
  check(recorded && value && *value == 1 && per_draw_us < 20.0 && per_changing_draw_us < 20.0,
        "a draw of native programs with root arguments records in well under 20 us and runs");

  call<ULONG>(target, 2);
  call<ULONG>(other_pipeline, 2);
  call<ULONG>(pipeline, 2);
  call<ULONG>(root, 2);
  call<ULONG>(targets, 2);
  call<ULONG>(heap, 2);
  call<ULONG>(fence, 2);
  call<ULONG>(list, 2);
  call<ULONG>(allocator, 2);
  VirtualFree(memory, 0, MEM_RELEASE);
}

// Committed buffers the CPU reaches, and copies: an upload buffer's GPU
// address is the memory Map gives (as on the console); a buffer copy into a
// readback buffer shows through Map, as does a native program's store at the
// readback buffer's GPU address; texture region copies take an upload buffer
// into a texture and the texture into the readback buffer.
void test_committed_buffers_and_copies(void* device, void* queue) {
  void* allocator = nullptr;
  void* list = nullptr;
  void* fence = nullptr;
  void* root = nullptr;
  void* pipeline = nullptr;
  void* upload = nullptr;
  void* readback = nullptr;
  void* texture = nullptr;
  const uint32_t list_desc[4] = {0, 2, 1, 0};
  call<HRESULT>(device, 9, D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), &allocator);
  if (allocator) call<HRESULT>(device, 73, list_desc, allocator, static_cast<void*>(nullptr), kListIid, &list);
  call<HRESULT>(device, 36, UINT64{0}, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), &fence);
  D3D12_ROOT_PARAMETER1 parameter{};
  parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  parameter.Constants = {0, 0, 4};
  D3D12_VERSIONED_ROOT_SIGNATURE_DESC root_desc{};
  root_desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
  root_desc.Desc_1_1.NumParameters = 1;
  root_desc.Desc_1_1.pParameters = &parameter;
  ID3DBlob* serialized = nullptr;
  if (SUCCEEDED(D3D12SerializeVersionedRootSignature(&root_desc, &serialized, nullptr)) && serialized) {
    call<HRESULT>(device, 16, 1u, blob_pointer(serialized), blob_size(serialized), __uuidof(ID3D12RootSignature),
                  &root);
  }
  // The program of test_root_arguments_outlive_later_recording: stores the
  // third constant at the address in the first two.
  const std::vector<uint8_t> container = native_only_container(
      {0xF4080100u, 0xFA000000u, 0xBF8CC07Fu, 0xBE940304u, 0xBE950305u, 0xB0160040u, 0xBE970380u, 0x7E020206u,
       0x7E000280u, 0xE0700000u, 0x80050100u, 0xBF810000u});
  D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
  pso.CS = {container.data(), container.size()};
  call<HRESULT>(device, 62, &pso, static_cast<void*>(nullptr), static_cast<void*>(nullptr),
                __uuidof(ID3D12PipelineState), &pipeline);
  D3D12_RESOURCE_DESC buffer{};
  buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  buffer.Width = 0x10000;
  buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
  buffer.SampleDesc.Count = 1;
  buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  D3D12_HEAP_PROPERTIES upload_heap{};
  upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
  D3D12_HEAP_PROPERTIES readback_heap{};
  readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
  call<HRESULT>(device, 27, &upload_heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_GENERIC_READ,
                static_cast<const D3D12_CLEAR_VALUE*>(nullptr), __uuidof(ID3D12Resource), &upload);
  call<HRESULT>(device, 27, &readback_heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST,
                static_cast<const D3D12_CLEAR_VALUE*>(nullptr), __uuidof(ID3D12Resource), &readback);
  D3D12_HEAP_PROPERTIES local{};
  local.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC image{};
  image.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  image.Width = 16;
  image.Height = 16;
  image.DepthOrArraySize = 1;
  image.MipLevels = 1;
  image.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  image.SampleDesc.Count = 1;
  call<HRESULT>(device, 127, &local, D3D12_HEAP_FLAG_NONE, &image, static_cast<UINT>(D3D12_BARRIER_LAYOUT_COPY_DEST),
                static_cast<const D3D12_CLEAR_VALUE*>(nullptr), static_cast<void*>(nullptr), 0u,
                static_cast<const void*>(nullptr), __uuidof(ID3D12Resource), &texture);
  uint32_t* up = nullptr;
  uint32_t* back = nullptr;
  if (upload) call<HRESULT>(upload, 8, 0u, static_cast<const D3D12_RANGE*>(nullptr), reinterpret_cast<void**>(&up));
  if (readback) call<HRESULT>(readback, 8, 0u, static_cast<const D3D12_RANGE*>(nullptr), reinterpret_cast<void**>(&back));
  auto writable = [](const void* p) {
    MEMORY_BASIC_INFORMATION info{};
    return p && VirtualQuery(p, &info, sizeof(info)) && info.State == MEM_COMMIT && (info.Protect & PAGE_READWRITE);
  };
  if (!list || !fence || !root || !pipeline || !upload || !readback || !texture) {
    check(false, "the objects for committed buffers and copies exist");
    return;
  }
  const uint64_t upload_va = call<uint64_t>(upload, 11);
  const uint64_t readback_va = call<uint64_t>(readback, 11);
  check(writable(up) && writable(back) && upload_va == reinterpret_cast<uint64_t>(up) &&
            readback_va == reinterpret_cast<uint64_t>(back),
        "a CPU-visible committed buffer is memory the CPU writes, at its GPU address");
  if (!writable(up) || !writable(back)) return;
  for (uint32_t i = 0; i < 64; ++i) up[i] = 0xC0DE0000u + i;
  // Rows of 16 texels (64 bytes) at a 256-byte pitch, from +0x1000.
  for (uint32_t row = 0; row < 16; ++row) {
    for (uint32_t x = 0; x < 16; ++x) up[(0x1000 + row * 256) / 4 + x] = (row << 8) | x | 0xFF000000u;
  }
  bool recorded = call<HRESULT>(list, 139, allocator, static_cast<void*>(nullptr), UINT64{0}) == S_OK;
  call<void>(list, 15, readback, UINT64{0}, upload, UINT64{0}, UINT64{256});
  D3D12_TEXTURE_COPY_LOCATION from_rows{};
  from_rows.pResource = static_cast<ID3D12Resource*>(upload);
  from_rows.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  from_rows.PlacedFootprint = {0x1000, {DXGI_FORMAT_R8G8B8A8_UNORM, 16, 16, 1, 256}};
  D3D12_TEXTURE_COPY_LOCATION in_texture{};
  in_texture.pResource = static_cast<ID3D12Resource*>(texture);
  in_texture.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  call<void>(list, 16, &in_texture, 0u, 0u, 0u, &from_rows, static_cast<const D3D12_BOX*>(nullptr));
  D3D12_TEXTURE_BARRIER barrier{};
  barrier.SyncBefore = D3D12_BARRIER_SYNC_COPY;
  barrier.SyncAfter = D3D12_BARRIER_SYNC_COPY;
  barrier.AccessBefore = D3D12_BARRIER_ACCESS_COPY_DEST;
  barrier.AccessAfter = D3D12_BARRIER_ACCESS_COPY_SOURCE;
  barrier.LayoutBefore = D3D12_BARRIER_LAYOUT_COPY_DEST;
  barrier.LayoutAfter = D3D12_BARRIER_LAYOUT_COPY_SOURCE;
  barrier.pResource = static_cast<ID3D12Resource*>(texture);
  barrier.Subresources.IndexOrFirstMipLevel = 0xFFFFFFFFu;
  D3D12_BARRIER_GROUP group{};
  group.Type = D3D12_BARRIER_TYPE_TEXTURE;
  group.NumBarriers = 1;
  group.pTextureBarriers = &barrier;
  call<void>(list, 146, 1u, static_cast<const D3D12_BARRIER_GROUP*>(&group));
  D3D12_TEXTURE_COPY_LOCATION to_rows = from_rows;
  to_rows.pResource = static_cast<ID3D12Resource*>(readback);
  call<void>(list, 16, &to_rows, 0u, 0u, 0u, &in_texture, static_cast<const D3D12_BOX*>(nullptr));
  const uint32_t constants[4] = {static_cast<uint32_t>(readback_va + 0x800), static_cast<uint32_t>(readback_va >> 32),
                                 0x57012Eu, 0};
  call<void>(list, 33, root);
  call<void>(list, 39, 0u, 4u, static_cast<const void*>(constants), 0u);
  call<void>(list, 25, pipeline);
  call<void>(list, 14, 1u, 1u, 1u);
  recorded = recorded && call<HRESULT>(list, 9) == S_OK;
  void* lists[] = {list};
  call<void>(queue, 35, 1u, static_cast<void* const*>(lists), UINT64{0}, 0u);
  call<HRESULT>(queue, 16, fence, UINT64{1});
  const auto* value = call<const volatile uint64_t*>(fence, 12);
  for (int i = 0; value && *value < 1 && i < 5000; ++i) Sleep(1);
  bool copied = true, rows = true;
  for (uint32_t i = 0; i < 64; ++i) copied = copied && back[i] == 0xC0DE0000u + i;
  for (uint32_t row = 0; row < 16; ++row) {
    for (uint32_t x = 0; x < 16; ++x) rows = rows && back[(0x1000 + row * 256) / 4 + x] == ((row << 8) | x | 0xFF000000u);
  }
  std::printf("    (readback: %08x %08x, a row texel %08x, stored %08x; fence %llu)\n", back[0], back[63],
              back[(0x1000 + 5 * 256) / 4 + 7], back[0x800 / 4], static_cast<unsigned long long>(value ? *value : 0));
  check(recorded && value && *value == 1 && copied, "a buffer copy into a readback buffer shows through Map");
  check(rows, "texture region copies take rows into a texture and back out into a buffer");
  check(back[0x800 / 4] == 0x57012Eu, "native code writes a committed buffer through its GPU address");

  call<ULONG>(texture, 2);
  call<ULONG>(readback, 2);
  call<ULONG>(upload, 2);
  call<ULONG>(pipeline, 2);
  call<ULONG>(root, 2);
  call<ULONG>(fence, 2);
  call<ULONG>(list, 2);
  call<ULONG>(allocator, 2);
}

// Copies of title memory in video memory (page_cache.h; HOLY_BUFFER_COPIES
// sets them going, else this is not run): a native program reads 256 times
// 16 bytes per thread over 1 MiB of a buffer viewed in the bound table, in
// title memory the host watches writes to. A first submission starts the copies, the next
// reads them (timed against the first), and what the title and its GPU work
// write stays seen: the CPU's writes after the copy, a native store read back
// later in the same list, and a buffer copy into the memory.
void test_memory_copies(void* device, void* queue) {
  using CounterFn = volatile int64_t* (*)(const char*);
  const HMODULE runtime = GetModuleHandleW(L"holyrt.dll");
  const auto counter = runtime ? reinterpret_cast<CounterFn>(GetProcAddress(runtime, "HolyCounter")) : nullptr;
  volatile int64_t* cached = counter ? counter("d3d12.cached_pages") : nullptr;
  wchar_t budget[16] = {};
  if (!GetEnvironmentVariableW(L"HOLY_BUFFER_COPIES", budget, 16) || budget[0] == L'0' || !cached) {
    std::printf("    (copies of title memory in video memory are off: not run)\n");
    return;
  }
  void* allocator = nullptr;
  void* list = nullptr;
  void* fence = nullptr;
  void* heap = nullptr;
  void* root = nullptr;
  void* reader = nullptr;
  void* writer = nullptr;
  void* placed = nullptr;
  void* source = nullptr;
  const uint32_t list_desc[4] = {0, 2, 1, 0};
  call<HRESULT>(device, 9, D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), &allocator);
  if (allocator) call<HRESULT>(device, 73, list_desc, allocator, static_cast<void*>(nullptr), kListIid, &list);
  call<HRESULT>(device, 36, UINT64{0}, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), &fence);
  D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,
                                       1};
  call<HRESULT>(device, 14, &heap_desc, __uuidof(ID3D12DescriptorHeap), &heap);
  D3D12_DESCRIPTOR_RANGE1 range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_NONE, 0};
  D3D12_ROOT_PARAMETER1 parameters[2] = {};
  parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  parameters[0].DescriptorTable = {1, &range};
  parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  parameters[1].Constants = {0, 0, 4};
  D3D12_VERSIONED_ROOT_SIGNATURE_DESC root_desc{};
  root_desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
  root_desc.Desc_1_1.NumParameters = 2;
  root_desc.Desc_1_1.pParameters = parameters;
  ID3DBlob* serialized = nullptr;
  if (SUCCEEDED(D3D12SerializeVersionedRootSignature(&root_desc, &serialized, nullptr)) && serialized) {
    call<HRESULT>(device, 16, 1u, blob_pointer(serialized), blob_size(serialized), __uuidof(ID3D12RootSignature),
                  &root);
  }
  // The reader: 256 reads of 16 bytes per thread over a 1 MiB working set
  // (more than the first-level caches hold, less than the second level):
  // s_load_dwordx4 s[4:7], s[0:1] (s[5:6] the data's address); s[20:23] the
  // data (1 MiB + 4 KiB); s[24:27] the sums (1 KiB at +2 MiB); v1 = v0 << 4;
  // v[6:9] = 0; 256 times: s30 = 4096k, buffer_load_dwordx4 v[2:5], v1,
  // s[20:23], s30 offen, v[6:9] += v[2:5]; buffer_store_dwordx4 v[6:9], v1,
  // s[24:27] offen; s_endpgm.
  std::vector<uint32_t> code = {0xF4080100u, 0xFA000000u, 0xBF8CC07Fu, 0xBE940305u, 0xBE950306u, 0xBE9603FFu,
                                0x00101000u, 0xBE970380u, 0x8018FF05u, 0x00200000u, 0x82198006u, 0xBE9A03FFu,
                                0x00000400u, 0xBE9B0380u, 0x34020084u, 0x7E0C0280u, 0x7E0E0280u, 0x7E100280u,
                                0x7E120280u};
  for (uint32_t k = 0; k < 256; ++k) {
    for (uint32_t word : {0xBE9E03FFu, k * 4096u, 0xE0381000u, 0x1E050201u, 0xBF8C3F70u, 0x4A0C0D02u, 0x4A0E0F03u,
                          0x4A101104u, 0x4A121305u}) {
      code.push_back(word);
    }
  }
  for (uint32_t word : {0xE0781000u, 0x80060601u, 0xBF810000u}) code.push_back(word);
  const std::vector<uint8_t> reader_container = native_only_container(code);
  D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
  pso.CS = {reader_container.data(), reader_container.size()};
  call<HRESULT>(device, 62, &pso, static_cast<void*>(nullptr), static_cast<void*>(nullptr),
                __uuidof(ID3D12PipelineState), &reader);
  // The writer: stores s7 (the third constant) at the data's word 0
  // (s[20:23] the data), then reads word 0 back and stores it at +0x400
  // (word 256).
  const std::vector<uint8_t> writer_container = native_only_container(
      {0xF4080100u, 0xFA000000u, 0xBF8CC07Fu, 0xBE940305u, 0xBE950306u, 0xBE9603FFu, 0x00010000u, 0xBE970380u,
       0x7E020207u, 0x7E000280u, 0xE0700000u, 0x80050100u, 0xBF8C3F70u, 0xE0300000u, 0x80050200u, 0xBF8C3F70u,
       0xE0700400u, 0x80050200u, 0xBF810000u});
  pso.CS = {writer_container.data(), writer_container.size()};
  call<HRESULT>(device, 62, &pso, static_cast<void*>(nullptr), static_cast<void*>(nullptr),
                __uuidof(ID3D12PipelineState), &writer);
  // Data (1 MiB + 4 KiB), sums (1 KiB at +2 MiB) and timestamps (at +3 MiB)
  // in title memory the host watches; a committed upload buffer to copy from.
  constexpr uint32_t kSums = 0x200000, kStamps = 0x300000, kDataWords = 0x101000 / 4;
  auto* memory = static_cast<uint8_t*>(XMemVirtualAlloc(nullptr, 0x400000, MEM_RESERVE | MEM_COMMIT, 0x23, PAGE_READWRITE));
  D3D12_HEAP_PROPERTIES upload_heap{};
  upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
  D3D12_RESOURCE_DESC buffer{};
  buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  buffer.Width = 0x10000;
  buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
  buffer.SampleDesc.Count = 1;
  buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  call<HRESULT>(device, 27, &upload_heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_GENERIC_READ,
                static_cast<const D3D12_CLEAR_VALUE*>(nullptr), __uuidof(ID3D12Resource), &source);
  if (!list || !fence || !heap || !root || !reader || !writer || !memory || !source) {
    check(false, "the objects for copies of title memory exist");
    return;
  }
  auto* data = reinterpret_cast<volatile uint32_t*>(memory);
  for (uint32_t i = 0; i < kDataWords; ++i) data[i] = i * 2654435761u;
  const auto base = reinterpret_cast<uint64_t>(memory);
  buffer.Width = 0x400000;
  buffer.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  call<HRESULT>(device, 133, base, &buffer, static_cast<UINT>(0xFFFFFFFF), static_cast<const void*>(nullptr),
                __uuidof(ID3D12Resource), &placed);
  // The data viewed in the table (what makes it read through a binding).
  struct {
    D3D12_SHADER_RESOURCE_VIEW_DESC srv;
    uint64_t address;
  } view{};
  view.srv.Format = DXGI_FORMAT_R32_TYPELESS;
  view.srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
  view.srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  view.srv.Buffer.NumElements = kDataWords;
  view.srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
  view.address = base;
  D3D12_RESOURCE_DESC viewed = buffer;
  viewed.Width = kDataWords * 4ull;
  viewed.Flags = D3D12_RESOURCE_FLAG_NONE;
  call<void>(device, 70, &viewed, &view, call<uint64_t>(heap, 9));
  UINT64 frequency = 0;
  call<HRESULT>(queue, 18, &frequency);
  uint64_t fence_value = 0;
  double last_wall_ms = 0;
  const auto* value = call<const volatile uint64_t*>(fence, 12);
  // Records `program` (4 x 4096 groups for the reader, one group for the
  // writer) between timestamps, runs it and waits; the GPU milliseconds.
  auto run = [&](void* program, uint32_t value7, bool copy_first) {
    // The allocator is reset once the GPU is done with the list's last run
    // (not while the new list records with it).
    bool ok = (!fence_value || call<HRESULT>(allocator, 8) == S_OK) &&
              call<HRESULT>(list, 139, allocator, static_cast<void*>(nullptr), UINT64{0}) == S_OK;
    if (copy_first) call<void>(list, 15, placed, UINT64{0x40}, source, UINT64{0}, UINT64{16});
    void* heaps[] = {heap};
    call<void>(list, 32, 1u, static_cast<void* const*>(heaps));
    call<void>(list, 72, base + kStamps, UINT64{0}, UINT64{0}, 1u);
    const uint32_t constants[4] = {static_cast<uint32_t>(base), static_cast<uint32_t>(base >> 32), value7, 0};
    call<void>(list, 33, root);
    call<void>(list, 35, 0u, call<uint64_t>(heap, 10));
    call<void>(list, 39, 1u, 4u, static_cast<const void*>(constants), 0u);
    call<void>(list, 25, program);
    call<void>(list, 14, program == reader ? 256u : 1u, 1u, 1u);
    call<void>(list, 72, base + kStamps + 8, UINT64{0}, UINT64{0}, 1u);
    ok = ok && call<HRESULT>(list, 9) == S_OK;
    void* lists[] = {list};
    LARGE_INTEGER wall_start{}, wall_end{}, wall_frequency{};
    QueryPerformanceFrequency(&wall_frequency);
    QueryPerformanceCounter(&wall_start);
    call<void>(queue, 35, 1u, static_cast<void* const*>(lists), UINT64{0}, 0u);
    call<HRESULT>(queue, 16, fence, ++fence_value);
    while (value && *value < fence_value) YieldProcessor();
    QueryPerformanceCounter(&wall_end);
    last_wall_ms = static_cast<double>(wall_end.QuadPart - wall_start.QuadPart) * 1e3 /
                   static_cast<double>(wall_frequency.QuadPart);
    const auto* stamps = reinterpret_cast<const volatile uint64_t*>(memory + kStamps);
    const double ms = frequency ? static_cast<double>(stamps[1] - stamps[0]) * 1e3 / static_cast<double>(frequency) : 0;
    return ok && value && *value == fence_value ? ms : -1.0;
  };
  // Thread t's first sum: the data's words 4t + 1024k.
  auto sum_right = [&](uint32_t thread) {
    uint32_t expected = 0;
    for (uint32_t k = 0; k < 256; ++k) expected += data[thread * 4 + k * 1024];
    return *reinterpret_cast<const volatile uint32_t*>(memory + kSums + thread * 16) == expected;
  };
  const int64_t cached_before = *cached;
  const double uncached_ms = run(reader, 0, false);  // starts the copies
  const double uncached_wall = last_wall_ms;
  const bool uncached_right = sum_right(0) && sum_right(63);
  const double cached_ms = run(reader, 0, false);     // reads them
  const double cached_wall = last_wall_ms;
  const bool cached_right = sum_right(0) && sum_right(63);
  std::printf("    (wall time from submission to the fence: %.3f ms uncached, %.3f ms cached)\n", uncached_wall,
              cached_wall);
  const int64_t cached_pages = *cached - cached_before;
  // The CPU writes the data after the copy: the next run sees it.
  for (uint32_t k = 0; k < 256; k += 16) data[k * 1024] += 1000 + k;
  run(reader, 0, false);
  const bool cpu_write_seen = sum_right(0);
  // A native store to the copied page, read back by the same program after
  // it (the page's flag sends the read to the memory itself).
  run(writer, 0x5704E5u, false);
  const bool store_seen = data[0] == 0x5704E5u && data[256] == 0x5704E5u;
  // A buffer copy into the data (word 16), read in the same list after it.
  auto* from = static_cast<uint32_t*>(nullptr);
  call<HRESULT>(source, 8, 0u, static_cast<const D3D12_RANGE*>(nullptr), reinterpret_cast<void**>(&from));
  for (uint32_t i = 0; i < 4; ++i) from[i] = 0xC0910000u + i;
  run(reader, 0, true);
  const bool copy_seen = data[16] == 0xC0910000u && sum_right(4);
  std::printf("    (copies of title memory: %lld pages; uncached %.3f ms, cached %.3f ms on the GPU (%.1fx); sums %s %s; "
              "CPU write %s, copy %s, store %s)\n",
              static_cast<long long>(cached_pages), uncached_ms, cached_ms,
              cached_ms > 0 ? uncached_ms / cached_ms : 0.0, uncached_right ? "right" : "wrong",
              cached_right ? "right" : "wrong", cpu_write_seen ? "seen" : "missed", copy_seen ? "seen" : "missed",
              store_seen ? "seen" : "missed");
  check(uncached_ms > 0 && cached_ms > 0 && uncached_right && cached_right && cached_pages > 0,
        "translated reads of title memory go to its copy in video memory, and compute the same");
  check(cpu_write_seen, "the CPU's writes to copied title memory reach the GPU at the next submission");
  check(copy_seen && store_seen, "GPU writes to copied title memory are read back (copies and native stores)");

  if (placed) call<ULONG>(placed, 2);
  call<ULONG>(source, 2);
  call<ULONG>(writer, 2);
  call<ULONG>(reader, 2);
  call<ULONG>(root, 2);
  call<ULONG>(heap, 2);
  call<ULONG>(fence, 2);
  call<ULONG>(list, 2);
  call<ULONG>(allocator, 2);
  VirtualFree(memory, 0, MEM_RELEASE);
}

// Title memory released while the GPU still reads it (a title's mistake the
// console may forgive): the work finishes, later work runs, and the runtime's
// heap over the memory goes once the GPU is done (its count of released
// heaps). The reads take tens of milliseconds (test_memory_read_cost).
void test_memory_released_while_read(void* device, void* queue) {
  void* allocator = nullptr;
  void* list = nullptr;
  void* fence = nullptr;
  void* heap = nullptr;
  void* root = nullptr;
  void* pipeline = nullptr;
  void* placed = nullptr;
  const uint32_t list_desc[4] = {0, 2, 1, 0};
  call<HRESULT>(device, 9, D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), &allocator);
  if (allocator) call<HRESULT>(device, 73, list_desc, allocator, static_cast<void*>(nullptr), kListIid, &list);
  call<HRESULT>(device, 36, UINT64{0}, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), &fence);
  D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,
                                       1};
  call<HRESULT>(device, 14, &heap_desc, __uuidof(ID3D12DescriptorHeap), &heap);
  D3D12_DESCRIPTOR_RANGE1 range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_NONE, 0};
  D3D12_ROOT_PARAMETER1 parameters[2] = {};
  parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  parameters[0].DescriptorTable = {1, &range};
  parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  parameters[1].Constants = {0, 0, 2};
  D3D12_VERSIONED_ROOT_SIGNATURE_DESC root_desc{};
  root_desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
  root_desc.Desc_1_1.NumParameters = 2;
  root_desc.Desc_1_1.pParameters = parameters;
  ID3DBlob* serialized = nullptr;
  if (SUCCEEDED(D3D12SerializeVersionedRootSignature(&root_desc, &serialized, nullptr)) && serialized) {
    call<HRESULT>(device, 16, 1u, blob_pointer(serialized), blob_size(serialized), __uuidof(ID3D12RootSignature),
                  &root);
  }
  // The program of test_memory_read_cost: 16 reads of 16 bytes per thread.
  std::vector<uint32_t> code = {0xF4080100u, 0xFA000000u, 0xBF8CC07Fu, 0xBE940305u, 0xBE950306u, 0xBE9603FFu,
                                0x00010000u, 0xBE970380u, 0x8018FF05u, 0x00010000u, 0x82198006u, 0xBE9A03FFu,
                                0x00000400u, 0xBE9B0380u, 0x34020084u, 0x7E0C0280u, 0x7E0E0280u, 0x7E100280u,
                                0x7E120280u};
  for (uint32_t k = 0; k < 16; ++k) {
    for (uint32_t word : {0xE0381000u | (k * 256u), 0x80050201u, 0xBF8C3F70u, 0x4A0C0D02u, 0x4A0E0F03u, 0x4A101104u,
                          0x4A121305u}) {
      code.push_back(word);
    }
  }
  for (uint32_t word : {0xE0781000u, 0x80060601u, 0xBF810000u}) code.push_back(word);
  const std::vector<uint8_t> container = native_only_container(code);
  D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
  pso.CS = {container.data(), container.size()};
  call<HRESULT>(device, 62, &pso, static_cast<void*>(nullptr), static_cast<void*>(nullptr),
                __uuidof(ID3D12PipelineState), &pipeline);
  using CounterFn = volatile int64_t* (*)(const char*);
  const HMODULE runtime = GetModuleHandleW(L"holyrt.dll");
  const auto counter = runtime ? reinterpret_cast<CounterFn>(GetProcAddress(runtime, "HolyCounter")) : nullptr;
  volatile int64_t* released = counter ? counter("d3d12.gpu_heaps_released") : nullptr;
  auto* memory = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x20000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
  if (!list || !fence || !heap || !root || !pipeline || !memory || !released) {
    check(false, "the objects for releasing memory the GPU reads exist");
    return;
  }
  const auto base = reinterpret_cast<uint64_t>(memory);
  D3D12_RESOURCE_DESC buffer{};
  buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  buffer.Width = 0x20000;
  buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
  buffer.SampleDesc.Count = 1;
  buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  buffer.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  call<HRESULT>(device, 133, base, &buffer, static_cast<UINT>(0xFFFFFFFF), static_cast<const void*>(nullptr),
                __uuidof(ID3D12Resource), &placed);
  if (placed) call<ULONG>(placed, 2);
  bool recorded = call<HRESULT>(list, 139, allocator, static_cast<void*>(nullptr), UINT64{0}) == S_OK;
  void* heaps[] = {heap};
  call<void>(list, 32, 1u, static_cast<void* const*>(heaps));
  const uint32_t constants[2] = {static_cast<uint32_t>(base), static_cast<uint32_t>(base >> 32)};
  call<void>(list, 33, root);
  call<void>(list, 35, 0u, call<uint64_t>(heap, 10));
  call<void>(list, 39, 1u, 2u, static_cast<const void*>(constants), 0u);
  call<void>(list, 25, pipeline);
  for (uint32_t i = 0; i < 4; ++i) call<void>(list, 14, 4096u, 1u, 1u);
  recorded = recorded && call<HRESULT>(list, 9) == S_OK;
  void* lists[] = {list};
  const int64_t released_before = *released;
  call<void>(queue, 35, 1u, static_cast<void* const*>(lists), UINT64{0}, 0u);
  call<HRESULT>(queue, 16, fence, UINT64{1});
  VirtualFree(memory, 0, MEM_RELEASE);  // while the GPU reads it
  const int64_t released_at_once = *released - released_before;
  const auto* value = call<const volatile uint64_t*>(fence, 12);
  for (int i = 0; value && *value < 1 && i < 10000; ++i) Sleep(1);
  // Later work: an empty submission (which also lets go of what the GPU is
  // done with), then the fence again.
  recorded = recorded && call<HRESULT>(allocator, 8) == S_OK &&
             call<HRESULT>(list, 139, allocator, static_cast<void*>(nullptr), UINT64{0}) == S_OK &&
             call<HRESULT>(list, 9) == S_OK;
  call<void>(queue, 35, 1u, static_cast<void* const*>(lists), UINT64{0}, 0u);
  call<HRESULT>(queue, 16, fence, UINT64{2});
  for (int i = 0; value && *value < 2 && i < 10000; ++i) Sleep(1);
  const int64_t released_later = *released - released_before;
  std::printf("    (heaps let go of: %lld when the memory was released, %lld after the GPU finished; fence %llu)\n",
              static_cast<long long>(released_at_once), static_cast<long long>(released_later),
              static_cast<unsigned long long>(value ? *value : 0));
  check(recorded && value && *value == 2 && released_at_once == 0 && released_later >= 1,
        "title memory released while the GPU reads it stays until the GPU is done, and later work runs");

  call<ULONG>(pipeline, 2);
  call<ULONG>(root, 2);
  call<ULONG>(heap, 2);
  call<ULONG>(fence, 2);
  call<ULONG>(list, 2);
  call<ULONG>(allocator, 2);
}

// A recording keeps its root arguments however much is recorded after it: a
// list records one dispatch of a native program storing a root constant, a
// second list records 100,000 more (more than the runtime's memory for root
// arguments once held), and only the first runs. The runtime's memory for
// them goes back once the lists are reset (its live count).
void test_root_arguments_outlive_later_recording(void* device, void* queue) {
  void* allocators[2] = {};
  void* lists[2] = {};
  void* fence = nullptr;
  void* root = nullptr;
  void* pipeline = nullptr;
  void* placed = nullptr;
  const uint32_t list_desc[4] = {0, 2, 1, 0};
  for (int i = 0; i < 2; ++i) {
    call<HRESULT>(device, 9, D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), &allocators[i]);
    if (allocators[i]) {
      call<HRESULT>(device, 73, list_desc, allocators[i], static_cast<void*>(nullptr), kListIid, &lists[i]);
    }
  }
  call<HRESULT>(device, 36, UINT64{0}, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), &fence);
  D3D12_ROOT_PARAMETER1 parameter{};
  parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  parameter.Constants = {0, 0, 4};
  D3D12_VERSIONED_ROOT_SIGNATURE_DESC root_desc{};
  root_desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
  root_desc.Desc_1_1.NumParameters = 1;
  root_desc.Desc_1_1.pParameters = &parameter;
  ID3DBlob* serialized = nullptr;
  if (SUCCEEDED(D3D12SerializeVersionedRootSignature(&root_desc, &serialized, nullptr)) && serialized) {
    call<HRESULT>(device, 16, 1u, blob_pointer(serialized), blob_size(serialized), __uuidof(ID3D12RootSignature),
                  &root);
  }
  // s_load_dwordx4 s[4:7], s[0:1] (the constants: address, value); s[20:23]
  // a buffer of 64 bytes at s[4:5]; v1 = s6; v0 = 0; buffer_store_dword v1,
  // off, s[20:23], 0; s_endpgm.
  const std::vector<uint8_t> container = native_only_container(
      {0xF4080100u, 0xFA000000u, 0xBF8CC07Fu, 0xBE940304u, 0xBE950305u, 0xB0160040u, 0xBE970380u, 0x7E020206u,
       0x7E000280u, 0xE0700000u, 0x80050100u, 0xBF810000u});
  D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
  pso.CS = {container.data(), container.size()};
  call<HRESULT>(device, 62, &pso, static_cast<void*>(nullptr), static_cast<void*>(nullptr),
                __uuidof(ID3D12PipelineState), &pipeline);
  auto* output = static_cast<volatile uint32_t*>(VirtualAlloc(nullptr, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
  using CounterFn = volatile int64_t* (*)(const char*);
  const HMODULE runtime = GetModuleHandleW(L"holyrt.dll");
  const auto counter = runtime ? reinterpret_cast<CounterFn>(GetProcAddress(runtime, "HolyCounter")) : nullptr;
  volatile int64_t* held = counter ? counter("d3d12.ring_chunks_held") : nullptr;
  if (!lists[0] || !lists[1] || !fence || !root || !pipeline || !output || !held) {
    check(false, "the objects for recording past the root argument memory exist");
    return;
  }
  const auto address = reinterpret_cast<uint64_t>(output);
  D3D12_RESOURCE_DESC buffer{};
  buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  buffer.Width = 0x10000;
  buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
  buffer.SampleDesc.Count = 1;
  buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  buffer.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  call<HRESULT>(device, 133, address, &buffer, static_cast<UINT>(0xFFFFFFFF), static_cast<const void*>(nullptr),
                __uuidof(ID3D12Resource), &placed);
  const int64_t held_before = *held;
  auto record = [&](void* list, void* allocator, uint32_t first_value, uint32_t dispatches) {
    bool ok = call<HRESULT>(list, 139, allocator, static_cast<void*>(nullptr), UINT64{0}) == S_OK;
    call<void>(list, 33, root);
    call<void>(list, 25, pipeline);
    for (uint32_t i = 0; i < dispatches; ++i) {
      const uint32_t constants[4] = {static_cast<uint32_t>(address), static_cast<uint32_t>(address >> 32),
                                     first_value + i, 0};
      call<void>(list, 39, 0u, 4u, static_cast<const void*>(constants), 0u);
      call<void>(list, 14, 1u, 1u, 1u);
    }
    return ok && call<HRESULT>(list, 9) == S_OK;
  };
  bool recorded = record(lists[0], allocators[0], 0xA11CEu, 1);
  recorded = record(lists[1], allocators[1], 1u, 100000) && recorded;
  const int64_t held_recorded = *held - held_before;
  void* first[] = {lists[0]};
  call<void>(queue, 35, 1u, static_cast<void* const*>(first), UINT64{0}, 0u);
  call<HRESULT>(queue, 16, fence, UINT64{1});
  const auto* value = call<const volatile uint64_t*>(fence, 12);
  for (int i = 0; value && *value < 1 && i < 5000; ++i) Sleep(1);
  const uint32_t stored = output[0];
  // Reset both (the GPU is done with the first; the second never ran), then
  // record nothing: the memory goes back.
  for (int i = 0; i < 2; ++i) {
    call<HRESULT>(allocators[i], 8);
    call<HRESULT>(lists[i], 139, allocators[i], static_cast<void*>(nullptr), UINT64{0});
    call<HRESULT>(lists[i], 9);
  }
  const int64_t held_after = *held - held_before;
  std::printf("    (stored %08x; root argument memory held: %lld MiB recorded, %lld MiB after the resets; fence %llu)\n",
              stored, static_cast<long long>(held_recorded), static_cast<long long>(held_after),
              static_cast<unsigned long long>(value ? *value : 0));
  check(recorded && value && *value == 1 && stored == 0xA11CEu,
        "a recording keeps its root arguments however much is recorded after it");
  check(held_recorded > 64 && held_after == 0, "root argument memory goes back when the lists are reset");

  if (placed) call<ULONG>(placed, 2);
  call<ULONG>(pipeline, 2);
  call<ULONG>(root, 2);
  call<ULONG>(fence, 2);
  for (int i = 0; i < 2; ++i) {
    call<ULONG>(lists[i], 2);
    call<ULONG>(allocators[i], 2);
  }
  VirtualFree(const_cast<uint32_t*>(output), 0, MEM_RELEASE);
}

// What translated memory reads cost the GPU against the desktop's: a native
// compute program and the same program in HLSL each read 256 times 16 bytes
// per thread over a 1 MiB working set (more than the first-level caches hold)
// and store the sums, 256 groups of 64 between GPU timestamps. The data is
// reached through root constants only, so it is never copied into video
// memory: both read title memory. Both sums must agree; the times are printed
// (the ratio is the cost of the translated access path).
void test_memory_read_cost(void* device, void* queue) {
  void* allocator = nullptr;
  void* list = nullptr;
  void* fence = nullptr;
  void* heap = nullptr;
  void* native_root = nullptr;
  void* desktop_root = nullptr;
  void* native = nullptr;
  void* desktop = nullptr;
  void* placed = nullptr;
  const uint32_t list_desc[4] = {0, 2, 1, 0};
  call<HRESULT>(device, 9, D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), &allocator);
  if (allocator) call<HRESULT>(device, 73, list_desc, allocator, static_cast<void*>(nullptr), kListIid, &list);
  call<HRESULT>(device, 36, UINT64{0}, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), &fence);
  D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,
                                       1};
  call<HRESULT>(device, 14, &heap_desc, __uuidof(ID3D12DescriptorHeap), &heap);
  // Native: a table (unused) and two constants, the memory's address.
  D3D12_DESCRIPTOR_RANGE1 range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_NONE, 0};
  D3D12_ROOT_PARAMETER1 parameters[2] = {};
  parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  parameters[0].DescriptorTable = {1, &range};
  parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  parameters[1].Constants = {0, 0, 2};
  D3D12_VERSIONED_ROOT_SIGNATURE_DESC root_desc{};
  root_desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
  root_desc.Desc_1_1.NumParameters = 2;
  root_desc.Desc_1_1.pParameters = parameters;
  ID3DBlob* serialized = nullptr;
  if (SUCCEEDED(D3D12SerializeVersionedRootSignature(&root_desc, &serialized, nullptr)) && serialized) {
    call<HRESULT>(device, 16, 1u, blob_pointer(serialized), blob_size(serialized), __uuidof(ID3D12RootSignature),
                  &native_root);
  }
  // Desktop: the data as a root view, the sums as a root UAV.
  D3D12_ROOT_PARAMETER1 views[2] = {};
  views[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
  views[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  root_desc.Desc_1_1.pParameters = views;
  serialized = nullptr;
  if (SUCCEEDED(D3D12SerializeVersionedRootSignature(&root_desc, &serialized, nullptr)) && serialized) {
    call<HRESULT>(device, 16, 1u, blob_pointer(serialized), blob_size(serialized), __uuidof(ID3D12RootSignature),
                  &desktop_root);
  }
  // s_load_dwordx4 s[4:7], s[0:1] (the root table: s[5:6] the address);
  // s[20:23] the data (64 KiB at it), s[24:27] the sums (1 KiB at +64 KiB);
  // v1 = v0 << 4; v[6:9] = 0; 16 times: buffer_load_dwordx4 v[2:5], v1,
  // s[20:23] offen offset:256k, then v[6:9] += v[2:5]; buffer_store_dwordx4
  // v[6:9], v1, s[24:27] offen; s_endpgm.
  // The reader of test_memory_copies: s[20:23] the data (1 MiB + 4 KiB),
  // s[24:27] the sums (at +2 MiB), 256 reads at offsets 4096k.
  std::vector<uint32_t> code = {0xF4080100u, 0xFA000000u, 0xBF8CC07Fu, 0xBE940305u, 0xBE950306u, 0xBE9603FFu,
                                0x00101000u, 0xBE970380u, 0x8018FF05u, 0x00200000u, 0x82198006u, 0xBE9A03FFu,
                                0x00000400u, 0xBE9B0380u, 0x34020084u, 0x7E0C0280u, 0x7E0E0280u, 0x7E100280u,
                                0x7E120280u};
  for (uint32_t k = 0; k < 256; ++k) {
    for (uint32_t word : {0xBE9E03FFu, k * 4096u, 0xE0381000u, 0x1E050201u, 0xBF8C3F70u, 0x4A0C0D02u, 0x4A0E0F03u,
                          0x4A101104u, 0x4A121305u}) {
      code.push_back(word);
    }
  }
  for (uint32_t word : {0xE0781000u, 0x80060601u, 0xBF810000u}) code.push_back(word);
  const std::vector<uint8_t> container = native_only_container(code);
  D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
  pso.CS = {container.data(), container.size()};
  call<HRESULT>(device, 62, &pso, static_cast<void*>(nullptr), static_cast<void*>(nullptr),
                __uuidof(ID3D12PipelineState), &native);
  HMODULE compiler = LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  using CompileFn = HRESULT(WINAPI*)(const void*, SIZE_T, const char*, const void*, void*, const char*, const char*,
                                     UINT, UINT, void**, void**);
  const auto compile = compiler ? reinterpret_cast<CompileFn>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
  const char source[] =
      "ByteAddressBuffer data : register(t0); RWByteAddressBuffer sums : register(u0);"
      "[numthreads(64,1,1)] void main(uint3 t : SV_GroupThreadID) { uint4 sum = 0;"
      "[loop] for (uint k = 0; k < 256; ++k) sum += data.Load4(t.x * 16 + k * 4096);"
      "sums.Store4(t.x * 16, sum); }";
  void* shader = nullptr;
  if (compile) compile(source, sizeof(source) - 1, nullptr, nullptr, nullptr, "main", "cs_5_1", 0, 0, &shader, nullptr);
  if (shader && desktop_root) {
    D3D12_COMPUTE_PIPELINE_STATE_DESC desktop_pso{};
    desktop_pso.pRootSignature = static_cast<ID3D12RootSignature*>(desktop_root);
    desktop_pso.CS = {blob_pointer(shader), blob_size(shader)};
    call<HRESULT>(device, 62, &desktop_pso, static_cast<void*>(nullptr), static_cast<void*>(nullptr),
                  __uuidof(ID3D12PipelineState), &desktop);
  }
  // Data at +0, native sums at +64 KiB, desktop sums at +128 KiB, timestamps
  // at +192 KiB.
  // Data at +0 (1 MiB + 4 KiB), native sums at +2 MiB, desktop sums at
  // +2 MiB + 64 KiB, timestamps at +3 MiB.
  constexpr uint32_t kDataWords = 0x101000 / 4, kSums = 0x200000, kDesktopSums = 0x210000, kStamps = 0x300000;
  auto* memory = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x400000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
  if (!list || !fence || !heap || !native_root || !desktop_root || !native || !desktop || !memory) {
    check(false, "the objects for measuring translated memory reads exist");
    return;
  }
  auto* data = reinterpret_cast<uint32_t*>(memory);
  for (uint32_t i = 0; i < kDataWords; ++i) data[i] = i * 2654435761u;
  const auto base = reinterpret_cast<uint64_t>(memory);
  D3D12_RESOURCE_DESC buffer{};
  buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  buffer.Width = 0x400000;
  buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
  buffer.SampleDesc.Count = 1;
  buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  buffer.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  call<HRESULT>(device, 133, base, &buffer, static_cast<UINT>(0xFFFFFFFF), static_cast<const void*>(nullptr),
                __uuidof(ID3D12Resource), &placed);
  const uint64_t stamps = base + kStamps;
  D3D12_GLOBAL_BARRIER global{D3D12_BARRIER_SYNC_COMPUTE_SHADING, D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                              D3D12_BARRIER_ACCESS_UNORDERED_ACCESS, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS};
  D3D12_BARRIER_GROUP group{};
  group.Type = D3D12_BARRIER_TYPE_GLOBAL;
  group.NumBarriers = 1;
  group.pGlobalBarriers = &global;
  constexpr uint32_t kRepeats = 1, kGroups = 256;
  bool recorded = call<HRESULT>(list, 139, allocator, static_cast<void*>(nullptr), UINT64{0}) == S_OK;
  void* heaps[] = {heap};
  call<void>(list, 32, 1u, static_cast<void* const*>(heaps));
  call<void>(list, 72, stamps, UINT64{0}, UINT64{0}, 1u);
  const uint32_t constants[2] = {static_cast<uint32_t>(base), static_cast<uint32_t>(base >> 32)};
  call<void>(list, 33, native_root);
  call<void>(list, 35, 0u, call<uint64_t>(heap, 10));
  call<void>(list, 39, 1u, 2u, static_cast<const void*>(constants), 0u);
  call<void>(list, 25, native);
  for (uint32_t i = 0; i < kRepeats; ++i) call<void>(list, 14, kGroups, 1u, 1u);
  call<void>(list, 146, 1u, static_cast<const D3D12_BARRIER_GROUP*>(&group));
  call<void>(list, 72, stamps + 8, UINT64{0}, UINT64{0}, 1u);
  call<void>(list, 33, desktop_root);
  call<void>(list, 43, 0u, base);
  call<void>(list, 45, 1u, base + kDesktopSums);
  call<void>(list, 25, desktop);
  for (uint32_t i = 0; i < kRepeats; ++i) call<void>(list, 14, kGroups, 1u, 1u);
  call<void>(list, 146, 1u, static_cast<const D3D12_BARRIER_GROUP*>(&group));
  call<void>(list, 72, stamps + 16, UINT64{0}, UINT64{0}, 1u);
  recorded = recorded && call<HRESULT>(list, 9) == S_OK;
  void* lists[] = {list};
  call<void>(queue, 35, 1u, static_cast<void* const*>(lists), UINT64{0}, 0u);
  call<HRESULT>(queue, 16, fence, UINT64{1});
  const auto* value = call<const volatile uint64_t*>(fence, 12);
  for (int i = 0; value && *value < 1 && i < 10000; ++i) Sleep(1);
  UINT64 frequency = 0;
  call<HRESULT>(queue, 18, &frequency);
  const auto* times = reinterpret_cast<const volatile uint64_t*>(memory + kStamps);
  const double native_ms = frequency ? static_cast<double>(times[1] - times[0]) * 1e3 / static_cast<double>(frequency) : 0;
  const double desktop_ms = frequency ? static_cast<double>(times[2] - times[1]) * 1e3 / static_cast<double>(frequency) : 0;
  const bool same = std::memcmp(memory + kSums, memory + kDesktopSums, 0x400) == 0;
  uint32_t expected = 0;
  for (uint32_t k = 0; k < 256; ++k) expected += data[k * 1024];
  const uint32_t first_sum = *reinterpret_cast<const volatile uint32_t*>(memory + kSums);
  std::printf("    (256 reads of 16 bytes per thread over 1 MiB, %u x %u groups of 64: translated %.3f ms, desktop %.3f ms on the "
              "GPU, %.1fx; sums %s; fence %llu)\n",
              kRepeats, kGroups, native_ms, desktop_ms, desktop_ms > 0 ? native_ms / desktop_ms : 0.0,
              same && first_sum == expected ? "agree" : "differ", static_cast<unsigned long long>(value ? *value : 0));
  check(recorded && value && *value == 1 && same && first_sum == expected,
        "a native program reading title memory computes what the same desktop program does");

  if (placed) call<ULONG>(placed, 2);
  call<ULONG>(desktop, 2);
  call<ULONG>(native, 2);
  call<ULONG>(desktop_root, 2);
  call<ULONG>(native_root, 2);
  call<ULONG>(heap, 2);
  call<ULONG>(fence, 2);
  call<ULONG>(list, 2);
  call<ULONG>(allocator, 2);
  VirtualFree(memory, 0, MEM_RELEASE);
}

// Pipelines from native descriptions (device slot 57). `stencil` false: the
// render state tests and writes depth (DB_DEPTH_CONTROL: less), and a near
// triangle drawn before a far one keeps its colour. True: a first pipeline
// replaces the stencil value (DB_STENCIL_CONTROL, reference 1), a second
// tests it for equality with 0, and the first's colour stays.
void test_native_depth(void* device, void* queue, bool stencil) {
  void* allocator = nullptr;
  void* list = nullptr;
  void* fence = nullptr;
  void* heap = nullptr;
  void* targets = nullptr;
  void* depths = nullptr;
  void* compute_root = nullptr;
  void* draw_pipeline = nullptr;
  void* second_pipeline = nullptr;
  void* read_pipeline = nullptr;
  void* target = nullptr;
  void* depth = nullptr;
  void* placed = nullptr;
  const uint32_t list_desc[4] = {0, 2, 1, 0};
  call<HRESULT>(device, 9, D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), &allocator);
  if (allocator) call<HRESULT>(device, 73, list_desc, allocator, static_cast<void*>(nullptr), kListIid, &list);
  call<HRESULT>(device, 36, UINT64{0}, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), &fence);
  D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 16,
                                       D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 1};
  call<HRESULT>(device, 14, &heap_desc, __uuidof(ID3D12DescriptorHeap), &heap);
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 1};
  call<HRESULT>(device, 14, &rtv_desc, __uuidof(ID3D12DescriptorHeap), &targets);
  const DXGI_FORMAT depth_format = stencil ? DXGI_FORMAT_D24_UNORM_S8_UINT : DXGI_FORMAT_D32_FLOAT;
  D3D12_DESCRIPTOR_HEAP_DESC dsv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 1};
  call<HRESULT>(device, 14, &dsv_desc, __uuidof(ID3D12DescriptorHeap), &depths);
  D3D12_DESCRIPTOR_RANGE1 range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_NONE, 0};
  D3D12_ROOT_PARAMETER1 parameters[2] = {};
  parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  parameters[0].DescriptorTable = {1, &range};
  parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  parameters[1].Constants = {0, 0, 2};
  D3D12_VERSIONED_ROOT_SIGNATURE_DESC read_desc{};
  read_desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
  read_desc.Desc_1_1.NumParameters = 2;
  read_desc.Desc_1_1.pParameters = parameters;
  ID3DBlob* serialized = nullptr;
  if (SUCCEEDED(D3D12SerializeVersionedRootSignature(&read_desc, &serialized, nullptr)) && serialized) {
    call<HRESULT>(device, 16, 1u, blob_pointer(serialized), blob_size(serialized), __uuidof(ID3D12RootSignature),
                  &compute_root);
  }
  // The vertex and pixel programs of test_native_draw, raw.
  const uint32_t vs_code[] = {0x4A000002u, 0xF4080100u, 0xFA000000u, 0xBF8CC07Fu, 0xE0382000u,
                              0x80010100u, 0xE0382010u, 0x80010500u, 0xBF8C3F70u, 0xF800020Fu,
                              0x08070605u, 0xF80008CFu, 0x04030201u, 0xBF810000u};
  const uint32_t ps_code[] = {0xC80A0002u, 0xC80E0102u, 0xC8120202u, 0xC8160302u, 0xF800080Fu, 0x05040302u, 0xBF810000u};
  uint32_t meta[12] = {};
  uint32_t state[0x17C / 4] = {};
  state[0x1D] = 0x060201BCu;
  state[0x1E] = 0xF;           // target 0 written
  // Depth tested (less) and written; or (stencil) the stencil test always
  // passing and replacing the value with the reference, masks 0xFF.
  state[0x2E] = stencil ? 0x1u | (7u << 8) | (7u << 20) : 0x2u | 0x4u | (1u << 4);
  state[0x2F] = stencil ? 0x30030u : 0u;
  state[0x30] = state[0x31] = stencil ? 0x1FFFF00u : 0u;
  state[0x34] = 0x80000000u;   // no blending
  uint32_t pipeline_block[0xBC / 4] = {};
  pipeline_block[4] = DXGI_FORMAT_R8G8B8A8_UNORM;
  pipeline_block[7] = 0xFFFFFFFFu;
  pipeline_block[9] = 0x00030000u;  // triangles
  pipeline_block[10] = 1;
  uint64_t native[17] = {};
  native[1] = reinterpret_cast<uint64_t>(state);
  native[2] = sizeof(state);
  native[3] = reinterpret_cast<uint64_t>(pipeline_block);
  native[4] = sizeof(pipeline_block);
  native[5] = reinterpret_cast<uint64_t>(vs_code);
  native[6] = sizeof(vs_code);
  native[7] = reinterpret_cast<uint64_t>(meta);
  native[8] = sizeof(meta);
  native[9] = reinterpret_cast<uint64_t>(ps_code);
  native[10] = sizeof(ps_code);
  native[11] = reinterpret_cast<uint64_t>(meta);
  native[12] = sizeof(meta);
  call<HRESULT>(device, 57, static_cast<const uint64_t*>(native), __uuidof(ID3D12PipelineState), &draw_pipeline);
  // The second: the stencil test passing where the value equals the reference, kept.
  state[0x2E] = stencil ? 0x1u | (2u << 8) | (2u << 20) : state[0x2E];
  state[0x2F] = 0;
  state[0x30] = state[0x31] = stencil ? 0x100FF00u : 0u;
  call<HRESULT>(device, 57, static_cast<const uint64_t*>(native), __uuidof(ID3D12PipelineState), &second_pipeline);
  const std::vector<uint8_t> read_cs = native_only_container(
      {0xF4080100u, 0xFA000000u, 0xBF8CC07Fu, 0xBE880304u, 0xBE890380u, 0xF40C0304u, 0xFA000000u, 0xBF8CC07Fu,
       0x7E000280u, 0x7E020280u, 0xF0001F08u, 0x00030200u, 0xBF8C3F70u, 0xBE940305u, 0xBE950306u, 0xB0160040u,
       0xBE970380u, 0xE0780000u, 0x80050200u, 0xBF810000u});
  D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
  pso.CS = {read_cs.data(), read_cs.size()};
  call<HRESULT>(device, 62, &pso, static_cast<void*>(nullptr), static_cast<void*>(nullptr),
                __uuidof(ID3D12PipelineState), &read_pipeline);

  D3D12_HEAP_PROPERTIES local{};
  local.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC texture{};
  texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  texture.Width = 64;
  texture.Height = 64;
  texture.DepthOrArraySize = 1;
  texture.MipLevels = 1;
  texture.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  texture.SampleDesc.Count = 1;
  texture.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  call<HRESULT>(device, 127, &local, D3D12_HEAP_FLAG_NONE, &texture, static_cast<UINT>(D3D12_BARRIER_LAYOUT_RENDER_TARGET),
                static_cast<const D3D12_CLEAR_VALUE*>(nullptr), static_cast<void*>(nullptr), 0u,
                static_cast<const void*>(nullptr), __uuidof(ID3D12Resource), &target);
  D3D12_RESOURCE_DESC depth_texture = texture;
  depth_texture.Format = depth_format;
  depth_texture.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
  call<HRESULT>(device, 127, &local, D3D12_HEAP_FLAG_NONE, &depth_texture,
                static_cast<UINT>(D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE), static_cast<const D3D12_CLEAR_VALUE*>(nullptr),
                static_cast<void*>(nullptr), 0u, static_cast<const void*>(nullptr), __uuidof(ID3D12Resource), &depth);

  auto* memory = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x20000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
  if (!list || !fence || !heap || !targets || !depths || !compute_root || !draw_pipeline || !second_pipeline ||
      !read_pipeline || !target || !depth || !memory) {
    check(false, "the objects for a native draw with depth exist");
    return;
  }
  // Six vertices: a near green triangle, then a far red one, both covering
  // the target (with the stencil test: red first, then green).
  float vertices[6][8] = {{-1, -1, 0.5f, 1, 0, 1, 0, 1}, {-1, 3, 0.5f, 1, 0, 1, 0, 1}, {3, -1, 0.5f, 1, 0, 1, 0, 1},
                          {-1, -1, 0.7f, 1, 1, 0, 0, 1}, {-1, 3, 0.7f, 1, 1, 0, 0, 1}, {3, -1, 0.7f, 1, 1, 0, 0, 1}};
  if (stencil) {
    for (int v = 0; v < 6; ++v) {
      vertices[v][4] = v < 3 ? 1.0f : 0.0f;
      vertices[v][5] = v < 3 ? 0.0f : 1.0f;
    }
  }
  std::memcpy(memory, vertices, sizeof(vertices));
  auto* output = reinterpret_cast<volatile float*>(memory + 0x10000);
  const uint64_t output_address = reinterpret_cast<uint64_t>(output);
  D3D12_RESOURCE_DESC buffer{};
  buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  buffer.Width = 0x10000;
  buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
  buffer.SampleDesc.Count = 1;
  buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  buffer.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  call<HRESULT>(device, 133, output_address, &buffer, static_cast<UINT>(0xFFFFFFFF), static_cast<const void*>(nullptr),
                __uuidof(ID3D12Resource), &placed);
  const uint64_t rtv = call<uint64_t>(targets, 9);
  const uint64_t dsv = call<uint64_t>(depths, 9);
  call<void>(device, 20, target, static_cast<const D3D12_RENDER_TARGET_VIEW_DESC*>(nullptr), rtv);
  D3D12_DEPTH_STENCIL_VIEW_DESC dsv_view{};
  dsv_view.Format = depth_format;
  dsv_view.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
  call<void>(device, 21, depth, &dsv_view, dsv);
  D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
  srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv.Texture2D.MipLevels = 1;
  call<void>(device, 18, target, &srv, call<uint64_t>(heap, 9));

  bool recorded = call<HRESULT>(list, 139, allocator, static_cast<void*>(nullptr), UINT64{0}) == S_OK;
  const D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle{static_cast<SIZE_T>(rtv)};
  const D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle{static_cast<SIZE_T>(dsv)};
  const float black[4] = {0, 0, 0, 0};
  call<void>(list, 52, static_cast<SIZE_T>(rtv), black, 0u, static_cast<const D3D12_RECT*>(nullptr));
  call<void>(list, 51, static_cast<SIZE_T>(dsv),
             static_cast<D3D12_CLEAR_FLAGS>(D3D12_CLEAR_FLAG_DEPTH | (stencil ? D3D12_CLEAR_FLAG_STENCIL : 0)), 1.0f,
             static_cast<UINT8>(0), 0u, static_cast<const D3D12_RECT*>(nullptr));
  call<void>(list, 25, draw_pipeline);
  call<void>(list, 20, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  const D3D12_VERTEX_BUFFER_VIEW vertex_buffer{reinterpret_cast<uint64_t>(memory), sizeof(vertices), 32};
  call<void>(list, 48, 0u, 1u, &vertex_buffer);
  call<void>(list, 50, 1u, &rtv_handle, FALSE, &dsv_handle);
  const D3D12_VIEWPORT viewport{0, 0, 64, 64, 0, 1};
  const D3D12_RECT scissor{0, 0, 64, 64};
  call<void>(list, 21, 1u, &viewport);
  call<void>(list, 22, 1u, &scissor);
  call<void>(list, 24, 1u);              // stencil reference
  call<void>(list, 12, 3u, 1u, 0u, 0u);  // near, green (stencil: red, writing 1)
  call<void>(list, 25, second_pipeline);
  call<void>(list, 24, 0u);
  call<void>(list, 12, 3u, 1u, 3u, 0u);  // far, red: hidden (stencil: green, failing the test)
  D3D12_TEXTURE_BARRIER barrier{};
  barrier.SyncBefore = D3D12_BARRIER_SYNC_RENDER_TARGET;
  barrier.SyncAfter = D3D12_BARRIER_SYNC_COMPUTE_SHADING;
  barrier.AccessBefore = D3D12_BARRIER_ACCESS_RENDER_TARGET;
  barrier.AccessAfter = D3D12_BARRIER_ACCESS_SHADER_RESOURCE;
  barrier.LayoutBefore = D3D12_BARRIER_LAYOUT_RENDER_TARGET;
  barrier.LayoutAfter = D3D12_BARRIER_LAYOUT_SHADER_RESOURCE;
  barrier.pResource = static_cast<ID3D12Resource*>(target);
  barrier.Subresources.IndexOrFirstMipLevel = 0xFFFFFFFFu;
  D3D12_BARRIER_GROUP group{};
  group.Type = D3D12_BARRIER_TYPE_TEXTURE;
  group.NumBarriers = 1;
  group.pTextureBarriers = &barrier;
  call<void>(list, 146, 1u, static_cast<const D3D12_BARRIER_GROUP*>(&group));
  const uint32_t constants[2] = {static_cast<uint32_t>(output_address), static_cast<uint32_t>(output_address >> 32)};
  void* heaps[] = {heap};
  call<void>(list, 32, 1u, static_cast<void* const*>(heaps));
  call<void>(list, 33, compute_root);
  call<void>(list, 35, 0u, call<uint64_t>(heap, 10));
  call<void>(list, 39, 1u, 2u, static_cast<const void*>(constants), 0u);
  call<void>(list, 25, read_pipeline);
  call<void>(list, 14, 1u, 1u, 1u);
  recorded = recorded && call<HRESULT>(list, 9) == S_OK;
  void* lists[] = {list};
  call<void>(queue, 35, 1u, static_cast<void* const*>(lists), UINT64{0}, 0u);
  call<HRESULT>(queue, 16, fence, UINT64{1});
  const auto* value = call<const volatile uint64_t*>(fence, 12);
  for (int i = 0; value && *value < 1 && i < 5000; ++i) Sleep(1);
  const float pixel[4] = {output[0], output[1], output[2], output[3]};
  std::printf("    (pixel after the two draws: %.3f %.3f %.3f %.3f; fence %llu; host commit left %.1f GiB)\n", pixel[0],
              pixel[1], pixel[2], pixel[3], static_cast<unsigned long long>(value ? *value : 0), host_commit_left());
  const bool first_kept = stencil ? pixel[0] > 0.99f && pixel[1] < 0.01f : pixel[0] < 0.01f && pixel[1] > 0.99f;
  check(recorded && value && *value == 1 && first_kept && pixel[2] < 0.01f,
        stencil ? "a native pipeline's stencil state keeps a second draw out where the first replaced the value"
                : "a native pipeline's depth state hides a far triangle drawn after a near one");

  if (placed) call<ULONG>(placed, 2);
  call<ULONG>(depth, 2);
  call<ULONG>(target, 2);
  call<ULONG>(read_pipeline, 2);
  call<ULONG>(draw_pipeline, 2);
  call<ULONG>(second_pipeline, 2);
  call<ULONG>(compute_root, 2);
  call<ULONG>(depths, 2);
  call<ULONG>(targets, 2);
  call<ULONG>(heap, 2);
  call<ULONG>(fence, 2);
  call<ULONG>(list, 2);
  call<ULONG>(allocator, 2);
  VirtualFree(memory, 0, MEM_RELEASE);
}

void test_graphics_resources(void* device, void* queue) {
  if (queue) test_native_draw(device, queue);
  if (queue) test_draw_recording_cost(device, queue);
  if (queue) test_memory_read_cost(device, queue);
  if (queue) test_root_arguments_outlive_later_recording(device, queue);
  if (queue) test_memory_released_while_read(device, queue);
  if (queue) test_committed_buffers_and_copies(device, queue);
  if (queue) test_memory_copies(device, queue);
  if (queue) test_native_depth(device, queue, false);
  if (queue) test_native_depth(device, queue, true);
  test_page_mapping_update(device);
  test_released_memory(device);
  test_released_texture_memory(device);
  if (queue) {
    test_compute_reads_a_texture_view(device, queue, TextureMemory::Plain);
    test_compute_reads_a_texture_view(device, queue, TextureMemory::Reserved);
    test_compute_reads_a_texture_view(device, queue, TextureMemory::Physical);
    test_compute_reads_a_texture_view(device, queue, TextureMemory::Unmapped);
  }
  test_allocation_hooks();
  test_placeholders();
  test_descriptor_heap(device);
  test_resources(device);
  test_root_signature_and_pipeline(device);
  test_native_pipeline(device);
  test_device_objects(device);
  test_pipeline_stream(device);
  test_small_constant_buffer_view(device);
  test_display_and_pacing(device);
}
