// Presentation.
//
// Observed: titles present through queue slot 32, PresentX(plane_count,
// planes, parameters), with planes in the public
// D3D12XBOX_PRESENT_PLANE_PARAMETERS layout (present.h): a frame token, one
// display buffer (a committed resource made with ALLOW_DISPLAY), a source
// rectangle and a colour space. Usage matches the public Xbox GDK sample
// code; the buffer is in the present (common) state when it is presented.
//
// Host model: the title's window gets a flip-model swap chain on the host
// queue behind the title's queue, sized and formatted like the display
// buffer's source rectangle. Each present copies the buffer into the swap
// chain's current back buffer and presents it; a host fence then carries the
// frame's token, so waits for a presented frame (device slot 80) wait for the
// GPU. Partial: only the first plane is shown; colour spaces and the
// destination placement are not applied; display buffer formats other than
// the swap-chain formats (8-bit and 10-bit UNORM, 16-bit float) are refused.
//
// Captured frames: presents 1, 30, 300 and every 1800th after that are also
// read back and written to <data directory>/frames/present-<n>.bmp, so a run
// leaves evidence of what the title drew.
#include "core/libs/d3d12_xs/call_check.h"
#include "core/libs/d3d12_xs/present.h"

#include <dxgi1_4.h>
#include <psapi.h>
#include <intrin.h>

#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <cwchar>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "core/libs/d3d12_xs/device.h"
#include "core/libs/d3d12_xs/frame_pacing.h"
#include "core/libs/d3d12_xs/texture_dump.h"
#include "core/libs/d3d12_xs/texture_upload.h"
#include "core/libs/support.h"

