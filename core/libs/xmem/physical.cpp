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
// Host model: every allocation is a pagefile-backed section reserved without
// commit; a "physical page" is a 64 KiB slice of it, named by an opaque id.
// Mapping replaces a 64 KiB placeholder of the target reservation with a view
// of that slice, so a page mapped at several addresses is truly shared, as the
// CPU and GPU views titles build expect. A reservation becomes a placeholder
// reservation the first time pages are mapped into it (it must not have
// committed memory at that point). Slices are committed when first mapped;
// if the host is out of commit charge, on first touch instead.
#include <windows.h>

#include <unordered_map>
#include <vector>

#include "core/libs/support.h"

namespace {

constexpr ULONG_PTR kPage = 0x10000;

struct Section {
  HANDLE handle = nullptr;
  ULONG_PTR live_pages = 0;
};

struct Page {
  uint32_t section = 0;
  uint32_t index = 0;
  bool live = false;
  bool committed = false;
};

SRWLOCK g_lock = SRWLOCK_INIT;
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
std::vector<std::pair<uintptr_t, uintptr_t>>& placeholder_regions() {
  static auto* storage = new std::vector<std::pair<uintptr_t, uintptr_t>>();
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

bool in_placeholder_region(uintptr_t address) {
  for (const auto& [begin, end] : placeholder_regions()) {
    if (address >= begin && address < end) return true;
  }
  return false;
}

// Turns the reservation holding `address` into a placeholder reservation of
// the same extent. Called with g_lock held.
bool make_placeholder_region(uintptr_t address) {
  if (in_placeholder_region(address)) return true;
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
  placeholder_regions().emplace_back(base, end);
  LIB_LOG(holy::lib::kDebug, "xmem", "reservation %p + 0x%llx now takes physical page mappings",
          reinterpret_cast<void*>(base), static_cast<unsigned long long>(end - base));
  return true;
}

// Makes [granule, granule + 64 KiB) a placeholder of its own.
void split_placeholder(uintptr_t granule) {
  MEMORY_BASIC_INFORMATION info;
  if (!VirtualQuery(reinterpret_cast<void*>(granule), &info, sizeof(info))) return;
  if (info.State != MEM_RESERVE || info.RegionSize <= kPage) return;
  VirtualFree(reinterpret_cast<void*>(granule), kPage, MEM_RELEASE | kMemPreservePlaceholder);
}

void unmap_granule(uintptr_t granule) {
  auto found = mapped().find(granule);
  if (found == mapped().end()) return;
  const auto unmap = kernelbase<UnmapViewOfFile2Fn>("UnmapViewOfFile2");
  if (unmap) unmap(GetCurrentProcess(), reinterpret_cast<void*>(granule), kMemPreservePlaceholder);
  mapped().erase(found);
}

bool map_granule(uintptr_t granule, ULONG_PTR id) {
  unmap_granule(granule);
  if (id == 0) return true;
  if (id > pages().size() || !pages()[id - 1].live) return false;
  Page& page = pages()[id - 1];
  split_placeholder(granule);
  const auto map_view = kernelbase<MapViewOfFile3Fn>("MapViewOfFile3");
  if (!map_view) return false;
  void* view = map_view(sections()[page.section].handle, GetCurrentProcess(), reinterpret_cast<void*>(granule),
                        static_cast<ULONG64>(page.index) * kPage, kPage, kMemReplacePlaceholder, PAGE_READWRITE,
                        nullptr, 0);
  if (!view) {
    LIB_LOG(holy::lib::kError, "xmem", "cannot map a physical page at %p (error %lu)",
            reinterpret_cast<void*>(granule), GetLastError());
    return false;
  }
  if (!page.committed) {
    // Committing through any view commits the slice of the section, which
    // every other view of it then shares.
    if (VirtualAlloc(view, kPage, MEM_COMMIT, PAGE_READWRITE)) {
      page.committed = true;
    } else if (commit_exhausted(GetLastError())) {
      HolyCommitOnDemand(view, kPage, PAGE_READWRITE);
    } else {
      return false;
    }
  } else {
    // Another view committed the slice; this view sees it committed already.
  }
  mapped()[granule] = id;
  return true;
}

}  // namespace

HOLY_EXPORT BOOL XMemAllocatePhysicalPages(uint64_t attributes, ULONG_PTR* count, ULONG_PTR* page_array) {
  if (!count || !page_array || *count == 0) {
    SetLastError(ERROR_INVALID_PARAMETER);
    return FALSE;
  }
  const ULONG_PTR requested = *count;
  const uint64_t bytes = static_cast<uint64_t>(requested) * kPage;
  HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_RESERVE,
                                      static_cast<DWORD>(bytes >> 32), static_cast<DWORD>(bytes), nullptr);
  if (!section) {
    const DWORD error = GetLastError();
    LIB_LOG(holy::lib::kError, "xmem", "physical allocation of %llu pages failed (error %lu)",
            static_cast<unsigned long long>(requested), error);
    *count = 0;
    SetLastError(error);
    return FALSE;
  }
  AcquireSRWLockExclusive(&g_lock);
  const auto section_index = static_cast<uint32_t>(sections().size());
  sections().push_back(Section{section, requested});
  for (ULONG_PTR i = 0; i < requested; ++i) {
    pages().push_back(Page{section_index, static_cast<uint32_t>(i), true, false});
    page_array[i] = pages().size();  // ids start at 1
  }
  ReleaseSRWLockExclusive(&g_lock);
  LIB_LOG(holy::lib::kDebug, "xmem", "allocated %llu physical pages (attributes 0x%llx)",
          static_cast<unsigned long long>(requested), static_cast<unsigned long long>(attributes));
  return TRUE;
}

