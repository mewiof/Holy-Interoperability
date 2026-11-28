#include "core/libs/d3d12_xs/write_watch.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <functional>
#include <unordered_map>
#include <vector>

namespace holy::d3d12 {
namespace {

constexpr uint64_t kPage = 0x1000;

struct Watch {
  std::vector<uint64_t> pages;  // the pages it holds (read-only while any watch holds them)
  std::vector<uint64_t> aliases;  // those of them that are other views of watched storage
};

// The lock guarding the maps. Code holding it never writes title memory, so
// a fault taken while holding it is not one of ours (g_owner).
SRWLOCK g_lock = SRWLOCK_INIT;
std::atomic<DWORD> g_owner{0};
std::unordered_map<WatchToken, Watch>* g_watches = nullptr;
std::unordered_map<uint64_t, std::vector<WatchToken>>* g_pages = nullptr;  // page -> watches holding it
// Alias page -> (watch, the watched page it shows): a write there is reported
// as a write to the watched page.
std::unordered_map<uint64_t, std::vector<std::pair<WatchToken, uint64_t>>>* g_aliases = nullptr;
std::atomic<void (*)(uint64_t, uint64_t)> g_written{nullptr};

void lock() {
  AcquireSRWLockExclusive(&g_lock);
  g_owner.store(GetCurrentThreadId(), std::memory_order_relaxed);
  if (!g_watches) {
    g_watches = new std::unordered_map<WatchToken, Watch>();
    g_pages = new std::unordered_map<uint64_t, std::vector<WatchToken>>();
    g_aliases = new std::unordered_map<uint64_t, std::vector<std::pair<WatchToken, uint64_t>>>();
  }
}
void unlock() {
  g_owner.store(0, std::memory_order_relaxed);
  ReleaseSRWLockExclusive(&g_lock);
}

// Makes `pages` writable again, a run of consecutive pages per call (lock
// held), each call inside one region (VirtualProtect stays in an allocation).
void restore(std::vector<uint64_t>& pages) {
  std::sort(pages.begin(), pages.end());
  for (size_t i = 0; i < pages.size();) {
    size_t j = i + 1;
    while (j < pages.size() && pages[j] == pages[j - 1] + kPage) ++j;
    uint64_t at = pages[i];
    const uint64_t end = pages[j - 1] + kPage;
    while (at < end) {
      MEMORY_BASIC_INFORMATION info{};
      if (!VirtualQuery(reinterpret_cast<void*>(at), &info, sizeof(info))) break;
      const uint64_t stop = std::min(end, reinterpret_cast<uint64_t>(info.BaseAddress) + info.RegionSize);
      DWORD old = 0;
      if (info.State == MEM_COMMIT && info.Protect == PAGE_READONLY) {
        VirtualProtect(reinterpret_cast<void*>(at), stop - at, PAGE_READWRITE, &old);
      }
      at = stop;
    }
    i = j;
  }
}

// Drops `token`'s watch (lock held), adding the pages no watch holds any
// more to `freed`.
void remove_locked(WatchToken token, std::vector<uint64_t>* freed) {
  auto it = g_watches->find(token);
  if (it == g_watches->end()) return;
  for (uint64_t page : it->second.aliases) {
    auto shown = g_aliases->find(page);
    if (shown == g_aliases->end()) continue;
    auto& list = shown->second;
    list.erase(std::remove_if(list.begin(), list.end(), [token](const auto& e) { return e.first == token; }),
               list.end());
    if (list.empty()) g_aliases->erase(shown);
  }
  for (uint64_t page : it->second.pages) {
    auto holders = g_pages->find(page);
    if (holders == g_pages->end()) continue;
    auto& tokens = holders->second;
    tokens.erase(std::remove(tokens.begin(), tokens.end(), token), tokens.end());
    if (tokens.empty()) {
      g_pages->erase(holders);
      freed->push_back(page);
    }
  }
  g_watches->erase(it);
}

// Drops every watch holding a page of [begin, end) (lock held).
void drop_locked(uint64_t begin, uint64_t end, std::vector<uint64_t>* freed) {
  for (uint64_t page = begin & ~(kPage - 1); page < end; page += kPage) {
    auto holders = g_pages->find(page);
    if (holders == g_pages->end()) continue;
    const std::vector<WatchToken> tokens = holders->second;  // remove_locked edits the original
    for (WatchToken token : tokens) remove_locked(token, freed);
  }
}

LONG CALLBACK on_write_fault(EXCEPTION_POINTERS* info) {
  const EXCEPTION_RECORD* record = info->ExceptionRecord;
  if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || record->NumberParameters < 2 ||
      record->ExceptionInformation[0] != 1) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  if (g_owner.load(std::memory_order_relaxed) == GetCurrentThreadId()) return EXCEPTION_CONTINUE_SEARCH;
  const uint64_t page = static_cast<uint64_t>(record->ExceptionInformation[1]) & ~(kPage - 1);
  std::vector<uint64_t> freed;
  std::vector<uint64_t> shown;  // watched pages an alias written shows
  lock();
  const bool ours = g_pages->count(page) != 0;
  if (ours) {
    auto alias = g_aliases->find(page);
    if (alias != g_aliases->end()) {
      for (const auto& entry : alias->second) shown.push_back(entry.second);
    }
    drop_locked(page, page + kPage, &freed);
    for (uint64_t watched : shown) drop_locked(watched, watched + kPage, &freed);
    restore(freed);
  }
  unlock();
  if (!ours) {
    // Another thread may have dropped the watch since the write faulted: the
    // page is writable again and the write can be retried.
    MEMORY_BASIC_INFORMATION query{};
    const bool writable = VirtualQuery(reinterpret_cast<void*>(page), &query, sizeof(query)) &&
                          query.State == MEM_COMMIT && query.Protect == PAGE_READWRITE;
    return writable ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
  }
  if (const auto written = g_written.load(std::memory_order_acquire)) {
    written(page, kPage);
    for (uint64_t watched : shown) written(watched, kPage);
  }
  return EXCEPTION_CONTINUE_EXECUTION;
}

}  // namespace

