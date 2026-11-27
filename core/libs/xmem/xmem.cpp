// xmem.dll: title memory allocation.
//
// Titles allocate through XMemAlloc(size, attributes) and release through
// XMemFree(address, attributes). `attributes` is a 64-bit word of bit fields.
// The layout assumed here follows public documentation of the previous console
// generation and is checked against observed calls during bring-up (the first
// allocations are logged at debug level):
//
//   bits  0..13  object type      (ignored)
//   bits 14..15  page size        0 = 4 KiB, non-zero = large pages
//   bits 16..23  allocator id     (ignored)
//   bits 24..28  log2(alignment)  0 = default
//   bits 29..32  memory type      0 = ordinary cacheable heap memory
//
// Mapping to the host: small ordinary requests come from an aligned heap,
// everything else (large, strongly aligned, large-page or non-heap memory
// types) gets its own zeroed page allocation. A registry of page allocations
// lets XMemFree tell the two apart without trusting the caller's attributes.
#include <windows.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

#include "core/libs/support.h"

// physical.cpp
void* reserve_placeholder(void* address, SIZE_T size);
void* replace_placeholder(void* address, SIZE_T size, DWORD type, DWORD protect);

namespace {

constexpr char kLibrary[] = "xmem.dll";
constexpr SIZE_T kDefaultAlignment = 16;
constexpr SIZE_T kPageAllocationThreshold = 1024 * 1024;
constexpr SIZE_T kAllocationGranularity = 0x10000;

// Commit charge on the host can run out long before physical memory does:
// a console title commits its pools up front, the host charges every
// committed page against RAM plus paging file. Large commits (kDeferredCommit
// and more), and any commit refused for that reason, are therefore reserved
// and committed on first touch (see core/runtime/memory.h): commit charge
// follows use. Fresh pages read as zero either way.
constexpr SIZE_T kDeferredCommit = 64ull << 20;
bool commit_exhausted(DWORD error) {
  return error == ERROR_COMMITMENT_LIMIT || error == ERROR_NOT_ENOUGH_MEMORY || error == ERROR_OUTOFMEMORY ||
         error == ERROR_COMMITMENT_MINIMUM;
}

std::atomic<bool> g_deferral_reported{false};

void defer_commit(void* base, SIZE_T size, DWORD protect) {
  const auto begin = reinterpret_cast<uintptr_t>(base) & ~static_cast<uintptr_t>(0xFFF);
  const uintptr_t end = (reinterpret_cast<uintptr_t>(base) + size + 0xFFF) & ~static_cast<uintptr_t>(0xFFF);
  HolyCommitOnDemand(reinterpret_cast<void*>(begin), end - begin, protect);
  if (!g_deferral_reported.exchange(true)) {
    LIB_LOG(holy::lib::kInfo, "xmem", "large title allocations are committed on first touch");
  }
}

struct Attributes {
  uint32_t page_size;
  uint32_t alignment_log2;
  uint32_t memory_type;
};

Attributes decode(uint64_t attributes) {
  Attributes out;
  out.page_size = static_cast<uint32_t>((attributes >> 14) & 0x3);
  out.alignment_log2 = static_cast<uint32_t>((attributes >> 24) & 0x1F);
  out.memory_type = static_cast<uint32_t>((attributes >> 29) & 0xF);
  return out;
}

SRWLOCK g_lock = SRWLOCK_INIT;
std::unordered_map<void*, SIZE_T>& page_allocations() {
  static auto* map = new std::unordered_map<void*, SIZE_T>();
  return *map;
}
std::atomic<int> g_log_budget{32};

void* allocate_pages(SIZE_T size, SIZE_T alignment) {
  // Whole 64 KiB units (the host's allocation granularity, which the address
  // takes anyway): the GPU reaches title memory through buffers over heaps
  // opened over it, and a buffer takes whole 64 KiB units of its heap, so a
  // smaller allocation could not be reached at all. The title sees its size.
  size = (size + kAllocationGranularity - 1) & ~(kAllocationGranularity - 1);
  void* block = nullptr;
  const bool defer = size >= kDeferredCommit;
  if (alignment <= kAllocationGranularity) {
    block = defer ? nullptr : VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!block && (defer || commit_exhausted(GetLastError()))) {
      block = VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_NOACCESS);
      if (block) defer_commit(block, size, PAGE_READWRITE);
    }
  } else {
    // Over-reserve to find an aligned address, then claim exactly that range.
    for (int attempt = 0; attempt < 8 && !block; ++attempt) {
      void* probe = VirtualAlloc(nullptr, size + alignment, MEM_RESERVE, PAGE_NOACCESS);
      if (!probe) break;
      const auto aligned =
          (reinterpret_cast<uintptr_t>(probe) + alignment - 1) & ~(static_cast<uintptr_t>(alignment) - 1);
      VirtualFree(probe, 0, MEM_RELEASE);
      block = defer ? nullptr
                    : VirtualAlloc(reinterpret_cast<void*>(aligned), size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
      if (!block && (defer || commit_exhausted(GetLastError()))) {
        block = VirtualAlloc(reinterpret_cast<void*>(aligned), size, MEM_RESERVE, PAGE_NOACCESS);
        if (block) defer_commit(block, size, PAGE_READWRITE);
      }
    }
  }
  if (block) {
    AcquireSRWLockExclusive(&g_lock);
    page_allocations()[block] = size;
    ReleaseSRWLockExclusive(&g_lock);
  }
  return block;
}