HOLY_EXPORT BOOL XMemFreePhysicalPages(ULONG_PTR count, ULONG_PTR* page_array) {
  if (!page_array) return FALSE;
  AcquireSRWLockExclusive(&g_lock);
  for (ULONG_PTR i = 0; i < count; ++i) {
    const ULONG_PTR id = page_array[i];
    if (id == 0 || id > pages().size() || !pages()[id - 1].live) continue;
    Page& page = pages()[id - 1];
    page.live = false;
    Section& section = sections()[page.section];
    if (--section.live_pages == 0 && section.handle) {
      // Views still mapped keep the memory alive until they are unmapped.
      CloseHandle(section.handle);
      section.handle = nullptr;
    }
  }
  ReleaseSRWLockExclusive(&g_lock);
  return TRUE;
}

HOLY_EXPORT void* XMemMapPhysicalPages(void* address, ULONG_PTR count, ULONG_PTR* page_array) {
  if (!address || count == 0 || (reinterpret_cast<uintptr_t>(address) & (kPage - 1))) return nullptr;
  const auto base = reinterpret_cast<uintptr_t>(address);
  bool ok = true;
  AcquireSRWLockExclusive(&g_lock);
  if (!make_placeholder_region(base)) {
    ok = false;
  } else {
    for (ULONG_PTR i = 0; i < count && ok; ++i) ok = map_granule(base + i * kPage, page_array ? page_array[i] : 0);
  }
  ReleaseSRWLockExclusive(&g_lock);
  return ok ? address : nullptr;
}

HOLY_EXPORT BOOL XMemMapPhysicalPagesScatter(void** addresses, ULONG_PTR count, ULONG_PTR* page_array) {
  if (!addresses) return FALSE;
  bool ok = true;
  AcquireSRWLockExclusive(&g_lock);
  for (ULONG_PTR i = 0; i < count && ok; ++i) {
    const auto granule = reinterpret_cast<uintptr_t>(addresses[i]);
    if (!granule || (granule & (kPage - 1)) || !make_placeholder_region(granule)) {
      ok = false;
      break;
    }
    ok = map_granule(granule, page_array ? page_array[i] : 0);
  }
  ReleaseSRWLockExclusive(&g_lock);
  return ok ? TRUE : FALSE;
}

// On the console this moves memory budget between the title and the system.
// A host process has no such partition, so there is nothing to move.
HOLY_EXPORT HRESULT XMemTransferMemory(uint32_t kind, SIZE_T bytes) {
  LIB_LOG(holy::lib::kDebug, "xmem", "memory transfer %u of 0x%llx bytes (no partition on the host)", kind,
          static_cast<unsigned long long>(bytes));
  return S_OK;
}
