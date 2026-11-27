#include "core/libs/d3d12_xs/page_cache.h"

#include <d3d12.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/libs/d3d12_xs/call_check.h"
#include "core/libs/d3d12_xs/device.h"
#include "core/libs/d3d12_xs/gpu_memory.h"
#include "core/libs/d3d12_xs/shader/translate.h"
#include "core/libs/d3d12_xs/texture_upload.h"
#include "core/libs/support.h"

namespace holy::d3d12 {
namespace {

constexpr uint64_t kPage = 0x10000;
constexpr uint64_t kPoolBytes = 512ull << 20;  // one memory slot each
constexpr uint32_t kPoolPages = static_cast<uint32_t>(kPoolBytes / kPage);
constexpr uint32_t kStartsPerSubmission = 2048;  // copies begun per submission (128 MiB)
constexpr uint64_t kWantSpan = 64 * kPage;        // pages wanted per buffer at most

uint64_t budget_bytes() {
  static const uint64_t bytes = [] {
    wchar_t text[16] = {};
    if (!GetEnvironmentVariableW(L"HOLY_BUFFER_COPIES", text, 16)) return uint64_t{0};
    const uint64_t mib = wcstoull(text, nullptr, 10);
    return std::min<uint64_t>(mib << 20, static_cast<uint64_t>(kPageCacheLines) * kPage);
  }();
  return bytes;
}

const lib::Counter& c_cached() {
  static const lib::Counter c("d3d12.cached_pages");
  return c;
}
const lib::Counter& c_refreshed() {
  static const lib::Counter c("d3d12.cached_page_refreshes");
  return c;
}
const lib::Counter& c_rejected() {
  static const lib::Counter c("d3d12.cached_pages_rejected");
  return c;
}

enum class State : uint8_t { Free, Filling, Ready };

struct Line {
  uint64_t page = 0;
  uint32_t pool_page = 0;
  State state = State::Free;
  uint64_t filled_at = 0;  // the copy queue's value when the copy is done
  uint8_t history = 0;     // CPU writes over the last scans, newest in bit 0
  PagePlace place;
};

struct Cache {
  std::mutex lock;
  bool started = false;
  bool failed = false;
  std::vector<Line> lines = std::vector<Line>(kPageCacheLines);
  std::vector<uint32_t> free_lines;
  std::vector<ID3D12Resource*> pools;
  std::vector<uint32_t> pool_slots;
  std::vector<uint32_t> free_pool_pages;  // pool * kPoolPages + page
  std::unordered_map<uint64_t, uint32_t> line_of;  // page -> line (filling or ready)
  std::unordered_set<uint64_t> rejected;            // written by the GPU, or by the CPU that often
  std::vector<std::pair<GpuEpoch, uint32_t>> retired;  // lines free once the GPU is done
  // The copy queue.
  ID3D12CommandQueue* queue = nullptr;
  ID3D12CommandAllocator* allocator = nullptr;
  ID3D12GraphicsCommandList* list = nullptr;
  ID3D12Fence* fence = nullptr;
  std::atomic<uint64_t> value{0};
  HANDLE event = nullptr;
  bool recording = false;
};

Cache& cache() {
  static auto* instance = new Cache();
  return *instance;
}

// Fed under other locks (the memory map's, recording threads'): a leaf lock.
std::mutex g_inbox_lock;
std::unordered_set<uint64_t> g_wants;
std::vector<std::pair<uint64_t, uint64_t>> g_written;
std::vector<uint32_t> g_dropped;
std::vector<std::pair<uint64_t, uint64_t>> g_gpu_writes;

void dropped(uint32_t line) {
  std::lock_guard<std::mutex> guard(g_inbox_lock);
  g_dropped.push_back(line);
}

bool start(Device* device, Cache& c) {
  if (c.started) return !c.failed;
  c.started = true;
  c.failed = true;
  page_table_address(device);  // makes the flags with the page table
  // A direct queue: the buffers copied are unordered-access views of
  // translated programs on the title's queues too, and copy queues take only
  // resources in the common and copy states (with copies on a copy queue the
  // host removed the device).
  D3D12_COMMAND_QUEUE_DESC desc{};
  desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  const bool made =
      SUCCEEDED(device->host->CreateCommandQueue(&desc, IID_PPV_ARGS(&c.queue))) &&
      SUCCEEDED(device->host->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&c.allocator))) &&
      SUCCEEDED(device->host->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, c.allocator, nullptr,
                                                IID_PPV_ARGS(&c.list))) &&
      SUCCEEDED(c.list->Close()) && SUCCEEDED(device->host->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&c.fence))) &&
      (c.event = CreateEventW(nullptr, FALSE, FALSE, nullptr)) != nullptr && cache_flags() != nullptr;
  if (!made) {
    LIB_LOG(lib::kError, "d3d12", "no copy queue for title memory copies in video memory: none are made");
    return false;
  }
  c.queue->SetName(L"holy title memory copies");
  register_gpu_timeline(c.fence, &c.value);
  for (uint32_t i = kPageCacheLines; i-- > 0;) c.free_lines.push_back(i);
  set_cache_dropped_hook(&dropped);
  c.failed = false;
  LIB_LOG(lib::kInfo, "d3d12", "title memory read by translated programs is copied into video memory (up to %llu MiB)",
          static_cast<unsigned long long>(budget_bytes() >> 20));
  return true;
}

