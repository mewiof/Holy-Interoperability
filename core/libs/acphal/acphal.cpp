// AcpHal.dll: the console's audio control processor interface. Implemented:
// the audio processor heap (memory shared with the audio hardware, which
// addresses it through 32-bit audio processor addresses).
//
// Observed (call sites; how results are checked):
//
//   HRESULT  ApuCreateHeap(uint32_t cached_bytes, uint32_t non_cached_bytes)
//            once at audio start-up; a failure is fatal to titles.
//   HRESULT  ApuAlloc(void** address, uint32_t* apu_address, uint32_t bytes,
//                     uint32_t alignment, uint32_t flags)
//            apu_address may be null; alignments of 2 KiB seen; flags 1 with
//            large requests, 2 with a request that just fits the non-cached
//            size (assumed: 1 the cached heap, 2 the non-cached one).
//   HRESULT  ApuFree(void* address)
//   bool     ApuIsVirtualAddressValid(const void* address, uint32_t bytes)
//   uint32_t ApuMapVirtualAddress(const void* address)
//   void*    ApuMapApuAddress(uint32_t apu_address)
//
// Host: both heaps are one reservation, committed as it is handed out; an
// audio processor address is the offset in it plus a fixed base (so 0 is
// never one). Allocation is first fit over each heap's free ranges.
//
// Shape contexts and the control processor (observed):
//
//   HRESULT AcpHalAllocateShapeContexts(ShapeContexts* contexts)
//           six 32-bit context counts the title fills in, then six pointers
//           this call writes: arrays of 64 bytes a context (the title clears
//           them itself).
//   void    AcpHalReleaseShapeContexts(void)
//   HRESULT AcpHalCreate(IAcpHal** out)
//           slot 0 (two 4 KiB sizes) once after creation, slot 2
//           (command, cookie, 0xFFFFFFFF, payload, extra) to send the
//           processor commands (results mostly not checked), slot 1 and 5
//           when audio shuts down.
//
// Host: a silent audio device. Context arrays are audio processor memory;
// commands are accepted and not carried out (nothing is decoded, mixed or
// played). Other object slots report themselves.
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <map>
#include <mutex>

#include "core/libs/support.h"
#include "core/libs/trace_file.h"
#include "core/libs/vtable.h"

namespace {

constexpr char kLibrary[] = "AcpHal.dll";
constexpr uint32_t kApuBase = 0x10000000;  // audio processor address of the reservation
constexpr uint32_t kCached = 1, kNonCached = 2;
constexpr uint64_t kCommitGranule = 0x10000;

struct Heap {
  uint64_t begin = 0, end = 0;          // offsets in the reservation
  std::map<uint64_t, uint64_t> free;    // offset -> size
};

struct State {
  std::mutex lock;
  uint8_t* base = nullptr;
  uint64_t size = 0;
  Heap heaps[2];                        // cached, non-cached
  std::map<uint64_t, uint64_t> used;    // offset -> size
};

State& state() {
  static auto* s = new State();
  return *s;
}

// Commits [offset, offset + bytes) of the reservation.
bool commit(State& s, uint64_t offset, uint64_t bytes) {
  const uint64_t first = offset & ~(kCommitGranule - 1);
  const uint64_t last = (offset + bytes + kCommitGranule - 1) & ~(kCommitGranule - 1);
  return VirtualAlloc(s.base + first, static_cast<SIZE_T>(last - first), MEM_COMMIT, PAGE_READWRITE) != nullptr;
}

bool inside(const State& s, const void* address, uint64_t bytes, uint64_t* offset) {
  const auto* at = static_cast<const uint8_t*>(address);
  if (!s.base || at < s.base) return false;
  const uint64_t o = static_cast<uint64_t>(at - s.base);
  if (o >= s.size || bytes > s.size - o) return false;
  *offset = o;
  return true;
}

}  // namespace

