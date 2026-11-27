#include "core/runtime/memory.h"

#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <vector>

#include "core/common/log.h"
#include "core/runtime/stats.h"

namespace holy::rt {
namespace {

constexpr char kComponent[] = "memory";
constexpr uintptr_t kChunk = 0x10000;

// Which chunks of a range are known to be committed: one bit per 64 KiB
// chunk, counted from the range's first chunk. Set when this file commits a
// chunk (or finds it committed), cleared when the title decommits or releases
// memory (note_decommit). It saves asking the host about memory that is
// already committed: observed, titles stream into buffers of many megabytes
// and descriptor heaps are read and written by the thousand, and a host query
// per buffer kept loading threads in the kernel for most of their time.
class ChunkMap {
 public:
  ChunkMap(uintptr_t begin, uintptr_t end)
      : base_(begin & ~(kChunk - 1)), words_(((end - base_ + kChunk - 1) / kChunk + 63) / 64) {}

  bool test(uintptr_t address) const {
    const uintptr_t chunk = (address - base_) / kChunk;
    return (words_[chunk / 64].load(std::memory_order_relaxed) >> (chunk % 64)) & 1;
  }
  // Marks the chunks overlapping [begin, end).
  void set(uintptr_t begin, uintptr_t end) { update(begin, end, true); }
  void clear(uintptr_t begin, uintptr_t end) { update(begin, end, false); }

 private:
  void update(uintptr_t begin, uintptr_t end, bool value) {
    if (end <= begin) return;
    const uintptr_t first = (std::max(begin, base_) - base_) / kChunk;
    const uintptr_t last = std::min((end - 1 - base_) / kChunk, words_.size() * 64 - 1);
    for (uintptr_t chunk = first; chunk <= last; ++chunk) {
      const uint64_t bit = 1ull << (chunk % 64);
      if (value) {
        words_[chunk / 64].fetch_or(bit, std::memory_order_relaxed);
      } else {
        words_[chunk / 64].fetch_and(~bit, std::memory_order_relaxed);
      }
    }
  }

