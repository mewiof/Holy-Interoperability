#include "core/libs/d3d12_xs/texture_dump.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include "core/libs/d3d12_xs/device.h"
#include "core/libs/support.h"

namespace holy::d3d12 {
namespace {

std::mutex g_lock;
std::unordered_set<Resource*>& live() {
  static auto* set = new std::unordered_set<Resource*>();
  return *set;
}

float half(uint16_t h) {
  const uint32_t sign = (h >> 15) & 1, exponent = (h >> 10) & 0x1F, mantissa = h & 0x3FF;
  float v;
  if (exponent == 0) {
    v = std::ldexp(static_cast<float>(mantissa), -24);
  } else if (exponent == 31) {
    v = mantissa ? NAN : INFINITY;
  } else {
    v = std::ldexp(static_cast<float>(mantissa | 0x400), static_cast<int>(exponent) - 25);
  }
  return sign ? -v : v;
}

float small_float(uint32_t bits, int mantissa_bits) {
  // Unsigned floats with a 5-bit exponent (R11G11B10).
  const uint32_t exponent = bits >> mantissa_bits, mantissa = bits & ((1u << mantissa_bits) - 1);
  if (exponent == 0) return std::ldexp(static_cast<float>(mantissa), -14 - mantissa_bits);
  if (exponent == 31) return INFINITY;
  return std::ldexp(static_cast<float>(mantissa | (1u << mantissa_bits)), static_cast<int>(exponent) - 15 - mantissa_bits);
}

// Up to four channels of a texel as floats; false for formats not decoded.
bool decode(DXGI_FORMAT format, const uint8_t* p, float out[4], int* channels) {
  uint32_t u32;
  uint16_t u16[4];
  float f32[4];
  switch (static_cast<uint32_t>(format)) {
    case 1: case 2: case 3: case 4:  // R32G32B32A32
      std::memcpy(f32, p, 16);
      if (format == 3 || format == 4) {
        for (int i = 0; i < 4; ++i) std::memcpy(&u32, p + 4 * i, 4), out[i] = static_cast<float>(u32);
      } else {
        for (int i = 0; i < 4; ++i) out[i] = f32[i];
      }
      *channels = 4;
      return true;
    case 9: case 10:  // R16G16B16A16 float
      std::memcpy(u16, p, 8);
      for (int i = 0; i < 4; ++i) out[i] = half(u16[i]);
      *channels = 4;
      return true;
    case 11: case 12: case 13: case 14:  // R16G16B16A16 unorm/uint/snorm/sint
      std::memcpy(u16, p, 8);
      for (int i = 0; i < 4; ++i) out[i] = format == 11 ? u16[i] / 65535.0f : static_cast<float>(u16[i]);
      *channels = 4;
      return true;
    case 15: case 16:  // R32G32 float
      std::memcpy(f32, p, 8);
      out[0] = f32[0], out[1] = f32[1];
      *channels = 2;
      return true;
    case 19: case 20: case 21:  // D32 S8X24
      std::memcpy(f32, p, 4);
      out[0] = f32[0];
      *channels = 1;
      return true;
    case 23: case 24: case 25:  // R10G10B10A2
      std::memcpy(&u32, p, 4);
      out[0] = (u32 & 0x3FF) / 1023.0f, out[1] = ((u32 >> 10) & 0x3FF) / 1023.0f,
      out[2] = ((u32 >> 20) & 0x3FF) / 1023.0f, out[3] = (u32 >> 30) / 3.0f;
      *channels = 4;
      return true;
    case 26:  // R11G11B10 float
      std::memcpy(&u32, p, 4);
      out[0] = small_float(u32 & 0x7FF, 6), out[1] = small_float((u32 >> 11) & 0x7FF, 6),
      out[2] = small_float(u32 >> 22, 5);
      *channels = 3;
      return true;
    case 27: case 28: case 29: case 30: case 31: case 32:  // R8G8B8A8
      for (int i = 0; i < 4; ++i) out[i] = format == 30 || format == 32 ? p[i] : p[i] / 255.0f;
      *channels = 4;
      return true;
    case 33: case 34:  // R16G16 float
      std::memcpy(u16, p, 4);
      out[0] = half(u16[0]), out[1] = half(u16[1]);
      *channels = 2;
      return true;
    case 35: case 36:  // R16G16 unorm / uint
      std::memcpy(u16, p, 4);
      out[0] = format == 35 ? u16[0] / 65535.0f : u16[0], out[1] = format == 35 ? u16[1] / 65535.0f : u16[1];
      *channels = 2;
      return true;
    case 39: case 40: case 41:  // R32 float / D32
      std::memcpy(f32, p, 4);
      out[0] = f32[0];
      *channels = 1;
      return true;
    case 42: case 43:  // R32 uint / sint
      std::memcpy(&u32, p, 4);
      out[0] = static_cast<float>(u32);
      *channels = 1;
      return true;
    case 44: case 45: case 46:  // R24G8 / D24S8
      std::memcpy(&u32, p, 4);
      out[0] = (u32 & 0xFFFFFF) / 16777215.0f, out[1] = static_cast<float>(u32 >> 24);
      *channels = 2;
      return true;
    case 48: case 49: case 50:  // R8G8
      out[0] = format == 50 ? p[0] : p[0] / 255.0f, out[1] = format == 50 ? p[1] : p[1] / 255.0f;
      *channels = 2;
      return true;
    case 53: case 54:  // R16 float
      std::memcpy(u16, p, 2);
      out[0] = half(u16[0]);
      *channels = 1;
      return true;
    case 55: case 56: case 57:  // D16 / R16 unorm / uint
      std::memcpy(u16, p, 2);
      out[0] = format == 57 ? u16[0] : u16[0] / 65535.0f;
      *channels = 1;
      return true;
    case 60: case 61: case 62: case 65:  // R8 / A8 (typeless: stencil planes, as integers)
      out[0] = format == 62 || format == 60 ? p[0] : p[0] / 255.0f;
      *channels = 1;
      return true;
    case 87: case 88: case 90: case 91:  // B8G8R8A8
      out[0] = p[2] / 255.0f, out[1] = p[1] / 255.0f, out[2] = p[0] / 255.0f, out[3] = p[3] / 255.0f;
      *channels = 4;
      return true;
    default:
      return false;
  }
}

void write_bmp(const std::wstring& path, UINT width, UINT height, const std::vector<uint8_t>& bgra) {
  BITMAPFILEHEADER file{};
  BITMAPINFOHEADER info{};
  info.biSize = sizeof(info);
  info.biWidth = static_cast<LONG>(width);
  info.biHeight = -static_cast<LONG>(height);
  info.biPlanes = 1;
  info.biBitCount = 32;
  info.biCompression = BI_RGB;
  file.bfType = 0x4D42;
  file.bfOffBits = sizeof(file) + sizeof(info);
  file.bfSize = static_cast<DWORD>(file.bfOffBits + bgra.size());
  const HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return;
  DWORD written = 0;
  WriteFile(handle, &file, sizeof(file), &written, nullptr);
  WriteFile(handle, &info, sizeof(info), &written, nullptr);
  WriteFile(handle, bgra.data(), static_cast<DWORD>(bgra.size()), &written, nullptr);
  CloseHandle(handle);
}

struct Pending {
  Resource* resource;
  ID3D12Resource* readback;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
  D3D12_BARRIER_LAYOUT layout;
  UINT plane;
};

// Planes of a host texture format: depth and stencil formats have two.
UINT plane_count(DXGI_FORMAT format) {
  switch (format) {
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
      return 2;
    default:
      return 1;
  }
}

}  // namespace

void register_live_resource(Resource* resource) {
  std::lock_guard<std::mutex> guard(g_lock);
  live().insert(resource);
}

void unregister_live_resource(Resource* resource) {
  std::lock_guard<std::mutex> guard(g_lock);
  live().erase(resource);
}

void note_texture_layout(Resource* resource, D3D12_BARRIER_LAYOUT layout) {
  if (resource) resource->layout.store(static_cast<int>(layout), std::memory_order_relaxed);
}

uint64_t texture_dump_frame() {
  static const uint64_t frame = [] {
    wchar_t text[32] = {};
    if (!GetEnvironmentVariableW(L"HOLY_DUMP_TARGETS", text, 32)) return 0ull;
    return static_cast<uint64_t>(_wtoi64(text));
  }();
  return frame;
}

// Logs each depth-stencil texture in title memory with the other live
// resources over the same memory (sizes as the host lays them out, so
// approximate): a texture the title clears or writes through another
// resource on the console does not see those writes on the host.
void report_depth_aliases(Device* device) {
  struct Span {
    Resource* resource;
    uint64_t begin, end;
  };
  std::vector<Span> spans;
  {
    std::lock_guard<std::mutex> guard(g_lock);
    for (Resource* r : live()) {
      if (!r->title_va) continue;
      if (!r->host) {
        if (r->desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) {
          LIB_LOG(lib::kInfo, "d3d12", "  depth texture at 0x%llx: %llux%u format %u with no host texture",
                  static_cast<unsigned long long>(r->title_va), static_cast<unsigned long long>(r->desc.Width),
                  r->desc.Height, static_cast<unsigned>(r->desc.Format));
        }
        continue;
      }
      const D3D12_RESOURCE_DESC desc = r->host->GetDesc();
      const uint64_t bytes = desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER
                                 ? r->desc.Width
                                 : device->host->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
      spans.push_back({r, r->title_va, r->title_va + bytes});
    }
  }
  for (const Span& d : spans) {
    if (!(d.resource->desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) continue;
    const D3D12_RESOURCE_DESC& desc = d.resource->desc;
    LIB_LOG(lib::kInfo, "d3d12", "  depth texture at 0x%llx-0x%llx: %llux%u format %u, %u samples, flags 0x%x",
            static_cast<unsigned long long>(d.begin), static_cast<unsigned long long>(d.end),
            static_cast<unsigned long long>(desc.Width), desc.Height, static_cast<unsigned>(desc.Format),
            desc.SampleDesc.Count, static_cast<unsigned>(desc.Flags));
    int shown = 0;
    for (const Span& o : spans) {
      if (o.resource == d.resource || o.begin >= d.end || o.end <= d.begin) continue;
      if (++shown > 16) continue;
      const D3D12_RESOURCE_DESC& other = o.resource->desc;
      LIB_LOG(lib::kInfo, "d3d12", "    overlapped by 0x%llx-0x%llx: dim %d %llux%u format %u, %u samples, flags 0x%x",
              static_cast<unsigned long long>(o.begin), static_cast<unsigned long long>(o.end), other.Dimension,
              static_cast<unsigned long long>(other.Width), other.Height, static_cast<unsigned>(other.Format),
              other.SampleDesc.Count, static_cast<unsigned>(other.Flags));
    }
    if (shown > 16) LIB_LOG(lib::kInfo, "d3d12", "    and %d more", shown - 16);
  }
}

void dump_textures(Device* device, ID3D12CommandQueue* queue, uint64_t present) {
  report_depth_aliases(device);
  std::vector<Resource*> resources;
  {
    std::lock_guard<std::mutex> guard(g_lock);
    for (Resource* r : live()) {
      if (!r->host || r->desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || r->desc.SampleDesc.Count > 1) continue;
      r->refs.fetch_add(1);
      resources.push_back(r);
    }
  }
  ID3D12Device* host = device->host;
  ID3D12CommandAllocator* allocator = nullptr;
  ID3D12GraphicsCommandList7* list = nullptr;
  ID3D12Fence* fence = nullptr;
  if (FAILED(host->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
      FAILED(host->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&list))) ||
      FAILED(host->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
    LIB_LOG(lib::kError, "d3d12", "texture dump: cannot create the copy objects");
    if (allocator) allocator->Release();
    if (list) list->Release();
    for (Resource* r : resources) reinterpret_cast<ULONG(STDMETHODCALLTYPE*)(Resource*)>(r->vtable[2])(r);
    return;
  }
  std::vector<Pending> pending;
  for (Resource* r : resources) {
    const D3D12_RESOURCE_DESC desc = r->host->GetDesc();
    for (UINT plane = 0; plane < plane_count(desc.Format); ++plane) {
      // Mip 0 of slice 0 of the plane.
      const UINT subresource = plane * desc.MipLevels * desc.DepthOrArraySize;
      D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
      UINT64 total = 0;
      host->GetCopyableFootprints(&desc, subresource, 1, 0, &footprint, nullptr, nullptr, &total);
      if (!total || total > (256ull << 20)) {
        LIB_LOG(lib::kInfo, "d3d12", "  texture at 0x%llx (%llux%u format %u, %u mips, plane %u): not copied (%llu bytes)",
                static_cast<unsigned long long>(r->title_va), static_cast<unsigned long long>(desc.Width),
                desc.Height, static_cast<unsigned>(desc.Format), desc.MipLevels, plane,
                static_cast<unsigned long long>(total));
        continue;
      }
      D3D12_HEAP_PROPERTIES heap{};
      heap.Type = D3D12_HEAP_TYPE_READBACK;
      D3D12_RESOURCE_DESC buffer{};
      buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
      buffer.Width = total;
      buffer.Height = 1;
      buffer.DepthOrArraySize = 1;
      buffer.MipLevels = 1;
      buffer.SampleDesc.Count = 1;
      buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
      ID3D12Resource* readback = nullptr;
      if (FAILED(host->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST,
                                               nullptr, IID_PPV_ARGS(&readback)))) {
        LIB_LOG(lib::kInfo, "d3d12", "  texture at 0x%llx: no readback buffer of %llu bytes",
                static_cast<unsigned long long>(r->title_va), static_cast<unsigned long long>(total));
        continue;
      }
      const int known = r->layout.load(std::memory_order_relaxed);
      const auto layout = known >= 0 ? static_cast<D3D12_BARRIER_LAYOUT>(known) : D3D12_BARRIER_LAYOUT_COMMON;
      D3D12_TEXTURE_BARRIER barrier{};
      barrier.SyncBefore = D3D12_BARRIER_SYNC_ALL;
      barrier.SyncAfter = D3D12_BARRIER_SYNC_COPY;
      barrier.AccessBefore = D3D12_BARRIER_ACCESS_COMMON;
      barrier.AccessAfter = D3D12_BARRIER_ACCESS_COPY_SOURCE;
      barrier.LayoutBefore = layout;
      barrier.LayoutAfter = D3D12_BARRIER_LAYOUT_COPY_SOURCE;
      barrier.pResource = r->host;
      barrier.Subresources = {0, 1, 0, 1, plane, 1};
      D3D12_BARRIER_GROUP group{};
      group.Type = D3D12_BARRIER_TYPE_TEXTURE;
      group.NumBarriers = 1;
      group.pTextureBarriers = &barrier;
      list->Barrier(1, &group);
      D3D12_TEXTURE_COPY_LOCATION source{}, target{};
      source.pResource = r->host;
      source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      source.SubresourceIndex = subresource;
      target.pResource = readback;
      target.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      target.PlacedFootprint = footprint;
      list->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
      std::swap(barrier.SyncBefore, barrier.SyncAfter);
      std::swap(barrier.AccessBefore, barrier.AccessAfter);
      std::swap(barrier.LayoutBefore, barrier.LayoutAfter);
      list->Barrier(1, &group);
      pending.push_back({r, readback, footprint, layout, plane});
    }
  }
  list->Close();
  ID3D12CommandList* lists[] = {list};
  queue->ExecuteCommandLists(1, lists);
  queue->Signal(fence, 1);
  const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  fence->SetEventOnCompletion(1, event);
  WaitForSingleObject(event, 20000);
  CloseHandle(event);

  const wchar_t* data_dir = HolyDataDir();
  const std::wstring dir = data_dir && *data_dir
                               ? std::wstring(data_dir) + L"\\frames\\textures-" + std::to_wstring(present)
                               : std::wstring();
  if (!dir.empty()) {
    const std::wstring frames = std::wstring(data_dir) + L"\\frames";
    CreateDirectoryW(frames.c_str(), nullptr);
    // Earlier dumps go first (a dump is gigabytes; measured: a day of runs
    // left 29 GB of them).
    WIN32_FIND_DATAW found{};
    const HANDLE search = FindFirstFileW((frames + L"\\textures-*").c_str(), &found);
    if (search != INVALID_HANDLE_VALUE) {
      do {
        if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        const std::wstring old = frames + L"\\" + found.cFileName;
        WIN32_FIND_DATAW file{};
        const HANDLE files = FindFirstFileW((old + L"\\*.bmp").c_str(), &file);
        if (files != INVALID_HANDLE_VALUE) {
          do {
            DeleteFileW((old + L"\\" + file.cFileName).c_str());
          } while (FindNextFileW(files, &file));
          FindClose(files);
        }
        RemoveDirectoryW(old.c_str());
      } while (FindNextFileW(search, &found));
      FindClose(search);
    }
    CreateDirectoryW(dir.c_str(), nullptr);
  }
  LIB_LOG(lib::kInfo, "d3d12", "texture dump at present %llu: %zu textures", static_cast<unsigned long long>(present),
          pending.size());
  size_t index = 0;
  for (const Pending& item : pending) {
    const D3D12_SUBRESOURCE_FOOTPRINT& f = item.footprint.Footprint;
    void* mapped = nullptr;
    const D3D12_RANGE range{0, static_cast<SIZE_T>(f.RowPitch) * f.Height};
    if (FAILED(item.readback->Map(0, &range, &mapped))) continue;
    const auto* bytes = static_cast<const uint8_t*>(mapped);
    const UINT texel = f.Width ? (f.Width * 0 + 1) : 1;
    (void)texel;
    float lo[4] = {INFINITY, INFINITY, INFINITY, INFINITY}, hi[4] = {-INFINITY, -INFINITY, -INFINITY, -INFINITY};
    int channels = 0;
    // Bytes per texel from the footprint's format.
    D3D12_RESOURCE_DESC one{};
    one.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    one.Width = 1;
    one.Height = 1;
    one.DepthOrArraySize = 1;
    one.MipLevels = 1;
    one.Format = f.Format;
    one.SampleDesc.Count = 1;
    UINT64 row_bytes = 0;
    host->GetCopyableFootprints(&one, 0, 1, 0, nullptr, nullptr, &row_bytes, nullptr);
    const UINT stride = static_cast<UINT>(row_bytes ? row_bytes : 4);
    const UINT out_width = std::min<UINT>(f.Width, 1920), out_height = static_cast<UINT>(
                                                              static_cast<uint64_t>(f.Height) * out_width / std::max<UINT>(f.Width, 1));
    std::vector<uint8_t> bgra(static_cast<size_t>(out_width) * std::max<UINT>(out_height, 1) * 4, 0);
    bool decodable = true;
    for (UINT y = 0; y < f.Height && decodable; ++y) {
      const uint8_t* row = bytes + static_cast<size_t>(y) * f.RowPitch;
      for (UINT x = 0; x < f.Width; ++x) {
        float v[4] = {0, 0, 0, 1};
        if (!decode(f.Format, row + static_cast<size_t>(x) * stride, v, &channels)) {
          decodable = false;
          break;
        }
        for (int c = 0; c < channels; ++c) {
          if (v[c] < lo[c]) lo[c] = v[c];
          if (v[c] > hi[c]) hi[c] = v[c];
        }
      }
    }
    if (decodable && out_height) {
      float scale[4];
      for (int c = 0; c < 4; ++c) scale[c] = (c < channels && hi[c] > 1.0f && std::isfinite(hi[c])) ? hi[c] : 1.0f;
      for (UINT y = 0; y < out_height; ++y) {
        const UINT sy = static_cast<UINT>(static_cast<uint64_t>(y) * f.Height / out_height);
        const uint8_t* row = bytes + static_cast<size_t>(sy) * f.RowPitch;
        for (UINT x = 0; x < out_width; ++x) {
          const UINT sx = static_cast<UINT>(static_cast<uint64_t>(x) * f.Width / out_width);
          float v[4] = {0, 0, 0, 1};
          decode(f.Format, row + static_cast<size_t>(sx) * stride, v, &channels);
          if (channels == 1) v[1] = v[2] = v[0];
          uint8_t* out = bgra.data() + (static_cast<size_t>(y) * out_width + x) * 4;
          for (int c = 0; c < 3; ++c) {
            const float n = v[c] / scale[c];
            out[2 - c] = static_cast<uint8_t>(!(n > 0.0f) ? 0 : n >= 1.0f ? 255 : n * 255.0f + 0.5f);
          }
          out[3] = 255;
        }
      }
    }
    item.readback->Unmap(0, nullptr);
    char stats[256] = "not decoded";
    if (decodable) {
      std::snprintf(stats, sizeof(stats), "min %g %g %g %g, max %g %g %g %g", lo[0], channels > 1 ? lo[1] : 0.0,
                    channels > 2 ? lo[2] : 0.0, channels > 3 ? lo[3] : 0.0, hi[0], channels > 1 ? hi[1] : 0.0,
                    channels > 2 ? hi[2] : 0.0, channels > 3 ? hi[3] : 0.0);
    }
    // Whether the title memory under it holds anything (sampled; reads that
    // fail count as nothing): tells a texture the title never wrote from a
    // host copy that missed the title's writes.
    uint64_t nonzero = 0, sampled = 0;
    MEMORY_BASIC_INFORMATION protection{};
    if (item.resource->title_va) {
      VirtualQuery(reinterpret_cast<const void*>(item.resource->title_va), &protection, sizeof(protection));
    }
    if (item.resource->title_va) {
      const uint64_t span = static_cast<uint64_t>(f.RowPitch) * f.Height;
      for (uint64_t at = 0; at < span; at += 4093, ++sampled) {
        uint8_t byte = 0;
        SIZE_T got = 0;
        if (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(item.resource->title_va + at), &byte, 1,
                              &got) &&
            byte) {
          ++nonzero;
        }
      }
    }
    LIB_LOG(lib::kInfo, "d3d12",
            "  texture %zu%s at 0x%llx: %ux%u format %u (host %u, title %u) layout %d flags 0x%x: %s; memory: %llu of "
            "%llu sampled bytes not zero, protection 0x%lx type 0x%lx",
            index, item.plane ? " (stencil)" : "", static_cast<unsigned long long>(item.resource->title_va), f.Width,
            f.Height, static_cast<unsigned>(f.Format),
            item.resource->host ? static_cast<unsigned>(item.resource->host->GetDesc().Format) : 0u,
            static_cast<unsigned>(item.resource->desc.Format),
            static_cast<int>(item.layout), static_cast<unsigned>(item.resource->desc.Flags), stats,
            static_cast<unsigned long long>(nonzero), static_cast<unsigned long long>(sampled), protection.Protect,
            protection.Type);
    if (decodable && !dir.empty() && out_height) {
      wchar_t name[96];
      std::swprintf(name, 96, L"\\t%03zu-%ux%u-f%u%ls.bmp", index, f.Width, f.Height, static_cast<unsigned>(f.Format),
                    item.plane ? L"-stencil" : L"");
      write_bmp(dir + name, out_width, out_height, bgra);
    }
    ++index;
  }
  for (const Pending& item : pending) item.readback->Release();
  for (Resource* r : resources) reinterpret_cast<ULONG(STDMETHODCALLTYPE*)(Resource*)>(r->vtable[2])(r);
  list->Release();
  allocator->Release();
  fence->Release();
}

}  // namespace holy::d3d12
