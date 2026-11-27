// Physical page allocation and mapping.
//
// Observed contract (call sites in titles), shaped like the desktop's
// user-physical-pages functions with 64 KiB pages:
//
//   BOOL  XMemAllocatePhysicalPages(uint64_t attributes, ULONG_PTR* count, ULONG_PTR* pages)
//         count is in/out, in 64 KiB pages (titles round byte sizes up with >> 16);
//         success is TRUE and the full count.
//   BOOL  XMemFreePhysicalPages(ULONG_PTR count, ULONG_PTR* pages)
//   void* XMemMapPhysicalPages(void* address, ULONG_PTR count, ULONG_PTR* pages)
//         maps the pages at consecutive 64 KiB granules of a reservation made with
//         XMemVirtualAlloc; a null page array (or a zero entry) unmaps. Titles
//         test the result as a pointer.
//   BOOL  XMemMapPhysicalPagesScatter(void** addresses, ULONG_PTR count, ULONG_PTR* pages)
//   HRESULT XMemTransferMemory(uint32_t, size_t bytes)   (assumed return type;
//         titles do not act on the result)
//
// Titles also reserve placeholder regions themselves (MEM_RESERVE_PLACEHOLDER)
// and replace parts of them with private memory (MEM_REPLACE_PLACEHOLDER).
//
// Host model: a physical page is an opaque id. Its storage is a 64 KiB slice
// of a pagefile-backed section, chosen when the page is first mapped: every
// region pages are mapped into is backed by sections laid out like the region
// (one per chunk of up to 1 GiB), and a fresh page takes the slice at the
// offset of the granule it is first mapped at. Pages a title maps side by side
// are therefore contiguous in one section, which lets the graphics library
// open one GPU heap over them (HolyXMemQueryPhysical). Mapping replaces a
// 64 KiB placeholder with a view of the page's slice, so a page mapped at
// several addresses is the same memory, as titles' CPU and GPU views expect.
// Sections are reserved; pages are committed when they get storage, and the
// graphics library commits the blocks it opens GPU heaps over.
//
// Storage is created lazily: mapping a page that has none yet only records
// the mapping. The page gets its storage, and the granule its view, on first
// CPU access (a vectored exception handler), when a buffer in it is handed to
// the kernel (HolySetBackingHook), or when the graphics library asks for the
// memory behind it (HolyXMemQueryPhysical). Fresh storage reads as zero
// either way. Titles map large pools up front (several GiB of texture pool)
// and use them gradually; commit charge follows use instead of the mapping.
#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <atomic>
#include <unordered_map>
#include <unordered_set>
#include <cstring>
#include <vector>

#include "core/libs/support.h"

