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

// A native compute program reads a texture the title viewed in its memory,
// all four channels, through the descriptor in the bound heap, and writes them
// to an address passed as root constants, in memory a buffer is placed in
// (the memory map): the whole path of translated work, from recording to the
// GPU.
// The command list interface titles ask for.
constexpr GUID kListIid = {0xc3827890, 0x4024, 0x474c, {0x98, 0xcf, 0x64, 0x5b, 0x51, 0xdc, 0x48, 0x74}};

void test_compute_reads_a_texture_view(void* device, void* queue) {
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
  // buffer_store_dwordx4 v[2:5], off, s[20:23], 0; s_endpgm.
  const std::vector<uint8_t> container = native_only_container(
      {0xF4080100u, 0xFA000000u, 0xBF8CC07Fu, 0xBE880304u, 0xBE890380u, 0xF40C0304u, 0xFA000000u, 0xBF8CC07Fu,
       0x7E000280u, 0x7E020280u, 0xF0001F08u, 0x00030200u, 0xBF8C3F70u, 0xBE940305u, 0xBE950306u, 0xB0160040u,
       0xBE970380u, 0xE0780000u, 0x80050200u, 0xBF810000u});
  D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
  pso.CS = {container.data(), container.size()};
  call<HRESULT>(device, 62, &pso, static_cast<void*>(nullptr), static_cast<void*>(nullptr),
                __uuidof(ID3D12PipelineState), &pipeline);
  auto* memory = static_cast<volatile uint8_t*>(VirtualAlloc(nullptr, 0x20000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
  if (!list || !fence || !heap || !signature || !pipeline || !memory) {
    check(false, "the objects for a compute program reading a texture view exist");
    return;
  }
  for (uint32_t i = 0; i < 0x4000; ++i) memory[i] = 0x40;
  memory[0] = 0x10;
  memory[1] = 0x20;
  memory[2] = 0x30;
  memory[3] = 0xFF;
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
  std::printf("    (texel read on the GPU: %.3f %.3f %.3f %.3f)\n", texel[0], texel[1], texel[2], texel[3]);
  check(recorded && value && *value == 1 && close_to(texel[0], 16 / 255.0f) && close_to(texel[1], 32 / 255.0f) &&
            close_to(texel[2], 48 / 255.0f) && close_to(texel[3], 1.0f),
        "a native compute program reads all four channels of a texture viewed in title memory");

  if (placed) call<ULONG>(placed, 2);
  call<ULONG>(pipeline, 2);
  call<ULONG>(signature, 2);
  call<ULONG>(heap, 2);
  call<ULONG>(fence, 2);
  call<ULONG>(list, 2);
  call<ULONG>(allocator, 2);
  VirtualFree(const_cast<uint8_t*>(memory), 0, MEM_RELEASE);
}

void test_graphics_resources(void* device, void* queue) {
  test_page_mapping_update(device);
  test_released_memory(device);
  if (queue) test_compute_reads_a_texture_view(device, queue);
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
