#include "core/runtime/stats.h"

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "core/common/log.h"

namespace holy::rt {
namespace {

constexpr char kComponent[] = "stats";
constexpr size_t kMaxCounters = 1024;
constexpr size_t kMaxName = 64;

struct Cell {
  char name[kMaxName];
  volatile int64_t value;
  int64_t reported;  // value at the previous report (report thread only)
};

// Cells never move: callers keep pointers to their values.
Cell g_cells[kMaxCounters];
size_t g_count = 0;
SRWLOCK g_lock = SRWLOCK_INIT;
ULONGLONG g_last_report = 0;

std::string gib(uint64_t bytes) {
  char text[32];
  std::snprintf(text, sizeof(text), "%.2f", static_cast<double>(bytes) / (1ull << 30));
  return text;
}

DWORD WINAPI report_thread(void* context) {
  const auto interval = static_cast<DWORD>(reinterpret_cast<uintptr_t>(context));
  for (;;) {
    Sleep(interval);
    log_stats("periodic");
  }
}

}  // namespace

volatile int64_t* counter(const char* name) {
  if (!name || !*name) return nullptr;
  AcquireSRWLockExclusive(&g_lock);
  for (size_t i = 0; i < g_count; ++i) {
    if (std::strncmp(g_cells[i].name, name, kMaxName - 1) == 0) {
      ReleaseSRWLockExclusive(&g_lock);
      return &g_cells[i].value;
    }
  }
  volatile int64_t* cell = nullptr;
  if (g_count < kMaxCounters) {
    Cell& c = g_cells[g_count++];
    std::strncpy(c.name, name, kMaxName - 1);
    c.name[kMaxName - 1] = '\0';
    cell = &c.value;
  } else {
    // Full: everything past the table shares the last cell.
    cell = &g_cells[kMaxCounters - 1].value;
  }
  ReleaseSRWLockExclusive(&g_lock);
  return cell;
}

void start_stats_reports(uint32_t interval_ms) {
  static bool started = false;
  if (started || interval_ms == 0) return;
  started = true;
  g_last_report = GetTickCount64();
  HANDLE thread = CreateThread(nullptr, 64 * 1024, &report_thread,
                               reinterpret_cast<void*>(static_cast<uintptr_t>(interval_ms)),
                               STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
  if (thread) {
    SetThreadDescription(thread, L"holy stats");
    CloseHandle(thread);
  }
}

void log_stats(const char* reason) {
  PROCESS_MEMORY_COUNTERS_EX process{};
  process.cb = sizeof(process);
  GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&process), sizeof(process));
  PERFORMANCE_INFORMATION system{};
  system.cb = sizeof(system);
  GetPerformanceInfo(&system, sizeof(system));
  const ULONGLONG now = GetTickCount64();
  AcquireSRWLockExclusive(&g_lock);
  const double seconds = g_last_report ? static_cast<double>(now - g_last_report) / 1000.0 : 0.0;
  g_last_report = now;
  LOG_INFO(kComponent, "%s (%.1f s): commit %s GiB (peak %s), working set %s GiB; system commit %s of %s GiB",
           reason, seconds, gib(process.PrivateUsage).c_str(), gib(process.PeakPagefileUsage).c_str(),
           gib(process.WorkingSetSize).c_str(), gib(static_cast<uint64_t>(system.CommitTotal) * system.PageSize).c_str(),
           gib(static_cast<uint64_t>(system.CommitLimit) * system.PageSize).c_str());
  // One line per component (the part of the name before the first dot),
  // counters in name order.
  std::vector<Cell*> order;
  order.reserve(g_count);
  for (size_t i = 0; i < g_count; ++i) order.push_back(&g_cells[i]);
  std::sort(order.begin(), order.end(), [](const Cell* a, const Cell* b) { return std::strcmp(a->name, b->name) < 0; });
  std::string line, component;
  auto flush = [&] {
    if (!line.empty()) LOG_INFO(kComponent, "  %s:%s", component.c_str(), line.c_str());
    line.clear();
  };
  for (Cell* cell : order) {
    const char* dot = std::strchr(cell->name, '.');
    const std::string prefix = dot ? std::string(cell->name, static_cast<size_t>(dot - cell->name)) : std::string();
    if (prefix != component) {
      flush();
      component = prefix;
    }
    const int64_t value = cell->value;
    const int64_t delta = value - cell->reported;
    cell->reported = value;
    char text[160];
    if (delta) {
      std::snprintf(text, sizeof(text), " %s=%lld(%+lld)", dot ? dot + 1 : cell->name,
                    static_cast<long long>(value), static_cast<long long>(delta));
    } else {
      std::snprintf(text, sizeof(text), " %s=%lld", dot ? dot + 1 : cell->name, static_cast<long long>(value));
    }
    line += text;
    if (line.size() > 600) flush();
  }
  flush();
  ReleaseSRWLockExclusive(&g_lock);
}

}  // namespace holy::rt