void start_write_watch(void (*written)(uint64_t begin, uint64_t size)) {
  g_written.store(written, std::memory_order_release);
  static const bool installed = [] {
    // First in line: a write to a watched page is ours whatever else handles
    // access violations (commit on demand, lazily backed physical pages).
    AddVectoredExceptionHandler(1, &on_write_fault);
    return true;
  }();
  (void)installed;
}

bool watch_memory(uint64_t begin, uint64_t size, WatchToken token) {
  if (!size) return false;
  const uint64_t end = begin + size;
  // Other views of the same physical pages (asked before taking this file's
  // lock: the memory library takes its own).
  std::vector<std::pair<uint64_t, uint64_t>> aliases;  // (alias page, watched page)
  for_each_alias(begin, size, [&](uint64_t alias, uint64_t granule, bool backed) {
    if (!backed) return;  // no view to protect yet: its backing is reported as a change (memory_aliases)
    for (uint64_t at = 0; at < 0x10000; at += kPage) {
      const uint64_t shown = granule + at;
      if (shown + kPage <= begin || shown >= end) continue;
      aliases.emplace_back(alias + at, shown);
    }
  });
  std::vector<uint64_t> freed;
  lock();
  // An earlier watch of the token goes first, its pages writable again so
  // the loop below takes them as any other.
  remove_locked(token, &freed);
  restore(freed);
  Watch watch;
  for (uint64_t at = begin & ~(kPage - 1); at < end;) {
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(reinterpret_cast<void*>(at), &info, sizeof(info))) break;
    const uint64_t stop = std::min(end, reinterpret_cast<uint64_t>(info.BaseAddress) + info.RegionSize);
    if (info.State == MEM_COMMIT && info.Protect == PAGE_READWRITE) {
      // Pages no watch holds yet (those are read-only, other regions).
      DWORD old = 0;
      if (VirtualProtect(reinterpret_cast<void*>(at), stop - at, PAGE_READONLY, &old)) {
        for (uint64_t page = at; page < stop; page += kPage) {
          (*g_pages)[page].push_back(token);
          watch.pages.push_back(page);
        }
      }
    } else if (info.State == MEM_COMMIT && info.Protect == PAGE_READONLY) {
      for (uint64_t page = at; page < stop; page += kPage) {
        auto holders = g_pages->find(page);
        if (holders == g_pages->end()) continue;  // read-only for its own reasons
        holders->second.push_back(token);
        watch.pages.push_back(page);
      }
    }
    at = stop;
  }
  for (const auto& [alias, shown] : aliases) {
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(reinterpret_cast<void*>(alias), &info, sizeof(info)) || info.State != MEM_COMMIT) continue;
    DWORD old = 0;
    if (info.Protect == PAGE_READWRITE) {
      if (!VirtualProtect(reinterpret_cast<void*>(alias), kPage, PAGE_READONLY, &old)) continue;
    } else if (info.Protect != PAGE_READONLY || !g_pages->count(alias)) {
      continue;  // read-only for its own reasons
    }
    (*g_pages)[alias].push_back(token);
    (*g_aliases)[alias].emplace_back(token, shown);
    watch.pages.push_back(alias);
    watch.aliases.push_back(alias);
  }
  const bool watched = !watch.pages.empty();
  if (watched) (*g_watches)[token] = std::move(watch);
  unlock();
  return watched;
}

void for_each_alias(uint64_t begin, uint64_t size,
                    const std::function<void(uint64_t alias, uint64_t granule, bool backed)>& visit) {
  using EnumerateFn = void (*)(const void*, uint64_t, void (*)(void*, uintptr_t, uintptr_t, int), void*);
  static const auto enumerate = reinterpret_cast<EnumerateFn>(
      GetProcAddress(GetModuleHandleW(L"xmem.dll"), "HolyXMemEnumerateAliases"));
  if (!enumerate) return;
  enumerate(reinterpret_cast<const void*>(begin), size,
            [](void* context, uintptr_t alias, uintptr_t granule, int backed) {
              (*static_cast<const std::function<void(uint64_t, uint64_t, bool)>*>(context))(alias, granule,
                                                                                            backed != 0);
            },
            const_cast<void*>(static_cast<const void*>(&visit)));
}

void unwatch(WatchToken token) {
  std::vector<uint64_t> freed;
  lock();
  remove_locked(token, &freed);
  restore(freed);
  unlock();
}

void forget_memory(uint64_t begin, uint64_t size) {
  if (!size) return;
  std::vector<uint64_t> freed;
  lock();
  drop_locked(begin, begin + size, &freed);
  restore(freed);
  unlock();
}

void watch_totals(uint64_t* watches, uint64_t* pages) {
  lock();
  *watches = g_watches->size();
  *pages = g_pages->size();
  unlock();
}

}  // namespace holy::d3d12