namespace holy::d3d12 {

namespace {

// Frame statistics for the runtime's report: presents (their rate is the
// frame rate), the longest gap between presents in the last ten seconds,
// gaps longer than two display periods at 30 frames a second, and the host
// GPU's memory use.
void count_present(ID3D12Device* device) {
  static const lib::Counter presents("d3d12.presents");
  static const lib::Counter slowest("d3d12.frame_gap_max_ms");
  static const lib::Counter hitches("d3d12.frame_gaps_over_50ms");
  static const lib::Counter local("d3d12.vram_local_mb");
  static const lib::Counter non_local("d3d12.vram_non_local_mb");
  static LARGE_INTEGER frequency = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return f;
  }();
  static LARGE_INTEGER last{}, window{};
  static int64_t window_max = 0;
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  presents.add(1);
  if (last.QuadPart) {
    const int64_t gap_ms = (now.QuadPart - last.QuadPart) * 1000 / frequency.QuadPart;
    if (gap_ms > 50) hitches.add(1);
    if (now.QuadPart - window.QuadPart > frequency.QuadPart * 10) {
      window = now;
      window_max = 0;
    }
    if (gap_ms > window_max) {
      window_max = gap_ms;
      slowest.set(gap_ms);
    }
  } else {
    window = now;
  }
  last = now;
  static IDXGIAdapter3* adapter = [device] {
    IDXGIAdapter3* found = nullptr;
    IDXGIFactory4* factory = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
      factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&found));
      factory->Release();
    }
    return found;
  }();
  static uint64_t queried = 0;
  if (adapter && presents.value() - static_cast<int64_t>(queried) >= 30) {
    queried = static_cast<uint64_t>(presents.value());
    DXGI_QUERY_VIDEO_MEMORY_INFO info{};
    if (SUCCEEDED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
      local.set(static_cast<int64_t>(info.CurrentUsage >> 20));
    }
    if (SUCCEEDED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &info))) {
      non_local.set(static_cast<int64_t>(info.CurrentUsage >> 20));
    }
  }
}

}  // namespace
namespace {

constexpr UINT kBuffers = 3;

struct Presenter {
  ID3D12Device* device = nullptr;
  ID3D12CommandQueue* queue = nullptr;
  HWND window = nullptr;
  IDXGISwapChain3* swap_chain = nullptr;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  UINT width = 0, height = 0;
  ID3D12CommandAllocator* allocators[kBuffers] = {};
  uint64_t allocator_values[kBuffers] = {};
  ID3D12GraphicsCommandList* list = nullptr;
  ID3D12Fence* fence = nullptr;  // value: the token of the last frame carried out
  uint64_t fence_value = 0;
  HANDLE event = nullptr;
  uint64_t presents = 0;
  UINT next_allocator = 0;
};

std::mutex g_lock;
Presenter* g_presenter = nullptr;

// Frames reach "presented" (frame event 2, wait_for_presented) at the frame
// interval boundary after their GPU work is done, as a console present flips
// at the interval: titles wait for an earlier frame's present to pace
// themselves. A thread carries the presents out in order.
struct Flip {
  uint64_t token;
  uint64_t gpu_value;  // the presenter fence value of the present's copy
};
std::mutex g_flip_lock;
std::condition_variable g_flip_ready;
std::deque<Flip> g_flips;
std::atomic<uint64_t> g_tokens{0};
std::atomic<uint64_t> g_last_presented{0};
ID3D12Fence* g_presented_fence = nullptr;  // value: highest token presented

struct FindWindow {
  DWORD process;
  HWND best;
  LONG area;
};

BOOL CALLBACK consider_window(HWND window, LPARAM context) {
  auto* find = reinterpret_cast<FindWindow*>(context);
  DWORD process = 0;
  GetWindowThreadProcessId(window, &process);
  if (process != find->process || !IsWindowVisible(window) || GetWindow(window, GW_OWNER)) return TRUE;
  RECT rect;
  GetClientRect(window, &rect);
  const LONG area = (rect.right - rect.left) * (rect.bottom - rect.top);
  if (!find->best || area > find->area) {
    find->best = window;
    find->area = area;
  }
  return TRUE;
}

// The title's window: the largest visible top-level window of the process.
HWND title_window() {
  FindWindow find{GetCurrentProcessId(), nullptr, 0};
  EnumWindows(&consider_window, reinterpret_cast<LPARAM>(&find));
  return find.best;
}

DXGI_FORMAT swap_chain_format(DXGI_FORMAT format) {
  switch (format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
      return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
      return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
      return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
      return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default:
      return DXGI_FORMAT_UNKNOWN;
  }
}

bool make_presenter(Device* device, ID3D12CommandQueue* queue, Presenter* out) {
  out->device = device->host;
  out->queue = queue;
  for (UINT i = 0; i < kBuffers; ++i) {
    if (FAILED(device->host->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                    IID_PPV_ARGS(&out->allocators[i])))) {
      return false;
    }
  }
  return SUCCEEDED(device->host->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, out->allocators[0], nullptr,
                                                   IID_PPV_ARGS(&out->list))) &&
         SUCCEEDED(out->list->SetName(L"holy present copy")) && SUCCEEDED(out->list->Close()) &&
         SUCCEEDED(device->host->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&out->fence))) &&
         SUCCEEDED(device->host->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_presented_fence))) &&
         (out->event = CreateEventW(nullptr, FALSE, FALSE, nullptr)) != nullptr;
}

void wait_fence(ID3D12Fence* fence, uint64_t value, HANDLE event) {
  if (fence->GetCompletedValue() >= value) return;
  fence->SetEventOnCompletion(value, event);
  WaitForSingleObject(event, INFINITE);
}

bool ensure_swap_chain(Presenter* p, DXGI_FORMAT format, UINT width, UINT height) {
  HWND window = title_window();
  if (!window) return false;
  if (p->swap_chain && p->window == window && p->format == format && p->width == width && p->height == height) {
    return true;
  }
  if (p->swap_chain && p->window == window && p->format == format) {
    wait_fence(p->fence, p->fence_value, p->event);
    if (SUCCEEDED(p->swap_chain->ResizeBuffers(0, width, height, format, 0))) {
      p->width = width;
      p->height = height;
      return true;
    }
  }
  if (p->swap_chain) {
    wait_fence(p->fence, p->fence_value, p->event);
    p->swap_chain->Release();
    p->swap_chain = nullptr;
  }
  IDXGIFactory2* factory = nullptr;
  if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return false;
  DXGI_SWAP_CHAIN_DESC1 desc{};
  desc.Width = width;
  desc.Height = height;
  desc.Format = format;
  desc.SampleDesc.Count = 1;
  desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  desc.BufferCount = kBuffers;
  desc.Scaling = DXGI_SCALING_STRETCH;
  desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
  IDXGISwapChain1* chain = nullptr;
  const HRESULT hr = factory->CreateSwapChainForHwnd(p->queue, window, &desc, nullptr, nullptr, &chain);
  if (SUCCEEDED(hr)) factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
  factory->Release();
  LIB_LOG(SUCCEEDED(hr) ? lib::kInfo : lib::kError, "d3d12", "swap chain %ux%u format %u on the title's window -> 0x%08lX",
          width, height, format, static_cast<unsigned long>(hr));
  if (FAILED(hr)) return false;
  chain->QueryInterface(IID_PPV_ARGS(&p->swap_chain));
  chain->Release();
  p->window = window;
  p->format = format;
  p->width = width;
  p->height = height;
  return p->swap_chain != nullptr;
}