bool free_pages(void* address) {
  AcquireSRWLockExclusive(&g_lock);
  auto found = page_allocations().find(address);
  const bool known = found != page_allocations().end();
  const SIZE_T size = known ? found->second : 0;
  if (known) page_allocations().erase(found);
  ReleaseSRWLockExclusive(&g_lock);
  if (known) {
    HolyBeforeRelease(address, size);
    VirtualFree(address, 0, MEM_RELEASE);
    HolyCommitOnDemandReleased(address, size);
  }
  return known;
}

void* allocate(SIZE_T size, uint64_t raw_attributes) {
  const Attributes attributes = decode(raw_attributes);
  SIZE_T alignment =
      attributes.alignment_log2 ? (static_cast<SIZE_T>(1) << attributes.alignment_log2) : kDefaultAlignment;
  if (alignment < kDefaultAlignment) alignment = kDefaultAlignment;
  if (size == 0) size = 1;

  const bool use_pages = size >= kPageAllocationThreshold || alignment > 4096 || attributes.page_size != 0 ||
                         attributes.memory_type != 0;
  void* block = nullptr;
  if (use_pages) {
    block = allocate_pages(size, alignment);
  } else {
    block = _aligned_malloc(size, alignment);
    if (block) std::memset(block, 0, size);
  }

  if (g_log_budget.load(std::memory_order_relaxed) > 0 && g_log_budget.fetch_sub(1) > 0) {
    LIB_LOG(holy::lib::kDebug, "xmem", "alloc size=0x%llx attributes=0x%016llx -> %p (%s, align 0x%llx)",
            static_cast<unsigned long long>(size), static_cast<unsigned long long>(raw_attributes), block,
            use_pages ? "pages" : "heap", static_cast<unsigned long long>(alignment));
  }
  if (!block) {
    LIB_LOG(holy::lib::kError, "xmem", "allocation of 0x%llx bytes failed (attributes 0x%016llx)",
            static_cast<unsigned long long>(size), static_cast<unsigned long long>(raw_attributes));
  }
  return block;
}

void release(void* address) {
  if (!address) return;
  if (!free_pages(address)) _aligned_free(address);
}

}  // namespace

// Allocation hooks (observed): a title may export __XMemAllocationHooks__, a
// table whose first two entries are an allocation function with XMemAlloc's
// signature and a release function with XMemFree's. When it does, every
// XMemAlloc and XMemFree goes to those hooks, which chain to
// XMemAllocDefault and XMemFreeDefault for what they do not handle
// themselves. Titles use them to place system allocations (descriptor heap
// memory, object type 4) at addresses of their choosing.
namespace {

struct AllocationHooks {
  void* (*alloc)(SIZE_T, uint64_t);
  void (*free)(void*, uint64_t);
};

const AllocationHooks* title_hooks() {
  static const AllocationHooks* hooks = [] {
    const auto* table = reinterpret_cast<const AllocationHooks*>(
        GetProcAddress(GetModuleHandleW(nullptr), "__XMemAllocationHooks__"));
    if (table && table->alloc && table->free) {
      LIB_LOG(holy::lib::kInfo, "xmem", "the title's allocation hooks are in use");
      return table;
    }
    return static_cast<const AllocationHooks*>(nullptr);
  }();
  return hooks;
}

}  // namespace

HOLY_EXPORT void* XMemAlloc(SIZE_T size, uint64_t attributes) {
  if (const AllocationHooks* hooks = title_hooks()) return hooks->alloc(size, attributes);
  return allocate(size, attributes);
}
HOLY_EXPORT void XMemFree(void* address, uint64_t attributes) {
  if (const AllocationHooks* hooks = title_hooks()) {
    hooks->free(address, attributes);
    return;
  }
  release(address);
}