namespace {

constexpr ULONG_PTR kPage = 0x10000;
constexpr uint64_t kChunk = 1ull << 30;

// Statistics (the runtime's report): storage the sections hold (committed or
// only reserved), pages handed out, pages with storage, granules with a view.
const holy::lib::Counter& c_section_committed() {
  static const holy::lib::Counter c("xmem.physical_committed_bytes");
  return c;
}
const holy::lib::Counter& c_section_reserved() {
  static const holy::lib::Counter c("xmem.physical_reserved_bytes");
  return c;
}
const holy::lib::Counter& c_page_commit() {
  static const holy::lib::Counter c("xmem.physical_page_commit_bytes");
  return c;
}
const holy::lib::Counter& c_pages_live() {
  static const holy::lib::Counter c("xmem.physical_pages");
  return c;
}
const holy::lib::Counter& c_pages_stored() {
  static const holy::lib::Counter c("xmem.physical_pages_stored");
  return c;
}
const holy::lib::Counter& c_granules_backed() {
  static const holy::lib::Counter c("xmem.physical_views");
  return c;
}

struct Section {
  HANDLE handle = nullptr;
  bool committed = false;
  std::vector<uint32_t> owner;  // page id owning each 64 KiB slot, 0 if free
  std::vector<bool> used;       // the slot has held a page (its contents are not zero)
};

struct Page {
  int32_t section = -1;  // -1 until first mapped
  uint32_t index = 0;
  bool live = false;
  bool committed = false;
  uint32_t maps = 0;  // granules it is mapped at (recorded or backed)
};

struct Region {
  uintptr_t begin = 0;
  uintptr_t end = 0;
  std::vector<int32_t> chunks;  // section per chunk, -1 if not created yet
};

// The lock guarding the page tables. Code holding it never touches title
// memory (title arrays are copied in and out around it): a fault on title
// memory would enter the backing handler, which takes the lock too. The
// owner is recorded so the handler can step aside for faults raised by the
// lock's own holder.
SRWLOCK g_lock = SRWLOCK_INIT;
std::atomic<DWORD> g_lock_owner{0};

// HolyXMemObserveMappings: told, with g_lock held, when a granule gets its
// page's storage and when it is unmapped.
using MappingObserver = void (*)(void* context, uintptr_t granule, HANDLE section, uint64_t offset);
MappingObserver g_observer = nullptr;
void* g_observer_context = nullptr;

void lock() {
  AcquireSRWLockExclusive(&g_lock);
  g_lock_owner.store(GetCurrentThreadId());
}
void unlock() {
  g_lock_owner.store(0);
  ReleaseSRWLockExclusive(&g_lock);
}
std::vector<Section>& sections() {
  static auto* storage = new std::vector<Section>();
  return *storage;
}
std::vector<Page>& pages() {
  static auto* storage = new std::vector<Page>();
  return *storage;
}
std::unordered_map<uintptr_t, ULONG_PTR>& mapped() {  // granule address -> page id
  static auto* storage = new std::unordered_map<uintptr_t, ULONG_PTR>();
  return *storage;
}
std::vector<Region>& regions() {
  static auto* storage = new std::vector<Region>();
  return *storage;
}
// Granules whose mapping is recorded but not yet backed by a view.
std::unordered_set<uintptr_t>& pending() {
  static auto* storage = new std::unordered_set<uintptr_t>();
  return *storage;
}

// Placeholder functions of the host (Windows 10 1803 and later).
using VirtualAlloc2Fn = PVOID(WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, void*, ULONG);
using MapViewOfFile3Fn = PVOID(WINAPI*)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, void*, ULONG);
using UnmapViewOfFile2Fn = BOOL(WINAPI*)(HANDLE, PVOID, ULONG);

template <typename Fn>
Fn kernelbase(const char* name) {
  static const HMODULE module = GetModuleHandleW(L"kernelbase.dll");
  return reinterpret_cast<Fn>(GetProcAddress(module, name));
}

constexpr ULONG kMemReservePlaceholder = 0x00040000;
constexpr ULONG kMemReplacePlaceholder = 0x00004000;
constexpr ULONG kMemPreservePlaceholder = 0x00000002;

bool commit_exhausted(DWORD error) {
  return error == ERROR_COMMITMENT_LIMIT || error == ERROR_NOT_ENOUGH_MEMORY || error == ERROR_OUTOFMEMORY ||
         error == ERROR_COMMITMENT_MINIMUM;
}

Region* find_region(uintptr_t address) {
  for (Region& region : regions()) {
    if (address >= region.begin && address < region.end) return &region;
  }
  return nullptr;
}

void add_region(uintptr_t begin, uintptr_t end) {
  Region region;
  region.begin = begin;
  region.end = end;
  region.chunks.assign(static_cast<size_t>((end - begin + kChunk - 1) / kChunk), -1);
  regions().push_back(std::move(region));
}