// Presents 1, 30 and 300 are captured; later ones every HOLY_CAPTURE_EVERY
// presents when that is set (a capture of a 4K frame is 33 MB: periodic
// captures by default would fill the disk over long sessions), and the
// presents HOLY_CAPTURE_FROM=<n>[+<count>] names (n and the count-1 after
// it, 8 by default: consecutive frames, for what changes frame to frame).
bool capture_due(uint64_t present) {
  static const uint64_t every = [] {
    wchar_t text[32] = {};
    return GetEnvironmentVariableW(L"HOLY_CAPTURE_EVERY", text, 32) ? static_cast<uint64_t>(_wtoi64(text)) : 0ull;
  }();
  static const std::pair<uint64_t, uint64_t> window = [] {
    wchar_t text[32] = {};
    if (!GetEnvironmentVariableW(L"HOLY_CAPTURE_FROM", text, 32)) return std::pair<uint64_t, uint64_t>{0, 0};
    const uint64_t from = static_cast<uint64_t>(_wtoi64(text));
    const wchar_t* plus = wcschr(text, L'+');
    const int64_t count = plus ? _wtoi64(plus + 1) : 8;
    return std::pair<uint64_t, uint64_t>{from, count > 0 ? static_cast<uint64_t>(count) : 8};
  }();
  const bool in_window = window.first && present >= window.first && present < window.first + window.second;
  return present == 1 || present == 30 || present == 300 || (every && present % every == 0) || in_window;
}

uint8_t to_byte(float value) {
  if (!(value > 0.0f)) return 0;
  if (value >= 1.0f) return 255;
  return static_cast<uint8_t>(value * 255.0f + 0.5f);
}

float half_to_float(uint16_t half) {
  const uint32_t sign = (half >> 15) & 1, exponent = (half >> 10) & 0x1F, mantissa = half & 0x3FF;
  float value;
  if (exponent == 0) {
    value = std::ldexp(static_cast<float>(mantissa), -24);
  } else if (exponent == 31) {
    value = mantissa ? 0.0f : 65504.0f;
  } else {
    value = std::ldexp(static_cast<float>(mantissa | 0x400), static_cast<int>(exponent) - 25);
  }
  return sign ? -value : value;
}

// Removes the files of `dir` matching `pattern` (captures of earlier runs).
// Measured: a frames directory grew to 35 GB over a day of runs (a 4K
// capture is 33 MB), each run's captures only ever added. A run's first
// capture clears the earlier runs' ones (texture dumps do the same with
// earlier dumps, texture_dump.cpp).
void remove_earlier(const std::wstring& dir, const wchar_t* pattern) {
  WIN32_FIND_DATAW found{};
  const HANDLE search = FindFirstFileW((dir + L"\\" + pattern).c_str(), &found);
  if (search == INVALID_HANDLE_VALUE) return;
  do {
    if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) DeleteFileW((dir + L"\\" + found.cFileName).c_str());
  } while (FindNextFileW(search, &found));
  FindClose(search);
}

