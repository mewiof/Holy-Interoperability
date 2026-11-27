#include "core/runtime/cpu_compat.h"

#include <windows.h>

#include <intrin.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "core/common/log.h"
#include "core/runtime/stats.h"

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

enum class Op { None, Clzero, Monitorx, Mwaitx };

// The 0F 01 xx instruction at `code`, after prefixes.
Op decode(const uint8_t* code, size_t available, size_t* length, bool* address32) {
  const size_t at = prefix_length(code, available, address32);
  if (at + 3 > available || code[at] != 0x0F || code[at + 1] != 0x01) return Op::None;
  *length = at + 3;
  switch (code[at + 2]) {
    case 0xFC: return Op::Clzero;
    case 0xFA: return Op::Monitorx;
    case 0xFB: return Op::Mwaitx;
    default: return Op::None;
  }
}

// SSE4a's bit-field instructions on the low quadword of an XMM register
// (documented in the vendor's programmer's manual, volume 4): a field of
// `length` bits (0 means 64) at bit `index`; the high quadword of the
// destination is left as it was (the manual leaves it undefined).
//   EXTRQ xmm, imm8, imm8   66 0F 78 /0 ib ib   dest = field of dest at index, zero-extended
//   EXTRQ xmm1, xmm2        66 0F 79 /r         length xmm2[5:0], index xmm2[13:8]
//   INSERTQ xmm1, xmm2, imm8, imm8  F2 0F 78 /r ib ib   field of dest at index = low bits of xmm2
//   INSERTQ xmm1, xmm2      F2 0F 79 /r         length xmm2[77:72], index xmm2[69:64]
// Only register operands exist (mod = 11); a REX prefix may extend both.
uint64_t field_mask(uint32_t length) { return length >= 64 ? ~0ull : ((1ull << length) - 1); }

M128A* xmm(CONTEXT* context, uint32_t index) { return &context->Xmm0 + index; }

std::atomic<uint64_t> g_sse4a{0};

// Carries out an SSE4a instruction at `code`; its length, or 0 if it is not one.
size_t sse4a(CONTEXT* context, const uint8_t* code) {
  size_t at = 0;
  const uint8_t mandatory = code[at];
  if (mandatory != 0x66 && mandatory != 0xF2) return 0;
  ++at;
  uint8_t rex = 0;
  if ((code[at] & 0xF0) == 0x40) rex = code[at++];
  if (code[at] != 0x0F || (code[at + 1] != 0x78 && code[at + 1] != 0x79)) return 0;
  const bool immediate = code[at + 1] == 0x78;
  const uint8_t modrm = code[at + 2];
  at += 3;
  if ((modrm >> 6) != 3) return 0;
  const uint32_t reg = ((modrm >> 3) & 7) | ((rex & 4) ? 8 : 0);
  const uint32_t rm = (modrm & 7) | ((rex & 1) ? 8 : 0);
  uint32_t length = 0, index = 0;
  if (immediate) {
    length = code[at] & 0x3F;
    index = code[at + 1] & 0x3F;
    at += 2;
  }
  if (length == 0) length = 64;
  if (mandatory == 0x66) {  // EXTRQ
    M128A* dest = xmm(context, rm);
    if (immediate) {
      if (((modrm >> 3) & 7) != 0) return 0;
    } else {
      // EXTRQ xmm1, xmm2: xmm1 (reg) the destination, xmm2 (r/m) the control.
      const uint64_t control = xmm(context, rm)->Low;
      length = control & 0x3F;
      index = (control >> 8) & 0x3F;
      dest = xmm(context, reg);
      if (length == 0) length = 64;
    }
    const uint64_t value = dest->Low;
    dest->Low = (index >= 64 ? 0 : (value >> index)) & field_mask(length);
  } else {  // INSERTQ
    const M128A* source = xmm(context, rm);
    if (!immediate) {
      length = static_cast<uint32_t>((source->High >> 8) & 0x3F);
      index = static_cast<uint32_t>(source->High & 0x3F);
      if (length == 0) length = 64;
    }
    M128A* dest = xmm(context, reg);
    const uint64_t mask = index >= 64 ? 0 : field_mask(length) << index;
    dest->Low = (dest->Low & ~mask) | ((source->Low << index) & mask);
  }
  g_sse4a.fetch_add(1, std::memory_order_relaxed);
  return at;
}

std::atomic<uint64_t> g_waits{0};

LONG CALLBACK cpu_handler(EXCEPTION_POINTERS* info) {
  if (info->ExceptionRecord->ExceptionCode != EXCEPTION_ILLEGAL_INSTRUCTION) return EXCEPTION_CONTINUE_SEARCH;
  CONTEXT* context = info->ContextRecord;
  const auto* code = reinterpret_cast<const uint8_t*>(context->Rip);
  if (const size_t sse4a_length = sse4a(context, code)) {
    static volatile int64_t* const emulated = counter("cpu.sse4a_instructions");
    _InterlockedIncrement64(emulated);
    if (g_sse4a.load(std::memory_order_relaxed) == 1) {
      LOG_INFO(kComponent, "the host CPU lacks SSE4a (EXTRQ/INSERTQ); they are carried out by the runtime (first at %p)",
               static_cast<const void*>(code));
    }
    context->Rip += sse4a_length;
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  size_t length = 0;
  bool address32 = false;
  const Op op = decode(code, 8, &length, &address32);
  if (op == Op::None) return EXCEPTION_CONTINUE_SEARCH;
  const DWORD saved_error = GetLastError();
  if (op == Op::Clzero) {
    uint64_t address = address32 ? static_cast<uint32_t>(context->Rax) : context->Rax;
    address &= ~static_cast<uint64_t>(kCacheLine - 1);
    // An access fault here (memory committed on first touch) is handled like
    // the title's own store would have been.
    std::memset(reinterpret_cast<void*>(address), 0, kCacheLine);
    if (g_emulated.fetch_add(1) == 0) {
      LOG_INFO(kComponent, "the host CPU lacks CLZERO; it is carried out by the runtime (first at %p)",
               static_cast<const void*>(code));
    }
  } else if (op == Op::Mwaitx) {
    // The wait may end at any time; it ends at once, after giving up the
    // processor, so callers re-check what they wait for.
    SwitchToThread();
    if (g_waits.fetch_add(1) == 0) {
      LOG_INFO(kComponent, "the host CPU lacks MONITORX/MWAITX; waits yield the processor (first at %p)",
               static_cast<const void*>(code));
    }
  }
  // MONITORX only arms the address monitor, which nothing here models.
  context->Rip += length;
  SetLastError(saved_error);
  return EXCEPTION_CONTINUE_EXECUTION;
}

}  // namespace

void install_cpu_compatibility() {
  AddVectoredExceptionHandler(1, &cpu_handler);
}

}  // namespace holy::rt