// Sections are reserved: each page takes commit charge when it gets storage
// (back_granule), and the graphics library commits the blocks it opens GPU
// heaps over (gpu_memory.cpp). Commit charge follows use instead of whole
// sections.
int32_t create_section(uint64_t bytes) {
  Section section;
  section.committed = false;
  section.handle = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_RESERVE,
                                      static_cast<DWORD>(bytes >> 32), static_cast<DWORD>(bytes), nullptr);
  if (!section.handle) {
    LIB_LOG(holy::lib::kError, "xmem", "cannot create 0x%llx bytes of physical storage (error %lu)",
            static_cast<unsigned long long>(bytes), GetLastError());
    return -1;
  }
  section.owner.assign(static_cast<size_t>(bytes / kPage), 0);
  section.used.assign(static_cast<size_t>(bytes / kPage), false);
  (section.committed ? c_section_committed() : c_section_reserved()).add(static_cast<int64_t>(bytes));
  sections().push_back(std::move(section));
  return static_cast<int32_t>(sections().size() - 1);
}

// Clears a slot that held another page, through a view of the section's own
// (never title memory, so it is safe with g_lock held): fresh physical pages
// read as zero, as the system's pages do. Observed: titles' streaming heaps
// keep data in physical pages and depend on that.
void clear_slot(Section& section, uint32_t slot) {
  if (!section.used[slot]) {
    section.used[slot] = true;
    return;
  }
  const uint64_t offset = static_cast<uint64_t>(slot) * kPage;
  void* view = MapViewOfFile(section.handle, FILE_MAP_WRITE, static_cast<DWORD>(offset >> 32),
                             static_cast<DWORD>(offset), kPage);
  if (!view) {
    LIB_LOG(holy::lib::kError, "xmem", "cannot clear recycled physical storage (error %lu)", GetLastError());
    return;
  }
  if (section.committed || VirtualAlloc(view, kPage, MEM_COMMIT, PAGE_READWRITE)) std::memset(view, 0, kPage);
  UnmapViewOfFile(view);
}

// Gives a page its storage, preferably the slot matching `granule` in the
// region's layout. Called with g_lock held.
bool materialize(ULONG_PTR id, uintptr_t granule) {
  Page& page = pages()[id - 1];
  if (page.section >= 0) return true;
  if (Region* region = find_region(granule)) {
    const uint64_t offset = granule - region->begin;
    const size_t chunk = static_cast<size_t>(offset / kChunk);
    if (region->chunks[chunk] < 0) {
      const uint64_t chunk_bytes = std::min<uint64_t>(kChunk, region->end - region->begin - chunk * kChunk);
      region->chunks[chunk] = create_section((chunk_bytes + kPage - 1) & ~static_cast<uint64_t>(kPage - 1));
    }
    if (region->chunks[chunk] >= 0) {
      Section& section = sections()[region->chunks[chunk]];
      const auto slot = static_cast<uint32_t>((offset % kChunk) / kPage);
      if (slot < section.owner.size() && section.owner[slot] == 0) {
        clear_slot(section, slot);
        section.owner[slot] = static_cast<uint32_t>(id);
        page.section = region->chunks[chunk];
        page.index = slot;
        page.committed = section.committed;
        c_pages_stored().add(1);
        return true;
      }
    }
  }
  // The matching slot is taken (or the address is in no known region): the
  // page gets storage of its own.
  const int32_t own = create_section(kPage);
  if (own < 0) return false;
  sections()[own].owner[0] = static_cast<uint32_t>(id);
  sections()[own].used[0] = true;
  page.section = own;
  page.index = 0;
  page.committed = sections()[own].committed;
  c_pages_stored().add(1);
  return true;
}