  uintptr_t base_;
  std::vector<std::atomic<uint64_t>> words_;
};

struct Range {
  uintptr_t begin;
  uintptr_t end;
  DWORD protect;
  std::shared_ptr<ChunkMap> chunks;
};

SRWLOCK g_lock = SRWLOCK_INIT;
std::vector<Range>& ranges() {
  static auto* storage = new std::vector<Range>();
  return *storage;
}
std::atomic<uint64_t> g_committed_on_demand{0};
std::atomic<bool> g_reported_failure{false};

bool find_range(uintptr_t address, Range* out) {
  AcquireSRWLockShared(&g_lock);
  const std::vector<Range>& list = ranges();
  auto it = std::upper_bound(list.begin(), list.end(), address,
                             [](uintptr_t value, const Range& range) { return value < range.begin; });
  bool found = false;
  if (it != list.begin()) {
    --it;
    if (address < it->end) {
      *out = *it;
      found = true;
    }
  }
  ReleaseSRWLockShared(&g_lock);
  return found;
}

// The first registered range overlapping [begin, end), if any.
bool find_overlap(uintptr_t begin, uintptr_t end, Range* out) {
  AcquireSRWLockShared(&g_lock);
  const std::vector<Range>& list = ranges();
  // The first range ending after `begin` (ranges do not overlap).
  auto it = std::upper_bound(list.begin(), list.end(), begin,
                             [](uintptr_t value, const Range& range) { return value < range.end; });
  const bool found = it != list.end() && it->begin < end;
  if (found) *out = *it;
  ReleaseSRWLockShared(&g_lock);
  return found;
}

void count_commit(uintptr_t bytes) {
  g_committed_on_demand.fetch_add(bytes);
  static volatile int64_t* const committed = counter("memory.committed_on_first_touch_bytes");
  _InterlockedExchangeAdd64(committed, static_cast<int64_t>(bytes));
}

void report_refusal() {
  if (!g_reported_failure.exchange(true)) {
    LOG_ERROR(kComponent, "the host refused to commit title memory on demand (error %lu, %llu MiB committed so far)",
              GetLastError(), static_cast<unsigned long long>(g_committed_on_demand.load() >> 20));
  }
}

// Commits the reserved parts of [begin, end) inside `range`, asking the host
// region by region (one query covers a run of pages in the same state).
bool commit_by_regions(uintptr_t begin, uintptr_t end, const Range& range) {
  bool ok = true;
  for (uintptr_t page = std::max(begin, range.begin) & ~static_cast<uintptr_t>(0xFFF);
       page < std::min(end, range.end);) {
    MEMORY_BASIC_INFORMATION info;
    if (!VirtualQuery(reinterpret_cast<void*>(page), &info, sizeof(info))) return false;
    const auto region_begin = reinterpret_cast<uintptr_t>(info.BaseAddress);
    const uintptr_t region_end = region_begin + info.RegionSize;
    const uintptr_t span_end = std::min({region_end, end, range.end});
    if (info.State == MEM_RESERVE) {
      const uintptr_t commit_begin = std::max({page & ~(kChunk - 1), region_begin, range.begin});
      const uintptr_t commit_end = std::min({(span_end + kChunk - 1) & ~(kChunk - 1), region_end, range.end});
      if (VirtualAlloc(reinterpret_cast<void*>(commit_begin), commit_end - commit_begin, MEM_COMMIT,
                       range.protect)) {
        count_commit(commit_end - commit_begin);
        range.chunks->set(commit_begin, commit_end);
      } else {
        report_refusal();
        ok = false;
      }
    } else if (info.State == MEM_COMMIT) {
      // Whole chunks only: a chunk is marked once all of it is committed.
      const uintptr_t whole_begin = (std::max(page, range.begin) + kChunk - 1) & ~(kChunk - 1);
      const uintptr_t whole_end = std::min(region_end, range.end) & ~(kChunk - 1);
      if (whole_end > whole_begin) range.chunks->set(whole_begin, whole_end);
    } else {
      ok = false;
    }
    page = span_end;
  }
  return ok;
}

// Commits the chunk around `address` if it lies in a registered range and is
// still only reserved. Returns true if the page is (now) committed.
bool commit_chunk(uintptr_t address, bool write) {
  Range range;
  if (!find_range(address, &range)) return false;
  MEMORY_BASIC_INFORMATION info;
  if (!VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info))) return false;
  // Committed meanwhile (another thread's fault): retried. A write to a page
  // committed read-only is not this handler's (write watches make pages
  // read-only); retrying it would fault forever.
  if (info.State == MEM_COMMIT) {
    const DWORD protect = info.Protect & 0xFF;
    const bool writable = protect == PAGE_READWRITE || protect == PAGE_EXECUTE_READWRITE ||
                          protect == PAGE_WRITECOPY || protect == PAGE_EXECUTE_WRITECOPY;
    return !write || writable;
  }
  if (info.State != MEM_RESERVE) return false;
  uintptr_t begin = std::max(address & ~(kChunk - 1), range.begin);
  uintptr_t end = std::min((address & ~(kChunk - 1)) + kChunk, range.end);
  // Stay inside the reserved region the page belongs to.
  const auto region_begin = reinterpret_cast<uintptr_t>(info.BaseAddress);
  const uintptr_t region_end = region_begin + info.RegionSize;
  begin = std::max(begin, region_begin);
  end = std::min(end, region_end);
  if (!VirtualAlloc(reinterpret_cast<void*>(begin), end - begin, MEM_COMMIT, range.protect)) {
    report_refusal();
    return false;
  }
  count_commit(end - begin);
  if (begin == (address & ~(kChunk - 1)) && end == begin + kChunk) range.chunks->set(begin, end);
  return true;
}

LONG CALLBACK commit_handler(EXCEPTION_POINTERS* info) {
  const EXCEPTION_RECORD* record = info->ExceptionRecord;
  if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || record->NumberParameters < 2) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  const DWORD saved_error = GetLastError();
  const bool committed = commit_chunk(record->ExceptionInformation[1], record->ExceptionInformation[0] == 1);
  SetLastError(saved_error);
  return committed ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

void install_commit_on_demand() {
  AddVectoredExceptionHandler(1, &commit_handler);
}