HOLY_EXPORT HRESULT ApuCreateHeap(uint32_t cached_bytes, uint32_t non_cached_bytes) {
  State& s = state();
  std::lock_guard<std::mutex> guard(s.lock);
  if (s.base) return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
  const uint64_t cached = (static_cast<uint64_t>(cached_bytes) + kCommitGranule - 1) & ~(kCommitGranule - 1);
  const uint64_t non_cached = (static_cast<uint64_t>(non_cached_bytes) + kCommitGranule - 1) & ~(kCommitGranule - 1);
  const uint64_t size = cached + non_cached;
  if (!size || size > 0xFFFFFFFFull - kApuBase) return E_INVALIDARG;
  s.base = static_cast<uint8_t*>(VirtualAlloc(nullptr, static_cast<SIZE_T>(size), MEM_RESERVE, PAGE_READWRITE));
  if (!s.base) return E_OUTOFMEMORY;
  s.size = size;
  s.heaps[0] = Heap{0, cached, {}};
  s.heaps[1] = Heap{cached, size, {}};
  if (cached) s.heaps[0].free[0] = cached;
  if (non_cached) s.heaps[1].free[cached] = non_cached;
  LIB_LOG(holy::lib::kInfo, "acphal", "audio processor heap: 0x%x cached and 0x%x non-cached bytes at %p",
          cached_bytes, non_cached_bytes, static_cast<void*>(s.base));
  return S_OK;
}

HOLY_EXPORT HRESULT ApuAlloc(void** address, uint32_t* apu_address, uint32_t bytes, uint32_t alignment,
                             uint32_t flags) {
  if (!address) return E_POINTER;
  *address = nullptr;
  if (apu_address) *apu_address = 0;
  if (!bytes || (alignment & (alignment - 1)) || (flags != kCached && flags != kNonCached)) return E_INVALIDARG;
  if (!alignment) alignment = 1;
  State& s = state();
  std::lock_guard<std::mutex> guard(s.lock);
  if (!s.base) return E_UNEXPECTED;  // no heap yet
  Heap& heap = s.heaps[flags == kCached ? 0 : 1];
  for (auto it = heap.free.begin(); it != heap.free.end(); ++it) {
    const uint64_t start = (it->first + alignment - 1) & ~static_cast<uint64_t>(alignment - 1);
    const uint64_t range_end = it->first + it->second;
    if (start + bytes > range_end) continue;
    if (!commit(s, start, bytes)) return E_OUTOFMEMORY;
    const uint64_t before = start - it->first, after = range_end - (start + bytes);
    const uint64_t range_start = it->first;
    heap.free.erase(it);
    if (before) heap.free[range_start] = before;
    if (after) heap.free[start + bytes] = after;
    s.used[start] = bytes;
    *address = s.base + start;
    if (apu_address) *apu_address = kApuBase + static_cast<uint32_t>(start);
    return S_OK;
  }
  LIB_LOG(holy::lib::kWarn, "acphal", "audio processor heap %u has no room for 0x%x bytes", flags, bytes);
  return E_OUTOFMEMORY;
}

HOLY_EXPORT HRESULT ApuFree(void* address) {
  State& s = state();
  std::lock_guard<std::mutex> guard(s.lock);
  uint64_t offset = 0;
  if (!inside(s, address, 0, &offset)) return E_INVALIDARG;
  auto used = s.used.find(offset);
  if (used == s.used.end()) return E_INVALIDARG;
  const uint64_t bytes = used->second;
  s.used.erase(used);
  Heap& heap = offset < s.heaps[0].end ? s.heaps[0] : s.heaps[1];
  // Back into the free ranges, merged with its neighbours.
  auto next = heap.free.lower_bound(offset);
  uint64_t start = offset, size = bytes;
  if (next != heap.free.begin()) {
    auto previous = std::prev(next);
    if (previous->first + previous->second == start) {
      start = previous->first;
      size += previous->second;
      heap.free.erase(previous);
    }
  }
  if (next != heap.free.end() && next->first == offset + bytes) {
    size += next->second;
    heap.free.erase(next);
  }
  heap.free[start] = size;
  return S_OK;
}

HOLY_EXPORT bool ApuIsVirtualAddressValid(const void* address, uint32_t bytes) {
  State& s = state();
  std::lock_guard<std::mutex> guard(s.lock);
  uint64_t offset = 0;
  if (!inside(s, address, bytes, &offset)) return false;
  // Within one allocation.
  auto it = s.used.upper_bound(offset);
  if (it == s.used.begin()) return false;
  --it;
  return offset + bytes <= it->first + it->second;
}

HOLY_EXPORT uint32_t ApuMapVirtualAddress(const void* address) {
  State& s = state();
  std::lock_guard<std::mutex> guard(s.lock);
  uint64_t offset = 0;
  return inside(s, address, 0, &offset) ? kApuBase + static_cast<uint32_t>(offset) : 0;
}