// Writes rows of `format` texels as a 32-bit top-down BMP.
void write_capture(uint64_t present, DXGI_FORMAT format, UINT width, UINT height, const uint8_t* rows, UINT pitch) {
  const wchar_t* data_dir = HolyDataDir();
  if (!data_dir || !*data_dir) return;
  const std::wstring dir = std::wstring(data_dir) + L"\\frames";
  CreateDirectoryW(dir.c_str(), nullptr);
  static const bool cleared = [&dir] {
    remove_earlier(dir, L"present-*.bmp");
    return true;
  }();
  (void)cleared;
  const std::wstring path = dir + L"\\present-" + std::to_wstring(present) + L".bmp";
  std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
  for (UINT y = 0; y < height; ++y) {
    const uint8_t* row = rows + static_cast<size_t>(y) * pitch;
    uint8_t* out = pixels.data() + static_cast<size_t>(y) * width * 4;
    for (UINT x = 0; x < width; ++x, out += 4) {
      uint8_t r = 0, g = 0, b = 0;
      if (format == DXGI_FORMAT_R8G8B8A8_UNORM) {
        r = row[x * 4], g = row[x * 4 + 1], b = row[x * 4 + 2];
      } else if (format == DXGI_FORMAT_B8G8R8A8_UNORM) {
        b = row[x * 4], g = row[x * 4 + 1], r = row[x * 4 + 2];
      } else if (format == DXGI_FORMAT_R10G10B10A2_UNORM) {
        uint32_t v;
        std::memcpy(&v, row + x * 4, 4);
        r = static_cast<uint8_t>((v & 0x3FF) >> 2), g = static_cast<uint8_t>(((v >> 10) & 0x3FF) >> 2),
        b = static_cast<uint8_t>(((v >> 20) & 0x3FF) >> 2);
      } else {
        uint16_t h[3];
        std::memcpy(h, row + x * 8, 6);
        r = to_byte(half_to_float(h[0])), g = to_byte(half_to_float(h[1])), b = to_byte(half_to_float(h[2]));
      }
      out[0] = b, out[1] = g, out[2] = r, out[3] = 255;
    }
  }
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
  file.bfSize = static_cast<DWORD>(file.bfOffBits + pixels.size());
  const HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return;
  DWORD written = 0;
  WriteFile(handle, &file, sizeof(file), &written, nullptr);
  WriteFile(handle, &info, sizeof(info), &written, nullptr);
  WriteFile(handle, pixels.data(), static_cast<DWORD>(pixels.size()), &written, nullptr);
  CloseHandle(handle);
  LIB_LOG(lib::kInfo, "d3d12", "present %llu captured (%ux%u, format %u)", static_cast<unsigned long long>(present),
          width, height, format);
  // What was recorded since the previous capture, and memory use.
  LIB_LOG(lib::kInfo, "d3d12", "since the previous capture: %s", recorded_work_summary().c_str());
  PROCESS_MEMORY_COUNTERS_EX counters{};
  counters.cb = sizeof(counters);
  GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters));
  MEMORYSTATUSEX status{};
  status.dwLength = sizeof(status);
  GlobalMemoryStatusEx(&status);
  uint64_t textures = 0, texture_bytes = 0;
  host_texture_totals(&textures, &texture_bytes);
  LIB_LOG(lib::kInfo, "d3d12", "memory: process commit %.2f GiB, system commit available %.2f GiB; %llu host textures "
          "copied from title memory (%.2f GiB)", counters.PrivateUsage / 1073741824.0,
          status.ullAvailPageFile / 1073741824.0, static_cast<unsigned long long>(textures),
          texture_bytes / 1073741824.0);
}