// Turns the reservation holding `address` into a placeholder reservation of
// the same extent. Called with g_lock held.
bool make_placeholder_region(uintptr_t address) {
  if (find_region(address)) return true;
  MEMORY_BASIC_INFORMATION info;
  if (!VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info)) || info.State == MEM_FREE) return false;
  const auto base = reinterpret_cast<uintptr_t>(info.AllocationBase);
  uintptr_t end = base;
  for (;;) {
    if (!VirtualQuery(reinterpret_cast<void*>(end), &info, sizeof(info))) break;
    if (reinterpret_cast<uintptr_t>(info.AllocationBase) != base || info.State == MEM_FREE) break;
    if (info.State != MEM_RESERVE) {
      LIB_LOG(holy::lib::kError, "xmem", "physical pages mapped into a reservation that has committed memory");
      return false;
    }
    end = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
  }
  const auto virtual_alloc2 = kernelbase<VirtualAlloc2Fn>("VirtualAlloc2");
  if (!virtual_alloc2 || !VirtualFree(reinterpret_cast<void*>(base), 0, MEM_RELEASE)) return false;
  void* again = virtual_alloc2(GetCurrentProcess(), reinterpret_cast<void*>(base), end - base,
                               MEM_RESERVE | kMemReservePlaceholder, PAGE_NOACCESS, nullptr, 0);
  if (again != reinterpret_cast<void*>(base)) {
    LIB_LOG(holy::lib::kError, "xmem", "cannot turn a reservation into a placeholder (error %lu)", GetLastError());
    return false;
  }
  add_region(base, end);
  LIB_LOG(holy::lib::kDebug, "xmem", "reservation %p + 0x%llx now takes physical page mappings",
          reinterpret_cast<void*>(base), static_cast<unsigned long long>(end - base));
  return true;
}

// A private reservation that replaced part of a placeholder (titles reserve
// sub-ranges that way before mapping pages into them) becomes a placeholder
// again, provided nothing in it is committed.
void restore_placeholder(uintptr_t granule) {
  MEMORY_BASIC_INFORMATION info;
  if (!VirtualQuery(reinterpret_cast<void*>(granule), &info, sizeof(info))) return;
  if (info.State != MEM_RESERVE || info.Type != MEM_PRIVATE) return;
  const auto base = reinterpret_cast<uintptr_t>(info.AllocationBase);
  uintptr_t end = base;
  for (;;) {
    MEMORY_BASIC_INFORMATION part;
    if (!VirtualQuery(reinterpret_cast<void*>(end), &part, sizeof(part))) break;
    if (reinterpret_cast<uintptr_t>(part.AllocationBase) != base || part.State == MEM_FREE) break;
    if (part.State == MEM_COMMIT) return;
    end = reinterpret_cast<uintptr_t>(part.BaseAddress) + part.RegionSize;
  }
  if (!VirtualFree(reinterpret_cast<void*>(base), end - base, MEM_RELEASE | kMemPreservePlaceholder)) {
    LIB_LOG(holy::lib::kDebug, "xmem", "restoring placeholder %p+0x%llx: error %lu", reinterpret_cast<void*>(base),
            static_cast<unsigned long long>(end - base), GetLastError());
  }
}

// Makes [granule, granule + 64 KiB) a placeholder of its own. (The region
// size a query reports counts from the queried address, so a granule at the
// end of a larger placeholder is recognised by the allocation base.)
void split_placeholder(uintptr_t granule) {
  MEMORY_BASIC_INFORMATION info;
  if (!VirtualQuery(reinterpret_cast<void*>(granule), &info, sizeof(info))) return;
  if (info.State != MEM_RESERVE) return;
  if (reinterpret_cast<uintptr_t>(info.AllocationBase) == granule && info.RegionSize <= kPage) return;
  VirtualFree(reinterpret_cast<void*>(granule), kPage, MEM_RELEASE | kMemPreservePlaceholder);
}

// A freed page keeps its storage while any mapping of it remains: a new page
// given the same storage would show through the old mapping. Called with
// g_lock held.
void release_if_unused(Page& page) {
  if (page.live || page.maps != 0 || page.section < 0) return;
  sections()[page.section].owner[page.index] = 0;
  page.section = -1;
  c_pages_stored().add(-1);
}

void unmap_granule(uintptr_t granule) {
  auto found = mapped().find(granule);
  if (found == mapped().end()) return;
  if (found->second && found->second <= pages().size()) {
    Page& page = pages()[found->second - 1];
    if (page.maps) --page.maps;
    release_if_unused(page);
  }
  if (pending().erase(granule)) {
    mapped().erase(found);
    return;
  }
  c_granules_backed().add(-1);
  const auto unmap = kernelbase<UnmapViewOfFile2Fn>("UnmapViewOfFile2");
  if (unmap) unmap(GetCurrentProcess(), reinterpret_cast<void*>(granule), kMemPreservePlaceholder);
  mapped().erase(found);
  if (g_observer) g_observer(g_observer_context, granule, nullptr, 0);
}