HOLY_EXPORT void* ApuMapApuAddress(uint32_t apu_address) {
  State& s = state();
  std::lock_guard<std::mutex> guard(s.lock);
  if (!s.base || apu_address < kApuBase || apu_address - kApuBase >= s.size) return nullptr;
  return s.base + (apu_address - kApuBase);
}

namespace {

constexpr int kContextKinds = 6;
constexpr uint32_t kContextBytes = 64;

struct ShapeContexts {
  uint32_t counts[kContextKinds];
  void* contexts[kContextKinds];
};
static_assert(sizeof(ShapeContexts) == 0x48);

void* g_contexts[kContextKinds] = {};

struct ControlProcessor {
  void* const* vtable;
  const holy::lib::VtableInfo* info;
};

const holy::lib::VtableInfo kProcessorInfo{kLibrary, "IAcpHal"};

HRESULT STDMETHODCALLTYPE processor_initialize(ControlProcessor*, uint32_t a, uint32_t b) {
  LIB_LOG(holy::lib::kInfo, "acphal", "control processor started (0x%x, 0x%x): a silent audio device", a, b);
  return S_OK;
}
HRESULT STDMETHODCALLTYPE processor_shutdown(ControlProcessor*) {
  return S_OK;
}
// HOLY_AUDIO_TRACE: the control processor's commands (number, cookie, mask,
// extra, then the payload's first 64 bytes) and shape context allocations,
// to <data>/logs/audio-acphal.txt (core/libs/trace_file.h).
holy::lib::TraceFile g_trace(L"HOLY_AUDIO_TRACE", L"audio-acphal.txt");

HRESULT STDMETHODCALLTYPE processor_submit(ControlProcessor*, uint32_t command, uint32_t cookie, uint32_t mask,
                                           const void* payload, uint32_t extra) {
  if (g_trace.on()) {
    const uint32_t words[4] = {command, cookie, mask, extra};
    g_trace.event("submit", words, 4);
    if (payload) g_trace.event("payload", payload, 16);
  }
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1) < 16) {
    LIB_LOG(holy::lib::kDebug, "acphal", "command %u (cookie 0x%x, mask 0x%x, payload %p, 0x%x): not carried out",
            command, cookie, mask, payload, extra);
  }
  return S_OK;
}
ULONG STDMETHODCALLTYPE processor_release(ControlProcessor*) {
  return 0;
}

const auto kProcessorVtable = [] {
  auto table = holy::lib::make_reporting_vtable<16>();
  table[0] = reinterpret_cast<void*>(&processor_initialize);
  table[1] = reinterpret_cast<void*>(&processor_shutdown);
  table[2] = reinterpret_cast<void*>(&processor_submit);
  table[5] = reinterpret_cast<void*>(&processor_release);
  return table;
}();

ControlProcessor g_processor{kProcessorVtable.data(), &kProcessorInfo};

}  // namespace

HOLY_EXPORT HRESULT AcpHalAllocateShapeContexts(ShapeContexts* contexts) {
  if (!contexts) return E_POINTER;
  for (int kind = 0; kind < kContextKinds; ++kind) {
    contexts->contexts[kind] = nullptr;
    if (!contexts->counts[kind]) continue;
    if (g_contexts[kind]) ApuFree(g_contexts[kind]);
    g_contexts[kind] = nullptr;
    const uint64_t bytes = static_cast<uint64_t>(contexts->counts[kind]) * kContextBytes;
    if (bytes > 0xFFFFFFFFu) return E_INVALIDARG;
    const HRESULT hr = ApuAlloc(&g_contexts[kind], nullptr, static_cast<uint32_t>(bytes), kContextBytes, kCached);
    if (FAILED(hr)) return hr;
    std::memset(g_contexts[kind], 0, static_cast<size_t>(bytes));
    contexts->contexts[kind] = g_contexts[kind];
  }
  if (g_trace.on()) g_trace.event("shape contexts", contexts, sizeof(ShapeContexts) / 4);
  LIB_LOG(holy::lib::kInfo, "acphal", "shape contexts: %u %u %u %u %u %u", contexts->counts[0], contexts->counts[1],
          contexts->counts[2], contexts->counts[3], contexts->counts[4], contexts->counts[5]);
  return S_OK;
}

HOLY_EXPORT void AcpHalReleaseShapeContexts() {
  for (void*& context : g_contexts) {
    if (context) ApuFree(context);
    context = nullptr;
  }
}

HOLY_EXPORT HRESULT AcpHalCreate(ControlProcessor** out) {
  if (!out) return E_POINTER;
  *out = &g_processor;
  return S_OK;
}
