#include "core/runtime/memory.h"

#include <algorithm>
#include <atomic>
#include <vector>

#include "core/common/log.h"

namespace holy::rt {
namespace {

constexpr char kComponent[] = "memory";
constexpr uintptr_t kChunk = 0x10000;

struct Range {
  uintptr_t begin;
  uintptr_t end;
  DWORD protect;
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

// Commits the chunk around `address` if it lies in a registered range and is
// still only reserved. Returns true if the page is (now) committed.
bool commit_chunk(uintptr_t address) {
  Range range;
  if (!find_range(address, &range)) return false;
  MEMORY_BASIC_INFORMATION info;
  if (!VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info))) return false;
  if (info.State == MEM_COMMIT) return true;
  if (info.State != MEM_RESERVE) return false;
  uintptr_t begin = std::max(address & ~(kChunk - 1), range.begin);
  uintptr_t end = std::min((address & ~(kChunk - 1)) + kChunk, range.end);
  // Stay inside the reserved region the page belongs to.
  const auto region_begin = reinterpret_cast<uintptr_t>(info.BaseAddress);
  const uintptr_t region_end = region_begin + info.RegionSize;
  begin = std::max(begin, region_begin);
  end = std::min(end, region_end);
  if (!VirtualAlloc(reinterpret_cast<void*>(begin), end - begin, MEM_COMMIT, range.protect)) {
    if (!g_reported_failure.exchange(true)) {
      LOG_ERROR(kComponent, "the host refused to commit title memory on demand (error %lu, %llu MiB committed so far)",
                GetLastError(), static_cast<unsigned long long>(g_committed_on_demand.load() >> 20));
    }
    return false;
  }
  g_committed_on_demand.fetch_add(end - begin);
  return true;
}

LONG CALLBACK commit_handler(EXCEPTION_POINTERS* info) {
  const EXCEPTION_RECORD* record = info->ExceptionRecord;
  if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || record->NumberParameters < 2) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  const DWORD saved_error = GetLastError();
  const bool committed = commit_chunk(record->ExceptionInformation[1]);
  SetLastError(saved_error);
  return committed ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

void install_commit_on_demand() {
  AddVectoredExceptionHandler(1, &commit_handler);
}

void register_commit_on_demand(void* base, size_t size, DWORD protect) {
  const auto begin = reinterpret_cast<uintptr_t>(base);
  AcquireSRWLockExclusive(&g_lock);
  std::vector<Range>& list = ranges();
  // A new registration replaces whatever overlapped it (the earlier range
  // was released and the address space reused).
  list.erase(std::remove_if(list.begin(), list.end(),
                            [&](const Range& range) { return range.begin < begin + size && begin < range.end; }),
             list.end());
  const Range range{begin, begin + size, protect};
  list.insert(std::upper_bound(list.begin(), list.end(), range,
                               [](const Range& a, const Range& b) { return a.begin < b.begin; }),
              range);
  ReleaseSRWLockExclusive(&g_lock);
  LOG_DEBUG(kComponent, "commit on demand for %p + 0x%llx", base, static_cast<unsigned long long>(size));
}

bool ensure_committed(const void* address, size_t size) {
  if (!address || size == 0) return true;
  const auto begin = reinterpret_cast<uintptr_t>(address);
  const uintptr_t end = begin + size;
  Range range;
  // Fast path: nothing registered in the buffer's neighbourhood.
  {
    AcquireSRWLockShared(&g_lock);
    const bool any = !ranges().empty();
    ReleaseSRWLockShared(&g_lock);
    if (!any) return true;
  }
  bool ok = true;
  for (uintptr_t page = begin & ~static_cast<uintptr_t>(0xFFF); page < end; page += 0x1000) {
    if (!find_range(page, &range)) continue;
    MEMORY_BASIC_INFORMATION info;
    if (!VirtualQuery(reinterpret_cast<void*>(page), &info, sizeof(info))) continue;
    if (info.State == MEM_COMMIT) {
      // Skip the rest of this committed region.
      const uintptr_t region_end = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
      page = std::max(page, region_end - 0x1000);
      continue;
    }
    if (!commit_chunk(page)) ok = false;
  }
  return ok;
}

}  // namespace holy::rt