// A pool page, a pool made if the budget allows; false when there is none.
bool take_pool_page(Device* device, Cache& c, uint32_t* out) {
  if (c.free_pool_pages.empty()) {
    if ((c.pools.size() + 1) * kPoolBytes > ((budget_bytes() + kPoolBytes - 1) / kPoolBytes) * kPoolBytes) return false;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = kPoolBytes;
    desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ID3D12Resource* pool = nullptr;
    if (FAILED(device->host->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON,
                                                     nullptr, IID_PPV_ARGS(&pool)))) {
      static bool logged = false;
      if (!logged) LIB_LOG(lib::kWarn, "d3d12", "video memory for title memory copies refused: no more are made");
      logged = true;
      return false;
    }
    const uint32_t slot = add_buffer_slots(device, pool, kPoolBytes);
    if (!slot) {
      pool->Release();
      return false;
    }
    pool->SetName(L"holy title memory copies");
    const auto index = static_cast<uint32_t>(c.pools.size());
    c.pools.push_back(pool);
    c.pool_slots.push_back(slot);
    // Same budget order as made (lowest pages first).
    for (uint32_t i = kPoolPages; i-- > 0;) c.free_pool_pages.push_back(index * kPoolPages + i);
  }
  *out = c.free_pool_pages.back();
  c.free_pool_pages.pop_back();
  return true;
}

// The copy list, open for copies (waiting for the previous ones).
void begin_copies(Cache& c) {
  if (c.recording) return;
  if (c.fence->GetCompletedValue() < c.value.load()) {
    c.fence->SetEventOnCompletion(c.value.load(), c.event);
    WaitForSingleObject(c.event, INFINITE);
  }
  c.allocator->Reset();
  c.list->Reset(c.allocator, nullptr);
  c.recording = true;
}

void copy_page(Cache& c, Line& line) {
  begin_copies(c);
  const uint32_t pool = line.pool_page / kPoolPages;
  c.list->CopyBufferRegion(c.pools[pool], static_cast<uint64_t>(line.pool_page % kPoolPages) * kPage, line.place.buffer,
                           line.place.buffer_offset, kPage);
}

void retire(Cache& c, uint32_t index) {
  Line& line = c.lines[index];
  if (line.state == State::Ready) c_cached().add(-1);
  c.line_of.erase(line.page);
  line.state = State::Free;
  c.retired.emplace_back(gpu_epoch_now(), index);
}

void reject(Cache& c, uint64_t page, const char* why) {
  if (!c.rejected.insert(page).second) return;
  c_rejected().add(1);
  LIB_LOG(lib::kDebug, "d3d12", "title memory page %p is not copied into video memory: %s", reinterpret_cast<void*>(page),
          why);
}

// Takes a page out of the cache (its entry back to the page itself).
void uncache(Cache& c, uint32_t index) {
  Line& line = c.lines[index];
  if (line.state == State::Ready) uncache_page(line.page, index);
  retire(c, index);
}

// Whether the host watches writes to the memory holding `page` (asked of the
// page itself: 0.2 us).
bool watched(uint64_t page) {
  void* written[1];
  ULONG_PTR count = 1;
  DWORD granularity = 0;
  return GetWriteWatch(0, reinterpret_cast<void*>(page), kPage, written, &count, &granularity) == 0;
}