void* map_view(const Page& page, uintptr_t granule) {
  const auto map = kernelbase<MapViewOfFile3Fn>("MapViewOfFile3");
  if (!map) return nullptr;
  return map(sections()[page.section].handle, GetCurrentProcess(), reinterpret_cast<void*>(granule),
             static_cast<ULONG64>(page.index) * kPage, kPage, kMemReplacePlaceholder, PAGE_READWRITE, nullptr, 0);
}

bool back_granule(uintptr_t granule, ULONG_PTR id);

bool map_granule(uintptr_t granule, ULONG_PTR id) {
  unmap_granule(granule);
  if (id == 0) return true;
  if (id > pages().size() || !pages()[id - 1].live) return false;
  ++pages()[id - 1].maps;
  if (pages()[id - 1].section < 0) {
    // No storage yet: backed on first use.
    mapped()[granule] = id;
    pending().insert(granule);
    return true;
  }
  return back_granule(granule, id);
}

// Gives a mapped granule its page's storage and a view of it. Called with
// g_lock held.
bool back_granule(uintptr_t granule, ULONG_PTR id) {
  pending().erase(granule);
  if (!materialize(id, granule)) return false;
  Page& page = pages()[id - 1];
  split_placeholder(granule);
  void* view = map_view(page, granule);
  if (!view && GetLastError() == ERROR_INVALID_ADDRESS) {
    restore_placeholder(granule);
    split_placeholder(granule);
    view = map_view(page, granule);
  }
  if (!view) {
    const DWORD error = GetLastError();
    MEMORY_BASIC_INFORMATION info{};
    VirtualQuery(reinterpret_cast<void*>(granule), &info, sizeof(info));
    LIB_LOG(holy::lib::kError, "xmem",
            "cannot map a physical page at %p (error %lu; region %p+0x%llx state 0x%lx type 0x%lx alloc base %p)",
            reinterpret_cast<void*>(granule), error, info.BaseAddress,
            static_cast<unsigned long long>(info.RegionSize), info.State, info.Type, info.AllocationBase);
    return false;
  }
  if (!page.committed) {
    // Committing through any view commits the slice of the section, which
    // every other view of it then shares.
    if (VirtualAlloc(view, kPage, MEM_COMMIT, PAGE_READWRITE)) {
      page.committed = true;
      c_page_commit().add(static_cast<int64_t>(kPage));
    } else if (commit_exhausted(GetLastError())) {
      // Said once, plainly: what fails later (the GPU's heaps over this
      // storage, titles' own checks) does not point here.
      static std::atomic<bool> warned{false};
      if (!warned.exchange(true)) {
        PERFORMANCE_INFORMATION system{};
        system.cb = sizeof(system);
        GetPerformanceInfo(&system, sizeof(system));
        LIB_LOG(holy::lib::kError, "xmem",
                "the host's commit is exhausted (%.1f of %.1f GiB): physical pages are committed on first touch, "
                "and the GPU cannot use them; close programs or restart the host",
                static_cast<double>(system.CommitTotal) * system.PageSize / (1ull << 30),
                static_cast<double>(system.CommitLimit) * system.PageSize / (1ull << 30));
      }
      HolyCommitOnDemand(view, kPage, PAGE_READWRITE);
    } else {
      return false;
    }
  }
  mapped()[granule] = id;
  c_granules_backed().add(1);
  if (g_observer) {
    g_observer(g_observer_context, granule, sections()[page.section].handle, static_cast<uint64_t>(page.index) * kPage);
  }
  return true;
}

// Backs every pending granule in [begin, end). Called with g_lock held.
bool back_range(uintptr_t begin, uintptr_t end) {
  if (pending().empty()) return true;
  bool ok = true;
  for (uintptr_t granule = begin & ~(kPage - 1); granule < end; granule += kPage) {
    if (!pending().count(granule)) continue;
    if (!back_granule(granule, mapped()[granule])) ok = false;
  }
  return ok;
}