// XMemVirtualAlloc(address, size, allocation_type, allocator_id, protect)
//
// VirtualAlloc with an allocator id for the console's memory accounting.
// Observed and documented differences from the desktop call:
//   - allocation_type may select the console's page sizes with the bits the
//     public winnt.h names MEM_64K_PAGES (MEM_LARGE_PAGES | MEM_PHYSICAL) and
//     MEM_4MB_PAGES. The host backs everything with its normal pages; only the
//     alignment those sizes imply is preserved.
//   - protect may carry PAGE_GRAPHICS_* bits describing GPU access, which have
//     no meaning to the host's virtual memory manager.
namespace {

constexpr DWORD kMemLargePages = 0x20000000;  // MEM_LARGE_PAGES
constexpr DWORD kMemPhysical = 0x00400000;    // MEM_PHYSICAL
constexpr DWORD kMem4MbPages = 0x80000000;    // MEM_4MB_PAGES
constexpr DWORD kConsolePageFlags = kMemLargePages | kMemPhysical | kMem4MbPages;
constexpr DWORD kGraphicsProtectMask = 0x0007F800;  // PAGE_GRAPHICS_NOACCESS .. PAGE_GRAPHICS_NOCACHE
constexpr SIZE_T k4Mb = 4 * 1024 * 1024;

std::atomic<int> g_virtual_log_budget{64};

void* reserve_aligned(SIZE_T size, SIZE_T alignment, DWORD type, DWORD protect) {
  for (int attempt = 0; attempt < 8; ++attempt) {
    void* probe = VirtualAlloc(nullptr, size + alignment, MEM_RESERVE, PAGE_NOACCESS);
    if (!probe) return nullptr;
    const auto aligned =
        (reinterpret_cast<uintptr_t>(probe) + alignment - 1) & ~(static_cast<uintptr_t>(alignment) - 1);
    VirtualFree(probe, 0, MEM_RELEASE);
    if (void* block = VirtualAlloc(reinterpret_cast<void*>(aligned), size, type, protect)) return block;
  }
  return nullptr;
}

}  // namespace

HOLY_EXPORT void* XMemVirtualAlloc(void* address, SIZE_T size, DWORD allocation_type, uint64_t allocator_id,
                                   DWORD protect) {
  const DWORD host_type = allocation_type & ~kConsolePageFlags;
  DWORD host_protect = protect & ~kGraphicsProtectMask;
  if ((host_protect & 0xFF) == 0) host_protect |= PAGE_NOACCESS;

  // Placeholder reservations (documented MEM_RESERVE_PLACEHOLDER) need the
  // host's VirtualAlloc2; the physical-page mapper takes them over.
  constexpr DWORD kMemReservePlaceholder = 0x00040000;
  if (allocation_type & kMemReservePlaceholder) {
    void* placeholder = reserve_placeholder(address, size);
    LIB_LOG(placeholder ? holy::lib::kDebug : holy::lib::kError, "xmem",
            "placeholder reservation size=0x%llx allocator=0x%llx -> %p", static_cast<unsigned long long>(size),
            static_cast<unsigned long long>(allocator_id), placeholder);
    return placeholder;
  }

  constexpr DWORD kMemReplacePlaceholder = 0x00004000;
  if (allocation_type & kMemReplacePlaceholder) {
    void* replaced = replace_placeholder(address, size, host_type, host_protect);
    if (!replaced) {
      LIB_LOG(holy::lib::kError, "xmem", "placeholder replacement at %p size=0x%llx failed (error %lu)", address,
              static_cast<unsigned long long>(size), GetLastError());
    }
    return replaced;
  }

  void* result = nullptr;
  const bool aligned_reservation = !address && (allocation_type & MEM_RESERVE) && (allocation_type & kMem4MbPages);
  const bool defer = (host_type & MEM_COMMIT) && size >= kDeferredCommit;
  if (defer) {
    SetLastError(ERROR_COMMITMENT_LIMIT);  // take the deferred path below
  } else if (aligned_reservation) {
    result = reserve_aligned(size, k4Mb, host_type, host_protect);
  } else {
    result = VirtualAlloc(address, size, host_type, host_protect);
  }
  if (!result && (host_type & MEM_COMMIT) && (defer || commit_exhausted(GetLastError()))) {
    // Keep the reservation semantics of the request; defer only the commit.
    if (host_type & MEM_RESERVE) {
      const DWORD reserve_type = host_type & ~static_cast<DWORD>(MEM_COMMIT);
      result = aligned_reservation ? reserve_aligned(size, k4Mb, reserve_type, PAGE_NOACCESS)
                                   : VirtualAlloc(address, size, reserve_type, PAGE_NOACCESS);
    } else {
      result = address;  // committing inside an existing reservation
    }
    if (result) defer_commit(result, size, host_protect);
  }
  const DWORD error = GetLastError();

  if (!result ||
      (g_virtual_log_budget.load(std::memory_order_relaxed) > 0 && g_virtual_log_budget.fetch_sub(1) > 0)) {
    LIB_LOG(result ? holy::lib::kDebug : holy::lib::kError, "xmem",
            "virtual alloc address=%p size=0x%llx type=0x%08lx allocator=0x%llx protect=0x%lx -> %p%s",
            address, static_cast<unsigned long long>(size), allocation_type,
            static_cast<unsigned long long>(allocator_id), protect, result, result ? "" : " (failed)");
  }
  SetLastError(error);
  return result;
}

// The "default" pair is what a title's own allocation hooks chain to.
HOLY_EXPORT void* XMemAllocDefault(SIZE_T size, uint64_t attributes) {
  return allocate(size, attributes);
}
HOLY_EXPORT void XMemFreeDefault(void* address, uint64_t) {
  release(address);
}

// Imported by libraries that titles ship; contracts not established yet.
HOLY_UNIMPLEMENTED_EXPORT(kLibrary, XMemVirtualQuery)
HOLY_UNIMPLEMENTED_EXPORT(kLibrary, XMemGetWorkingSetStatistics)