// Reads `resource` back (in the common state) and writes it to a file.
void capture(Presenter* p, ID3D12Resource* resource, DXGI_FORMAT format, const D3D12_BOX& box) {
  D3D12_RESOURCE_DESC desc = resource->GetDesc();
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  UINT64 total = 0;
  desc.Width = box.right - box.left;
  desc.Height = box.bottom - box.top;
  desc.MipLevels = 1;
  desc.DepthOrArraySize = 1;
  p->device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &total);
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
  if (FAILED(p->device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST,
                                                nullptr, IID_PPV_ARGS(&readback)))) {
    return;
  }
  ID3D12CommandAllocator* allocator = p->allocators[p->next_allocator];
  wait_fence(p->fence, p->allocator_values[p->next_allocator], p->event);
  allocator->Reset();
  p->list->Reset(allocator, nullptr);
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Transition.pResource = resource;
  barrier.Transition.Subresource = 0;
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
  p->list->ResourceBarrier(1, &barrier);
  D3D12_TEXTURE_COPY_LOCATION source{}, target{};
  source.pResource = resource;
  source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  target.pResource = readback;
  target.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  target.PlacedFootprint = footprint;
  target.PlacedFootprint.Footprint.Format = format;
  p->list->CopyTextureRegion(&target, 0, 0, 0, &source, &box);
  std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
  p->list->ResourceBarrier(1, &barrier);
  p->list->Close();
  ID3D12CommandList* lists[] = {p->list};
  p->queue->ExecuteCommandLists(1, lists);
  p->queue->Signal(p->fence, ++p->fence_value);
  p->allocator_values[p->next_allocator] = p->fence_value;
  p->next_allocator = (p->next_allocator + 1) % kBuffers;
  wait_fence(p->fence, p->fence_value, p->event);
  void* data = nullptr;
  if (SUCCEEDED(readback->Map(0, nullptr, &data))) {
    write_capture(p->presents, format, footprint.Footprint.Width, footprint.Footprint.Height,
                  static_cast<const uint8_t*>(data), footprint.Footprint.RowPitch);
    readback->Unmap(0, nullptr);
  }
  readback->Release();
}

void flip_thread(Presenter* p) {
  const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  for (;;) {
    Flip flip;
    {
      std::unique_lock<std::mutex> lock(g_flip_lock);
      g_flip_ready.wait(lock, [] { return !g_flips.empty(); });
      flip = g_flips.front();
      g_flips.pop_front();
    }
    wait_fence(p->fence, flip.gpu_value, event);
    wait_frame_boundary();
    if (flip.token > g_presented_fence->GetCompletedValue()) g_presented_fence->Signal(flip.token);
  }
}

}  // namespace

uint64_t next_frame_token() {
  return g_tokens.fetch_add(1) + 1;
}

HRESULT wait_for_presented(uint64_t token, uint32_t timeout_ms) {
  if (!token || token > g_tokens.load()) return E_INVALIDARG;
  ID3D12Fence* fence = g_presented_fence;
  // Nothing presented yet: no frame is in flight, so there is nothing to
  // wait for.
  if (!fence) return S_OK;
  if (fence->GetCompletedValue() >= token) return S_OK;
  const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  fence->SetEventOnCompletion(token, event);
  const DWORD result = WaitForSingleObject(event, timeout_ms);
  CloseHandle(event);
  return result == WAIT_OBJECT_0 ? S_OK : static_cast<HRESULT>(0x887A0027);
}