LONG CALLBACK back_on_fault(EXCEPTION_POINTERS* info) {
  const EXCEPTION_RECORD* record = info->ExceptionRecord;
  if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || record->NumberParameters < 2) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  const uintptr_t granule = static_cast<uintptr_t>(record->ExceptionInformation[1]) & ~(kPage - 1);
  if (g_lock_owner.load() == GetCurrentThreadId()) return EXCEPTION_CONTINUE_SEARCH;
  lock();
  const auto found = pending().count(granule) ? mapped().find(granule) : mapped().end();
  const bool backed = found != mapped().end() && back_granule(granule, found->second);
  unlock();
  return backed ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
}

int back_buffer(const void* address, size_t size) {
  if (!address || size == 0) return 1;
  const auto begin = reinterpret_cast<uintptr_t>(address);
  lock();
  const bool ok = back_range(begin, begin + size);
  unlock();
  return ok ? 1 : 0;
}

void install_lazy_backing() {
  static const bool installed = [] {
    AddVectoredExceptionHandler(1, &back_on_fault);
    HolySetBackingHook(&back_buffer);
    return true;
  }();
  (void)installed;
}

}  // namespace

void* reserve_placeholder(void* address, SIZE_T size) {
  const auto virtual_alloc2 = kernelbase<VirtualAlloc2Fn>("VirtualAlloc2");
  if (!virtual_alloc2) return nullptr;
  void* base = virtual_alloc2(GetCurrentProcess(), address, size, MEM_RESERVE | kMemReservePlaceholder,
                              PAGE_NOACCESS, nullptr, 0);
  if (!base) return nullptr;
  lock();
  add_region(reinterpret_cast<uintptr_t>(base), reinterpret_cast<uintptr_t>(base) + size);
  unlock();
  return base;
}

// Replaces [address, address + size) of a placeholder with private memory.
// The placeholder is split to exactly that range first if it is larger.
void* replace_placeholder(void* address, SIZE_T size, DWORD type, DWORD protect) {
  const auto virtual_alloc2 = kernelbase<VirtualAlloc2Fn>("VirtualAlloc2");
  if (!virtual_alloc2 || !address) return nullptr;
  MEMORY_BASIC_INFORMATION info;
  if (VirtualQuery(address, &info, sizeof(info)) && info.State == MEM_RESERVE &&
      (info.BaseAddress != address || info.RegionSize != size)) {
    VirtualFree(address, size, MEM_RELEASE | kMemPreservePlaceholder);
  }
  const ULONG flags = (type & ~static_cast<DWORD>(kMemReplacePlaceholder)) | MEM_RESERVE | kMemReplacePlaceholder;
  void* result = virtual_alloc2(GetCurrentProcess(), address, size, flags, protect, nullptr, 0);
  if (!result && (type & MEM_COMMIT) && commit_exhausted(GetLastError())) {
    result = virtual_alloc2(GetCurrentProcess(), address, size, (flags & ~static_cast<DWORD>(MEM_COMMIT)),
                            PAGE_NOACCESS, nullptr, 0);
    if (result) HolyCommitOnDemand(result, size, protect);
  }
  return result;
}

// Observed: titles keep page numbers packed as `number >> 4` in 21-bit fields
// and rebuild them as `value << 4`: page numbers are frame numbers in 4 KiB
// units, so every 64 KiB page's number is a multiple of 16 (as the desktop's
// user physical page frame numbers count 4 KiB frames). Internally pages are
// numbered from 1; titles see that number times 16.
namespace {

ULONG_PTR to_frame(ULONG_PTR id) {
  return id << 4;
}
ULONG_PTR to_id(ULONG_PTR frame) {
  if (frame == 0) return 0;
  if (frame & 15) return ~ULONG_PTR{0};  // not a page this library handed out
  return frame >> 4;
}
std::vector<ULONG_PTR> ids_of(const ULONG_PTR* frames, ULONG_PTR count) {
  std::vector<ULONG_PTR> ids(count, 0);
  for (ULONG_PTR i = 0; frames && i < count; ++i) ids[i] = to_id(frames[i]);
  return ids;
}

}  // namespace