void register_commit_on_demand(void* base, size_t size, DWORD protect) {
  const auto begin = reinterpret_cast<uintptr_t>(base);
  Range range{begin, begin + size, protect, std::make_shared<ChunkMap>(begin, begin + size)};
  AcquireSRWLockExclusive(&g_lock);
  std::vector<Range>& list = ranges();
  // A new registration replaces whatever overlapped it (the earlier range
  // was released and the address space reused).
  list.erase(std::remove_if(list.begin(), list.end(),
                            [&](const Range& r) { return r.begin < range.end && range.begin < r.end; }),
             list.end());
  list.insert(std::upper_bound(list.begin(), list.end(), range,
                               [](const Range& a, const Range& b) { return a.begin < b.begin; }),
              std::move(range));
  ReleaseSRWLockExclusive(&g_lock);
  LOG_DEBUG(kComponent, "commit on demand for %p + 0x%llx", base, static_cast<unsigned long long>(size));
}

void note_decommit(const void* address, size_t size) {
  const auto begin = reinterpret_cast<uintptr_t>(address);
  const uintptr_t end = begin + size;
  Range range;
  for (uintptr_t at = begin; at < end && find_overlap(at, end, &range); at = range.end) {
    range.chunks->clear(std::max(begin, range.begin), std::min(end, range.end));
  }
}

void note_release(const void* address, size_t size) {
  const auto begin = reinterpret_cast<uintptr_t>(address);
  const uintptr_t end = begin + size;
  AcquireSRWLockExclusive(&g_lock);
  std::vector<Range>& list = ranges();
  list.erase(std::remove_if(list.begin(), list.end(), [&](const Range& r) { return r.begin >= begin && r.end <= end; }),
             list.end());
  // Ranges released only in part keep their registration; the released part
  // is no longer known to be committed.
  for (Range& r : list) {
    if (r.begin < end && begin < r.end) r.chunks->clear(std::max(begin, r.begin), std::min(end, r.end));
  }
  ReleaseSRWLockExclusive(&g_lock);
}

namespace {
std::atomic<int (*)(const void*, size_t)> g_backing_hook{nullptr};
std::atomic<void (*)(const void*, size_t)> g_release_hook{nullptr};
std::atomic<void (*)(const void*, size_t)> g_write_hook{nullptr};
}  // namespace

void set_write_hook(void (*hook)(const void* address, size_t size)) {
  g_write_hook.store(hook);
}

void before_write(const void* address, size_t size) {
  if (!address || !size) return;
  if (const auto hook = g_write_hook.load()) hook(address, size);
}

void set_release_hook(void (*hook)(const void* address, size_t size)) {
  g_release_hook.store(hook);
}

void before_release(const void* address, size_t size) {
  if (!address || !size) return;
  if (const auto hook = g_release_hook.load()) hook(address, size);
}

void set_backing_hook(int (*hook)(const void* address, size_t size)) {
  g_backing_hook.store(hook);
}

bool ensure_committed(const void* address, size_t size, bool verify) {
  if (!address || size == 0) return true;
  if (const auto hook = g_backing_hook.load()) {
    if (!hook(address, size)) return false;
  }
  const auto begin = reinterpret_cast<uintptr_t>(address);
  const uintptr_t end = begin + size;
  bool ok = true;
  Range range;
  for (uintptr_t at = begin; at < end && find_overlap(at, end, &range); at = range.end) {
    const uintptr_t lo = std::max(begin, range.begin), hi = std::min(end, range.end);
    if (verify) {
      range.chunks->clear(lo, hi);
      if (!commit_by_regions(lo, hi, range)) ok = false;
      continue;
    }
    // Runs of chunks not known to be committed are committed in one call
    // each (committing pages that are already committed succeeds); a run the
    // host refuses that way (it crosses allocations) is committed region by
    // region.
    for (uintptr_t chunk = lo & ~(kChunk - 1); chunk < hi;) {
      if (range.chunks->test(std::max(chunk, range.begin))) {
        chunk += kChunk;
        continue;
      }
      uintptr_t run_end = chunk + kChunk;
      while (run_end < hi && !range.chunks->test(run_end)) run_end += kChunk;
      const uintptr_t commit_begin = std::max(chunk, range.begin), commit_end = std::min(run_end, range.end);
      if (VirtualAlloc(reinterpret_cast<void*>(commit_begin), commit_end - commit_begin, MEM_COMMIT, range.protect)) {
        count_commit(commit_end - commit_begin);
        range.chunks->set(commit_begin, commit_end);
      } else if (!commit_by_regions(commit_begin, commit_end, range)) {
        ok = false;
      }
      chunk = run_end;
    }
  }
  return ok;
}

}  // namespace holy::rt