// Pages the CPU wrote among the cached ones (write watching, with reset), by
// runs of consecutive pages; the textures over them are told too, since this
// scan resets what the texture scan would see.
void scan_writes(Cache& c, std::vector<uint64_t>* written_pages) {
  std::vector<uint64_t> pages;
  pages.reserve(c.line_of.size());
  for (const auto& [page, line] : c.line_of) pages.push_back(page);
  std::sort(pages.begin(), pages.end());
  static void* found[4096];
  for (size_t i = 0; i < pages.size();) {
    size_t j = i + 1;
    while (j < pages.size() && pages[j] == pages[j - 1] + kPage) ++j;
    const uint64_t begin = pages[i], end = pages[j - 1] + kPage;
    for (;;) {
      ULONG_PTR count = sizeof(found) / sizeof(found[0]);
      DWORD granularity = 0;
      if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, reinterpret_cast<void*>(begin), static_cast<SIZE_T>(end - begin), found,
                        &count, &granularity) != 0) {
        break;
      }
      for (ULONG_PTR k = 0; k < count; ++k) {
        const auto at = reinterpret_cast<uint64_t>(found[k]);
        written_pages->push_back(at & ~(kPage - 1));
        texture_memory_changed(at, granularity);
      }
      if (count < sizeof(found) / sizeof(found[0])) break;
    }
    i = j;
  }
}

}  // namespace

bool page_cache_enabled() {
  return budget_bytes() != 0;
}

void page_cache_want(uint64_t address, uint64_t size) {
  if (!page_cache_enabled() || !address) return;
  const uint64_t first = address & ~(kPage - 1);
  uint64_t last = (address + (size ? size : 1) - 1) & ~(kPage - 1);
  if (last - first >= kWantSpan) last = first + kWantSpan - kPage;
  for (uint64_t page = first; page <= last; page += kPage) {
    const uint32_t word = page_entry_word(page);
    if (!word || (word & kCachedPageBit)) continue;
    std::lock_guard<std::mutex> guard(g_inbox_lock);
    g_wants.insert(page);
  }
}

void page_cache_gpu_writes(uint64_t address, uint64_t size) {
  if (!page_cache_enabled() || !address || !size) return;
  std::lock_guard<std::mutex> guard(g_inbox_lock);
  g_gpu_writes.emplace_back(address, size);
}

void page_cache_written(uint64_t page, uint64_t bytes) {
  if (!page_cache_enabled()) return;
  std::lock_guard<std::mutex> guard(g_inbox_lock);
  g_written.emplace_back(page, bytes);
}