HOLY_EXPORT BOOL XMemAllocatePhysicalPages(uint64_t attributes, ULONG_PTR* count, ULONG_PTR* page_array) {
  if (!count || !page_array || *count == 0) {
    SetLastError(ERROR_INVALID_PARAMETER);
    return FALSE;
  }
  const ULONG_PTR requested = *count;
  lock();
  const ULONG_PTR first = pages().size() + 1;  // ids start at 1
  pages().resize(pages().size() + requested, Page{-1, 0, true, false});
  unlock();
  c_pages_live().add(static_cast<int64_t>(requested));
  for (ULONG_PTR i = 0; i < requested; ++i) page_array[i] = to_frame(first + i);
  LIB_LOG(holy::lib::kDebug, "xmem", "allocated %llu physical pages (attributes 0x%llx)",
          static_cast<unsigned long long>(requested), static_cast<unsigned long long>(attributes));
  return TRUE;
}

// Not a console function: lets this project's graphics library find the
// section memory behind a title address. Returns the section, the offset of
// `address` in it and how many bytes from `address` on are mapped
// contiguously from that section.
HOLY_EXPORT BOOL HolyXMemQueryPhysical(const void* address, HANDLE* section, uint64_t* offset,
                                       uint64_t* contiguous) {
  const auto at = reinterpret_cast<uintptr_t>(address);
  const uintptr_t granule = at & ~(kPage - 1);
  lock();
  // Back the granule and every pending one mapped contiguously after it.
  uintptr_t last = granule;
  while (mapped().count(last + kPage)) last += kPage;
  back_range(granule, last + kPage);
  BOOL found = FALSE;
  auto it = mapped().find(granule);
  if (it != mapped().end()) {
    const Page& first = pages()[it->second - 1];
    const Section& storage = sections()[first.section];
    *section = storage.handle;
    *offset = static_cast<uint64_t>(first.index) * kPage + (at - granule);
    uint64_t run = kPage - (at - granule);
    for (uintptr_t next = granule + kPage;; next += kPage) {
      auto following = mapped().find(next);
      if (following == mapped().end()) break;
      const Page& page = pages()[following->second - 1];
      if (page.section != first.section || page.index != first.index + (next - granule) / kPage) break;
      run += kPage;
    }
    *contiguous = run;
    found = TRUE;
  }
  unlock();
  return found;
}

// Not console functions: let this project's graphics library keep its view of
// which title addresses show which section storage current. The observer is
// called, with this library's lock held (it must not call back into this
// library), when a granule gets its page's storage (`section` and the page's
// `offset` in it) and when a granule is unmapped (`section` null).
HOLY_EXPORT void HolyXMemObserveMappings(MappingObserver observer, void* context) {
  lock();
  g_observer = observer;
  g_observer_context = context;
  unlock();
}

// Calls `visit` (without the lock) for every granule showing storage of
// `section` in [begin, end), with the storage's offset.
HOLY_EXPORT void HolyXMemEnumerateMapped(HANDLE section, uint64_t begin, uint64_t end,
                                         void (*visit)(void* context, uintptr_t granule, uint64_t offset),
                                         void* context) {
  std::vector<std::pair<uintptr_t, uint64_t>> found;
  lock();
  for (const auto& [granule, id] : mapped()) {
    if (pending().count(granule) || id == 0 || id > pages().size()) continue;
    const Page& page = pages()[id - 1];
    if (page.section < 0 || sections()[page.section].handle != section) continue;
    const uint64_t offset = static_cast<uint64_t>(page.index) * kPage;
    if (offset >= begin && offset < end) found.emplace_back(granule, offset);
  }
  unlock();
  for (const auto& [granule, offset] : found) visit(context, granule, offset);
}