HRESULT present_x(CommandQueue* queue, uint32_t plane_count, const PresentPlane* planes, const void*) {
  if (!plane_count || !planes || !planes[0].resource_count || !planes[0].resources) return E_INVALIDARG;
  const auto* title_resource = static_cast<const Resource*>(planes[0].resources[0]);
  if (!title_resource || !title_resource->host) return E_INVALIDARG;
  ID3D12Resource* resource = title_resource->host;
  const D3D12_RESOURCE_DESC desc = resource->GetDesc();
  const DXGI_FORMAT format = swap_chain_format(desc.Format);
  if (format == DXGI_FORMAT_UNKNOWN) {
    LIB_LOG(lib::kError, "d3d12", "display buffer format %u cannot be presented on the host", desc.Format);
    HolyUnimplementedFrom(kLibrary, "PresentX(display buffer format)", _ReturnAddress());
    return E_NOTIMPL;
  }
  D3D12_BOX box{0, 0, 0, static_cast<UINT>(desc.Width), desc.Height, 1};
  if (planes[0].source_rects) {
    const RECT& r = planes[0].source_rects[0];
    if (r.right > r.left && r.bottom > r.top && static_cast<UINT64>(r.right) <= desc.Width &&
        static_cast<UINT>(r.bottom) <= desc.Height) {
      box = {static_cast<UINT>(r.left), static_cast<UINT>(r.top), 0, static_cast<UINT>(r.right),
             static_cast<UINT>(r.bottom), 1};
    }
  }
  const UINT width = box.right - box.left, height = box.bottom - box.top;

  std::lock_guard<std::mutex> guard(g_lock);
  if (!g_presenter) {
    auto* made = new Presenter();
    if (!make_presenter(queue->device, queue->host, made)) {
      LIB_LOG(lib::kError, "d3d12", "cannot create the presentation objects");
      delete made;
      return E_FAIL;
    }
    g_presenter = made;
  }
  Presenter* p = g_presenter;
  ++p->presents;
  count_present(queue->device->host);
  HOLY_TRACE("present %llu of %llx: %ux%u format %u", static_cast<unsigned long long>(p->presents),
             static_cast<unsigned long long>(title_resource->title_va), width, height, static_cast<unsigned>(format));
  trace_present(p->presents);
  const uint64_t token = planes[0].token;
  if (ensure_swap_chain(p, format, width, height)) {
    LIB_LOG(lib::kDebug, "d3d12", "present %llu: window %p%s%s", static_cast<unsigned long long>(p->presents),
            static_cast<void*>(p->window), IsHungAppWindow(p->window) ? " (not responding)" : "",
            IsIconic(p->window) ? " (minimised)" : "");
    ID3D12Resource* back = nullptr;
    p->swap_chain->GetBuffer(p->swap_chain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&back));
    ID3D12CommandAllocator* allocator = p->allocators[p->next_allocator];
    wait_fence(p->fence, p->allocator_values[p->next_allocator], p->event);
    allocator->Reset();
    p->list->Reset(allocator, nullptr);
    D3D12_RESOURCE_BARRIER barriers[2]{};
    barriers[0].Transition = {resource, 0, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE};
    barriers[1].Transition = {back, 0, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST};
    p->list->ResourceBarrier(2, barriers);
    D3D12_TEXTURE_COPY_LOCATION source{}, target{};
    source.pResource = resource;
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    target.pResource = back;
    target.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    p->list->CopyTextureRegion(&target, 0, 0, 0, &source, &box);
    for (auto& barrier : barriers) std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    p->list->ResourceBarrier(2, barriers);
    p->list->Close();
    ID3D12CommandList* lists[] = {p->list};
    p->queue->ExecuteCommandLists(1, lists);
    // Sync interval 0: titles pace their frames with frame events (device
    // slot 80), so presenting does not wait for the display's refresh as
    // well. A windowed flip-model swap chain does not tear either way.
    check_point("present copy");
    const HRESULT hr = p->swap_chain->Present(0, 0);
    check_point("present");
    if (FAILED(hr)) {
      LIB_LOG(lib::kError, "d3d12", "host present failed: 0x%08lX", static_cast<unsigned long>(hr));
      report_device_removal(queue->device);
    }
    p->queue->Signal(p->fence, ++p->fence_value);
    p->allocator_values[p->next_allocator] = p->fence_value;
    p->next_allocator = (p->next_allocator + 1) % kBuffers;
    back->Release();
  } else if (p->presents == 1) {
    LIB_LOG(lib::kWarn, "d3d12", "the title presents without a visible window; frames are not shown");
  }
  if (p->presents == 1) {
    LIB_LOG(lib::kInfo, "d3d12", "first present: %ux%u, format %u, token %llu", width, height, desc.Format,
            static_cast<unsigned long long>(token));
  }
  if (capture_due(p->presents)) capture(p, resource, format, box);
  if (p->presents == texture_dump_frame()) dump_textures(queue->device, p->queue, p->presents);
  if (token) {
    uint64_t seen = g_last_presented.load();
    while (token > seen && !g_last_presented.compare_exchange_weak(seen, token)) {
    }
    p->queue->Signal(p->fence, ++p->fence_value);
    static std::once_flag started;
    std::call_once(started, [p] { std::thread(flip_thread, p).detach(); });
    {
      std::lock_guard<std::mutex> lock(g_flip_lock);
      g_flips.push_back({g_last_presented.load(), p->fence_value});
    }
    g_flip_ready.notify_one();
  }
  return S_OK;
}

}  // namespace holy::d3d12