void page_cache_before_submit(Device* device, ID3D12CommandQueue* queue) {
  if (!page_cache_enabled()) return;
  Cache& c = cache();
  std::lock_guard<std::mutex> guard(c.lock);
  if (!start(device, c)) return;
  std::unordered_set<uint64_t> wants;
  std::vector<std::pair<uint64_t, uint64_t>> reported, gpu_writes;
  std::vector<uint32_t> gone;
  {
    std::lock_guard<std::mutex> inbox(g_inbox_lock);
    wants.swap(g_wants);
    reported.swap(g_written);
    gone.swap(g_dropped);
    gpu_writes.swap(g_gpu_writes);
  }
  // Lines whose entries the memory map dropped (memory released or remapped).
  for (uint32_t index : gone) {
    if (c.lines[index].state != State::Free) retire(c, index);
  }
  // GPU writes other than translated ones, recorded for this or later work.
  for (const auto& [address, size] : gpu_writes) {
    for (uint64_t page = address & ~(kPage - 1); page < address + size; page += kPage) {
      reject(c, page, "the GPU writes it other than through translated programs");
      auto found = c.line_of.find(page);
      if (found != c.line_of.end()) uncache(c, found->second);
    }
  }
  // Lines the GPU is done with since they left the cache (their flags
  // cleared: no work can set them any more).
  volatile uint32_t* flags = cache_flags();
  {
    size_t done = 0;
    while (done < c.retired.size() && gpu_epoch_done(c.retired[done].first)) {
      const uint32_t index = c.retired[done].second;
      flags[index] = 0;
      c.free_pool_pages.push_back(c.lines[index].pool_page);
      c.free_lines.push_back(index);
      ++done;
    }
    c.retired.erase(c.retired.begin(), c.retired.begin() + static_cast<std::ptrdiff_t>(done));
  }
  // Flagged lines (translated writes): out for good. Filled lines: in.
  const uint64_t completed = c.fence->GetCompletedValue();
  std::vector<uint32_t> active;
  active.reserve(c.line_of.size());
  for (const auto& [page, index] : c.line_of) active.push_back(index);
  for (uint32_t index : active) {
    Line& line = c.lines[index];
    if (line.state == State::Ready && flags[index]) {
      reject(c, line.page, "a translated program wrote it");
      uncache(c, index);
    } else if (line.state == State::Filling && completed >= line.filled_at) {
      const uint32_t pool = line.pool_page / kPoolPages;
      const auto offset = static_cast<uint32_t>((line.pool_page % kPoolPages) * kPage);
      if (cache_page(line.page, line.place, index, c.pool_slots[pool], offset)) {
        line.state = State::Ready;
        c_cached().add(1);
        LIB_LOG(lib::kDebug, "d3d12", "title memory page %p read from video memory (line %u)",
                reinterpret_cast<void*>(line.page), index);
      } else {
        retire(c, index);  // the page moved meanwhile
      }
    }
  }
  // The CPU's writes: copies refreshed (filling ones copied again); pages
  // written in three of the last eight scans leave the cache.
  std::vector<uint64_t> written;
  scan_writes(c, &written);
  for (const auto& [page, bytes] : reported) {
    for (uint64_t at = page & ~(kPage - 1); at < page + bytes; at += kPage) written.push_back(at);
  }
  std::sort(written.begin(), written.end());
  written.erase(std::unique(written.begin(), written.end()), written.end());
  for (auto& [page, index] : c.line_of) c.lines[index].history = static_cast<uint8_t>(c.lines[index].history << 1);
  std::vector<uint32_t> refresh;
  for (uint64_t page : written) {
    auto found = c.line_of.find(page);
    if (found == c.line_of.end()) continue;
    Line& line = c.lines[found->second];
    line.history |= 1;
    if (std::popcount(static_cast<unsigned>(line.history)) >= 3) {
      reject(c, page, "the CPU writes it often");
      uncache(c, found->second);
    } else {
      refresh.push_back(found->second);
    }
  }
  bool copies = false;
  for (uint32_t index : refresh) {
    copy_page(c, c.lines[index]);
    copies = true;
    c_refreshed().add(1);
  }
  // New copies for pages calls read.
  uint32_t started = 0;
  std::vector<uint32_t> filling = refresh;
  for (uint64_t page : wants) {
    if (started == kStartsPerSubmission) break;
    if (c.line_of.count(page) || c.rejected.count(page) || c.free_lines.empty()) continue;
    PagePlace place;
    if (!page_place(page, &place)) continue;
    if (!watched(page)) {
      reject(c, page, "the host does not watch writes to it");
      continue;
    }
    uint32_t pool_page = 0;
    if (!take_pool_page(device, c, &pool_page)) break;
    const uint32_t index = c.free_lines.back();
    c.free_lines.pop_back();
    Line& line = c.lines[index];
    line = Line{page, pool_page, State::Filling, 0, 0, place};
    c.line_of[page] = index;
    ResetWriteWatch(reinterpret_cast<void*>(page), kPage);  // writes from now on are seen; the copy may hold them
    copy_page(c, line);
    copies = true;
    filling.push_back(index);
    ++started;
    LIB_LOG(lib::kDebug, "d3d12", "title memory page %p copied into video memory (line %u)", reinterpret_cast<void*>(page),
            index);
  }
  if (!copies) return;
  c.list->Close();
  c.recording = false;
  ID3D12CommandList* lists[] = {c.list};
  c.queue->ExecuteCommandLists(1, lists);
  const uint64_t value = c.value.load() + 1;
  c.queue->Signal(c.fence, value);
  check_point("title memory copies");
  c.value.store(value, std::memory_order_release);
  for (uint32_t index : filling) {
    if (c.lines[index].state == State::Filling) c.lines[index].filled_at = value;
  }
  // The submission's work reads the refreshed copies.
  queue->Wait(c.fence, value);
}

}  // namespace holy::d3d12
