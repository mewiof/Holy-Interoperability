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

namespace {

constexpr char kLibrary[] = "xmem.dll";
constexpr SIZE_T kDefaultAlignment = 16;
constexpr SIZE_T kPageAllocationThreshold = 1024 * 1024;
constexpr SIZE_T kAllocationGranularity = 0x10000;

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
  void* block = nullptr;
  if (alignment <= kAllocationGranularity) {
    block = VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  } else {
    // Over-reserve to find an aligned address, then claim exactly that range.
    for (int attempt = 0; attempt < 8 && !block; ++attempt) {
      void* probe = VirtualAlloc(nullptr, size + alignment, MEM_RESERVE, PAGE_NOACCESS);
      if (!probe) break;
      const auto aligned =
          (reinterpret_cast<uintptr_t>(probe) + alignment - 1) & ~(static_cast<uintptr_t>(alignment) - 1);
      VirtualFree(probe, 0, MEM_RELEASE);
      block = VirtualAlloc(reinterpret_cast<void*>(aligned), size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
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
  const bool known = page_allocations().erase(address) != 0;
  ReleaseSRWLockExclusive(&g_lock);
  if (known) VirtualFree(address, 0, MEM_RELEASE);
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

HOLY_EXPORT void* XMemAlloc(SIZE_T size, uint64_t attributes) {
  return allocate(size, attributes);
}
HOLY_EXPORT void XMemFree(void* address, uint64_t) {
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
constexpr DWORD kGraphicsProtectMask = 0x0003F800;  // PAGE_GRAPHICS_NOACCESS .. PAGE_GRAPHICS_COHERENT
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

  void* result = nullptr;
  if (!address && (allocation_type & MEM_RESERVE) && (allocation_type & kMem4MbPages)) {
    result = reserve_aligned(size, k4Mb, host_type, host_protect);
  } else {
    result = VirtualAlloc(address, size, host_type, host_protect);
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
