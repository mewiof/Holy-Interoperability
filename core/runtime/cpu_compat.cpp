#include "core/runtime/cpu_compat.h"

#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "core/common/log.h"

namespace holy::rt {
namespace {

constexpr char kComponent[] = "cpu";
constexpr size_t kCacheLine = 64;

std::atomic<uint64_t> g_emulated{0};

// Prefixes that may precede the instruction: address-size override (0x67)
// and, harmlessly, segment overrides the instruction ignores in 64-bit mode.
size_t prefix_length(const uint8_t* code, size_t available, bool* address32) {
  size_t at = 0;
  *address32 = false;
  while (at < available && at < 4) {
    const uint8_t b = code[at];
    if (b == 0x67) {
      *address32 = true;
    } else if (b != 0x2E && b != 0x3E && b != 0x26 && b != 0x36) {
      break;
    }
    ++at;
  }
  return at;
}

bool is_clzero(const uint8_t* code, size_t available, size_t* length, bool* address32) {
  const size_t at = prefix_length(code, available, address32);
  if (at + 3 > available) return false;
  if (code[at] != 0x0F || code[at + 1] != 0x01 || code[at + 2] != 0xFC) return false;
  *length = at + 3;
  return true;
}

LONG CALLBACK cpu_handler(EXCEPTION_POINTERS* info) {
  if (info->ExceptionRecord->ExceptionCode != EXCEPTION_ILLEGAL_INSTRUCTION) return EXCEPTION_CONTINUE_SEARCH;
  CONTEXT* context = info->ContextRecord;
  const auto* code = reinterpret_cast<const uint8_t*>(context->Rip);
  size_t length = 0;
  bool address32 = false;
  if (!is_clzero(code, 8, &length, &address32)) return EXCEPTION_CONTINUE_SEARCH;
  const DWORD saved_error = GetLastError();
  uint64_t address = address32 ? static_cast<uint32_t>(context->Rax) : context->Rax;
  address &= ~static_cast<uint64_t>(kCacheLine - 1);
  // An access fault here (memory committed on first touch) is handled like
  // the title's own store would have been.
  std::memset(reinterpret_cast<void*>(address), 0, kCacheLine);
  context->Rip += length;
  if (g_emulated.fetch_add(1) == 0) {
    LOG_INFO(kComponent, "the host CPU lacks CLZERO; it is carried out by the runtime (first at %p)",
             static_cast<const void*>(code));
  }
  SetLastError(saved_error);
  return EXCEPTION_CONTINUE_EXECUTION;
}

}  // namespace

void install_cpu_compatibility() {
  AddVectoredExceptionHandler(1, &cpu_handler);
}

}  // namespace holy::rt
