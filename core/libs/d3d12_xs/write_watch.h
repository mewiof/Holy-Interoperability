// Write tracking by page protection, for title memory whose writes the host
// does not track (mapped views of physical pages: write watching does not
// apply to views; see texture_upload.h).
//
// A watch makes the pages of a range read-only; the first CPU write to any of
// them (the fault is caught here) makes them writable again, drops every
// watch holding them and reports the written page to the callback. Measured
// host behaviour (development host): protection changes are refused on memory
// a heap was opened over, but not on another view of the same section (the
// GPU reaches physical pages through views of its own), and a write to a
// read-only view faults.
//
// Pages are 4 KiB: a write next to a watched range in the same page drops its
// watch too. Only committed read-write pages are watched; others are left
// alone.
#pragma once

#include <cstdint>
#include <functional>

namespace holy::d3d12 {

using WatchToken = uintptr_t;

// Installs the fault handler (once) and the callback written pages are
// reported to. The callbacks run in the faulting thread, inside the fault
// handler: they must only take leaf locks. `stale` (optional) hears of a
// write that faulted on a page this file left read-only without a watch
// holding it (made writable and reported as written; see on_write_fault).
void start_write_watch(void (*written)(uint64_t begin, uint64_t size), void (*stale)(uint64_t page) = nullptr);

// Watches [begin, begin + size) for `token` (an earlier watch of the token
// goes first); false if no page could be watched.
bool watch_memory(uint64_t begin, uint64_t size, WatchToken token);
// Calls `visit` for every other granule (64 KiB) showing a physical page
// that [begin, begin + size) shows: titles map the same pages at a CPU and a
// GPU address (observed: video frames written at one, textures placed at the
// other). Watches cover the backed aliases of their range: a write there is
// reported as a write to the page it shows.
void for_each_alias(uint64_t begin, uint64_t size,
                    const std::function<void(uint64_t alias, uint64_t granule, bool backed)>& visit);
// Drops `token`'s watch.
void unwatch(WatchToken token);
// [begin, begin + size) changed in another way (remapped, released): the
// watches holding its pages are dropped, without a report.
void forget_memory(uint64_t begin, uint64_t size);

// Watches and watched pages now, for tests and the log.
void watch_totals(uint64_t* watches, uint64_t* pages);

}  // namespace holy::d3d12
