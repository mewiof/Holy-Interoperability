#include "core/runtime/thunks.h"

#include <windows.h>

#include <cstring>

#include "core/common/boot_block.h"
#include "core/common/log.h"
#include "core/runtime/state.h"

namespace holy::rt {
namespace {

constexpr size_t kChunkSize = 0x10000;
constexpr size_t kStubSize = 24;

// Shared trampoline. On entry r10 holds the ImportRecord and the stack is as
// the title's `call` left it. It saves the argument registers, calls
// dispatch(record, saved_registers) and jumps to the address dispatch returns
// with the argument registers restored.
//
//   push r9 / push r8 / push rdx / push rcx
//   mov  rdx, rsp
//   mov  rcx, r10
//   sub  rsp, 0x68
//   movups [rsp+0x20..0x50], xmm0..xmm3
//   mov  rax, <dispatch>
//   call rax
//   movups xmm0..xmm3, [rsp+0x20..0x50]
//   add  rsp, 0x68
//   pop  rcx / pop rdx / pop r8 / pop r9
//   jmp  rax
constexpr uint8_t kTrampolineHead[] = {
    0x41, 0x51, 0x41, 0x50, 0x52, 0x51,                          // push r9, r8, rdx, rcx
    0x48, 0x89, 0xE2,                                            // mov rdx, rsp
    0x4C, 0x89, 0xD1,                                            // mov rcx, r10
    0x48, 0x83, 0xEC, 0x68,                                      // sub rsp, 0x68
    0x0F, 0x11, 0x44, 0x24, 0x20, 0x0F, 0x11, 0x4C, 0x24, 0x30,  // movups [rsp+0x20], xmm0 / [rsp+0x30], xmm1
    0x0F, 0x11, 0x54, 0x24, 0x40, 0x0F, 0x11, 0x5C, 0x24, 0x50,  // movups [rsp+0x40], xmm2 / [rsp+0x50], xmm3
    0x48, 0xB8,                                                  // mov rax, imm64
};
constexpr uint8_t kTrampolineTail[] = {
    0xFF, 0xD0,                                                  // call rax
    0x0F, 0x10, 0x44, 0x24, 0x20, 0x0F, 0x10, 0x4C, 0x24, 0x30,  // movups xmm0, [rsp+0x20] / xmm1, [rsp+0x30]
    0x0F, 0x10, 0x54, 0x24, 0x40, 0x0F, 0x10, 0x5C, 0x24, 0x50,  // movups xmm2, [rsp+0x40] / xmm3, [rsp+0x50]
    0x48, 0x83, 0xC4, 0x68,                                      // add rsp, 0x68
    0x59, 0x5A, 0x41, 0x58, 0x41, 0x59,                          // pop rcx, rdx, r8, r9
    0xFF, 0xE0,                                                  // jmp rax
};

struct Chunk {
  uint8_t* base = nullptr;
  size_t used = 0;
  uint8_t* trampoline = nullptr;
};

Chunk g_chunk;
SRWLOCK g_lock = SRWLOCK_INIT;

uint64_t return_zero() {
  return 0;
}

// Layout of `frame`: [0..3] rcx, rdx, r8, r9; [4] return address; [5..8] the
// caller's home space; [9..] stack arguments.
void* dispatch(ImportRecord* record, const uint64_t* frame) {
  const DWORD saved_error = GetLastError();
  const uint64_t count = record->calls.fetch_add(1, std::memory_order_relaxed) + 1;
  const void* caller = reinterpret_cast<const void*>(frame[4]);
  void* next = record->target;

  if (!record->target) {
    LOG_ERROR("import", "unimplemented import %s!%s called from %s", record->library.c_str(),
              record->symbol.c_str(), describe_address(caller).c_str());
    LOG_ERROR("import", "  rcx=%016llx rdx=%016llx r8=%016llx r9=%016llx", frame[0], frame[1], frame[2],
              frame[3]);
    LOG_ERROR("import", "  stack arguments: %016llx %016llx %016llx %016llx", frame[9], frame[10], frame[11],
              frame[12]);
    if (state().trap_unimplemented) fatal_exit(kExitUnimplementedImport);
    next = reinterpret_cast<void*>(&return_zero);
  } else if (count <= 8 || (count & (count - 1)) == 0) {
    LOG_INFO("trace", "%s!%s(%llx, %llx, %llx, %llx) from %s [call %llu]", record->library.c_str(),
             record->symbol.c_str(), frame[0], frame[1], frame[2], frame[3], describe_address(caller).c_str(),
             count);
  }

  SetLastError(saved_error);
  return next;
}

bool start_chunk() {
  auto* base = static_cast<uint8_t*>(
      VirtualAlloc(nullptr, kChunkSize, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
  if (!base) return false;
  uint8_t* cursor = base;
  std::memcpy(cursor, kTrampolineHead, sizeof(kTrampolineHead));
  cursor += sizeof(kTrampolineHead);
  const uint64_t handler = reinterpret_cast<uint64_t>(&dispatch);
  std::memcpy(cursor, &handler, sizeof(handler));
  cursor += sizeof(handler);
  std::memcpy(cursor, kTrampolineTail, sizeof(kTrampolineTail));
  cursor += sizeof(kTrampolineTail);

  g_chunk.base = base;
  g_chunk.trampoline = base;
  g_chunk.used = (static_cast<size_t>(cursor - base) + 15) & ~static_cast<size_t>(15);
  return true;
}

}  // namespace

void* make_reporting_thunk(ImportRecord* record) {
  AcquireSRWLockExclusive(&g_lock);
  if (!g_chunk.base || g_chunk.used + kStubSize > kChunkSize) {
    if (!start_chunk()) {
      ReleaseSRWLockExclusive(&g_lock);
      return nullptr;
    }
  }
  uint8_t* stub = g_chunk.base + g_chunk.used;
  g_chunk.used += kStubSize;

  uint8_t* cursor = stub;
  const uint64_t record_address = reinterpret_cast<uint64_t>(record);
  const uint64_t trampoline_address = reinterpret_cast<uint64_t>(g_chunk.trampoline);
  *cursor++ = 0x49;  // mov r10, imm64
  *cursor++ = 0xBA;
  std::memcpy(cursor, &record_address, sizeof(record_address));
  cursor += sizeof(record_address);
  *cursor++ = 0x48;  // mov rax, imm64
  *cursor++ = 0xB8;
  std::memcpy(cursor, &trampoline_address, sizeof(trampoline_address));
  cursor += sizeof(trampoline_address);
  *cursor++ = 0xFF;  // jmp rax
  *cursor++ = 0xE0;

  FlushInstructionCache(GetCurrentProcess(), g_chunk.base, kChunkSize);
  ReleaseSRWLockExclusive(&g_lock);
  return stub;
}

}  // namespace holy::rt
