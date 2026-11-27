// Write tracking by page protection (core/libs/d3d12_xs/write_watch.h).
#include "core/libs/d3d12_xs/write_watch.h"

#include <windows.h>

#include <atomic>
#include <thread>

#include "tests/unit/test.h"

using namespace holy::d3d12;

namespace {

// The callback runs from the fault handler, which the compiler cannot see:
// what it records is read through atomics, after a signal fence.
std::atomic<int> g_count{0};
std::atomic<uint64_t> g_pages[16];
void record(uint64_t begin, uint64_t) {
  const int i = g_count.load();
  if (i < 16) g_pages[i].store(begin);
  g_count.store(i + 1);
}
int reports() {
  std::atomic_signal_fence(std::memory_order_seq_cst);
  return g_count.load();
}
uint64_t reported(int i) { return g_pages[i].load(); }

void write(volatile uint8_t* at, uint8_t value) {
  *at = value;
  std::atomic_signal_fence(std::memory_order_seq_cst);
}

bool writable(const volatile uint8_t* address) {
  MEMORY_BASIC_INFORMATION info{};
  return VirtualQuery(const_cast<const uint8_t*>(address), &info, sizeof(info)) && info.Protect == PAGE_READWRITE;
}

volatile uint8_t* fresh_memory() {
  start_write_watch(&record);
  g_count.store(0);
  return static_cast<volatile uint8_t*>(VirtualAlloc(nullptr, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
}

}  // namespace

TEST(a_write_to_a_watched_page_is_reported_once_and_taken) {
  volatile uint8_t* memory = fresh_memory();
  EXPECT(memory != nullptr);
  if (!memory) return;
  const auto base = reinterpret_cast<uint64_t>(memory);
  // 0x100..0x2100: three pages.
  EXPECT(watch_memory(base + 0x100, 0x2000, 7));
  EXPECT(!writable(memory) && !writable(memory + 0x2000));
  EXPECT(writable(memory + 0x3000));
  write(memory + 0x3000, 1);  // outside: no report
  EXPECT(reports() == 0);
  write(memory + 0x1800, 2);
  EXPECT(reports() == 1 && reported(0) == base + 0x1000);
  EXPECT(memory[0x1800] == 2);
  write(memory + 0x10, 3);  // the watch is gone: no second report
  EXPECT(reports() == 1);
  EXPECT(writable(memory) && writable(memory + 0x2000));
  VirtualFree(const_cast<uint8_t*>(memory), 0, MEM_RELEASE);
}

TEST(watches_sharing_a_page_go_together_and_can_be_dropped) {
  volatile uint8_t* memory = fresh_memory();
  if (!memory) return;
  const auto base = reinterpret_cast<uint64_t>(memory);
  EXPECT(watch_memory(base, 0x1000, 1));
  EXPECT(watch_memory(base + 0x800, 0x1000, 2));  // shares the first page, holds the second
  EXPECT(watch_memory(base + 0x4000, 0x1000, 3));
  unwatch(3);
  EXPECT(writable(memory + 0x4000));
  write(memory + 0x4000, 1);  // dropped: no report
  EXPECT(reports() == 0);
  write(memory + 0x10, 1);  // the shared page: both watches go
  EXPECT(reports() == 1);
  EXPECT(writable(memory + 0x1000));  // the second watch's other page, given back
  uint64_t watches = 0, pages = 0;
  watch_totals(&watches, &pages);
  EXPECT(watches == 0 && pages == 0);
  VirtualFree(const_cast<uint8_t*>(memory), 0, MEM_RELEASE);
}

TEST(rewatching_forgetting_and_other_threads) {
  volatile uint8_t* memory = fresh_memory();
  if (!memory) return;
  const auto base = reinterpret_cast<uint64_t>(memory);
  EXPECT(watch_memory(base, 0x3000, 9));
  EXPECT(watch_memory(base, 0x3000, 9));  // a refresh watches the same range again
  EXPECT(!writable(memory + 0x1000));
  forget_memory(base + 0x2000, 0x10);  // remapped there: dropped without a report
  EXPECT(reports() == 0);
  EXPECT(writable(memory + 0x1000));
  // Another thread's write is reported as well.
  EXPECT(watch_memory(base, 0x1000, 10));
  std::thread writer([memory] { write(memory + 0x20, 5); });
  writer.join();
  EXPECT(reports() == 1 && reported(0) == base);
  // Memory that is not read-write is left alone.
  DWORD old = 0;
  VirtualProtect(const_cast<uint8_t*>(memory + 0x8000), 0x1000, PAGE_READONLY, &old);
  EXPECT(!watch_memory(base + 0x8000, 0x1000, 11));
  VirtualFree(const_cast<uint8_t*>(memory), 0, MEM_RELEASE);
}