// For the graphics library: the page number (as XMemAllocatePhysicalPages
// handed it out) of the physical page mapped at `address`, 0 if none is.
HOLY_EXPORT ULONG_PTR HolyXMemPhysicalPageAt(const void* address) {
  const uintptr_t granule = reinterpret_cast<uintptr_t>(address) & ~(kPage - 1);
  lock();
  auto it = mapped().find(granule);
  const ULONG_PTR frame = it == mapped().end() ? 0 : to_frame(it->second);
  unlock();
  return frame;
}

HOLY_EXPORT BOOL XMemFreePhysicalPages(ULONG_PTR count, ULONG_PTR* page_array) {
  if (!page_array) return FALSE;
  const std::vector<ULONG_PTR> ids = ids_of(page_array, count);
  lock();
  for (ULONG_PTR i = 0; i < count; ++i) {
    const ULONG_PTR id = ids[i];
    if (id == 0 || id > pages().size() || !pages()[id - 1].live) continue;
    Page& page = pages()[id - 1];
    page.live = false;
    c_pages_live().add(-1);
    // The storage becomes free for a fresh page once no mapping shows it.
    release_if_unused(page);
  }
  unlock();
  return TRUE;
}

HOLY_EXPORT void* XMemMapPhysicalPages(void* address, ULONG_PTR count, ULONG_PTR* page_array) {
  if (!address || count == 0 || (reinterpret_cast<uintptr_t>(address) & (kPage - 1))) return nullptr;
  install_lazy_backing();
  const auto base = reinterpret_cast<uintptr_t>(address);
  const std::vector<ULONG_PTR> ids = ids_of(page_array, count);
  if (HolyLogEnabled(holy::lib::kTrace)) {
    std::unordered_set<ULONG_PTR> distinct(ids.begin(), ids.end());
    LIB_LOG(holy::lib::kTrace, "xmem", "map %llu pages at %p (%llu distinct)", static_cast<unsigned long long>(count),
            address, static_cast<unsigned long long>(distinct.size()));
  }
  bool ok = true;
  lock();
  if (!make_placeholder_region(base)) {
    ok = false;
  } else {
    for (ULONG_PTR i = 0; i < count && ok; ++i) ok = map_granule(base + i * kPage, ids[i]);
  }
  unlock();
  return ok ? address : nullptr;
}

HOLY_EXPORT BOOL XMemMapPhysicalPagesScatter(void** addresses, ULONG_PTR count, ULONG_PTR* page_array) {
  if (!addresses) return FALSE;
  install_lazy_backing();
  const std::vector<void*> places(addresses, addresses + count);
  const std::vector<ULONG_PTR> ids = ids_of(page_array, count);
  if (HolyLogEnabled(holy::lib::kTrace)) {
    std::unordered_set<ULONG_PTR> distinct(ids.begin(), ids.end());
    LIB_LOG(holy::lib::kTrace, "xmem", "scatter-map %llu pages (%llu distinct), first at %p",
            static_cast<unsigned long long>(count), static_cast<unsigned long long>(distinct.size()),
            places.empty() ? nullptr : places[0]);
  }
  bool ok = true;
  lock();
  for (ULONG_PTR i = 0; i < count && ok; ++i) {
    const auto granule = reinterpret_cast<uintptr_t>(places[i]);
    if (!granule || (granule & (kPage - 1)) || !make_placeholder_region(granule)) {
      ok = false;
      break;
    }
    ok = map_granule(granule, ids[i]);
  }
  unlock();
  return ok ? TRUE : FALSE;
}

// On the console this moves memory budget between the title and the system.
// A host process has no such partition, so there is nothing to move.
HOLY_EXPORT HRESULT XMemTransferMemory(uint32_t kind, SIZE_T bytes) {
  LIB_LOG(holy::lib::kDebug, "xmem", "memory transfer %u of 0x%llx bytes (no partition on the host)", kind,
          static_cast<unsigned long long>(bytes));
  return S_OK;
}
