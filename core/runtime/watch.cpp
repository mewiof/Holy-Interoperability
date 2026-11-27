#include "core/runtime/watch.h"

#include <windows.h>
#include <tlhelp32.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "core/common/log.h"
#include "core/runtime/crash.h"
#include "core/runtime/state.h"

namespace holy::rt {
namespace {

constexpr char kComponent[] = "watch";

struct Spec {
  uint64_t at = 0;  // absolute address of the trigger instruction
  bool has_if = false;
  int if_reg = -1;
  int64_t if_offset = 0;
  uint32_t if_value = 0;
  int watch_reg = -1;
  int64_t watch_offset = 0;
  bool watch_indirect = false;  // watch the address stored there instead
};

Spec g_spec;
bool g_active = false;
// The three latest armed addresses (DR1-DR3), as a ring.
std::atomic<uint64_t> g_watched[3] = {};
std::atomic<uint32_t> g_next{0};
std::atomic<int> g_triggers_logged{0}, g_writes_logged{0};
HANDLE g_rearm = nullptr;

const char* const kRegisters[] = {"rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp",
                                  "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15"};

int register_index(const std::string& name) {
  for (int i = 0; i < 16; ++i) {
    if (name == kRegisters[i]) return i;
  }
  return -1;
}

uint64_t register_value(const CONTEXT* c, int index) {
  const DWORD64 values[] = {c->Rax, c->Rbx, c->Rcx, c->Rdx, c->Rsi, c->Rdi, c->Rbp, c->Rsp,
                            c->R8,  c->R9,  c->R10, c->R11, c->R12, c->R13, c->R14, c->R15};
  return index >= 0 && index < 16 ? values[index] : 0;
}

// "<reg>+<hex offset>" or "<reg>-<hex offset>".
bool parse_operand(const std::string& text, int* reg, int64_t* offset) {
  size_t sign = text.find_first_of("+-");
  *reg = register_index(text.substr(0, sign));
  *offset = 0;
  if (sign != std::string::npos) {
    *offset = static_cast<int64_t>(std::strtoull(text.c_str() + sign + 1, nullptr, 16));
    if (text[sign] == '-') *offset = -*offset;
  }
  return *reg >= 0;
}

bool parse(const std::string& text) {
  size_t start = 0;
  while (start < text.size()) {
    const size_t end = text.find(';', start);
    const std::string part = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
    start = end == std::string::npos ? text.size() : end + 1;
    const size_t eq = part.find('=');
    if (eq == std::string::npos) return false;
    const std::string key = part.substr(0, eq), value = part.substr(eq + 1);
    if (key == "at") {
      g_spec.at = reinterpret_cast<uint64_t>(state().title_base) + std::strtoull(value.c_str(), nullptr, 16);
    } else if (key == "if") {
      const size_t colon = value.find(':');
      if (colon == std::string::npos || !parse_operand(value.substr(0, colon), &g_spec.if_reg, &g_spec.if_offset)) {
        return false;
      }
      g_spec.if_value = static_cast<uint32_t>(std::strtoull(value.c_str() + colon + 1, nullptr, 16));
      g_spec.has_if = true;
    } else if (key == "watch") {
      g_spec.watch_indirect = !value.empty() && value[0] == '*';
      if (!parse_operand(value.substr(g_spec.watch_indirect ? 1 : 0), &g_spec.watch_reg, &g_spec.watch_offset)) {
        return false;
      }
    } else {
      return false;
    }
  }
  return g_spec.at && g_spec.watch_reg >= 0;
}

// DR0: execution breakpoint at the trigger; DR1-DR3: 8-byte write watches.
void apply(CONTEXT* c) {
  c->Dr0 = g_spec.at;
  DWORD64 dr7 = 1;  // L0, execute, length 1
  DWORD64* slots[3] = {&c->Dr1, &c->Dr2, &c->Dr3};
  for (int i = 0; i < 3; ++i) {
    const uint64_t watched = g_watched[i].load();
    if (!watched) continue;
    *slots[i] = watched & ~7ull;
    const int n = i + 1;
    dr7 |= 1ull << (2 * n);                                     // Ln
    dr7 |= (1ull << (16 + 4 * n)) | (2ull << (18 + 4 * n));  // RWn = write, LENn = 8 bytes
  }
  c->Dr7 = dr7;
}

// Applies the debug registers to every other thread, one at a time; nothing
// is allocated while a thread is suspended.
void apply_all() {
  const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return;
  std::vector<DWORD> ids;
  ids.reserve(256);
  THREADENTRY32 entry{};
  entry.dwSize = sizeof(entry);
  for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
    if (entry.th32OwnerProcessID == GetCurrentProcessId() && entry.th32ThreadID != GetCurrentThreadId()) {
      ids.push_back(entry.th32ThreadID);
    }
  }
  CloseHandle(snapshot);
  for (DWORD id : ids) {
    const HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, id);
    if (!thread) continue;
    if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
      CONTEXT c{};
      c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
      if (GetThreadContext(thread, &c)) {
        apply(&c);
        SetThreadContext(thread, &c);
      }
      ResumeThread(thread);
    }
    CloseHandle(thread);
  }
}

DWORD WINAPI watch_thread(void*) {
  for (;;) {
    WaitForSingleObject(g_rearm, 500);
    apply_all();
  }
}

LONG CALLBACK on_single_step(EXCEPTION_POINTERS* info) {
  if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
  CONTEXT* c = info->ContextRecord;
  const DWORD64 dr6 = c->Dr6;
  if (!(dr6 & 0xF)) return EXCEPTION_CONTINUE_SEARCH;
  const DWORD saved_error = GetLastError();
  if (dr6 & 1) {
    bool match = true;
    if (g_spec.has_if) {
      uint32_t value = 0;
      SIZE_T read = 0;
      const uint64_t at = register_value(c, g_spec.if_reg) + g_spec.if_offset;
      match = ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(at), &value, 4, &read) &&
              value == g_spec.if_value;
    }
    if (match) {
      uint64_t watched = register_value(c, g_spec.watch_reg) + g_spec.watch_offset;
      if (g_spec.watch_indirect) {
        uint64_t target = 0;
        SIZE_T got = 0;
        ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(watched), &target, 8, &got);
        watched = target;
      }
      bool already = false;
      for (const auto& slot : g_watched) already = already || slot.load() == watched;
      if (!already) g_watched[g_next.fetch_add(1) % 3].store(watched);
      if (g_triggers_logged.fetch_add(1) < 512) {
        uint64_t current = 0;
        SIZE_T got = 0;
        ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(watched), &current, 8, &got);
        uint64_t pointed[2] = {};
        ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(current), pointed, 16, &got);
        LOG_WARN(kComponent, "trigger at %s (thread %lu, rsp %p): watching writes to %p (now %016llx -> %s, %016llx)",
                 describe_address(reinterpret_cast<void*>(c->Rip)).c_str(), GetCurrentThreadId(),
                 reinterpret_cast<void*>(c->Rsp), reinterpret_cast<void*>(watched),
                 static_cast<unsigned long long>(current), describe_address(reinterpret_cast<void*>(pointed[0])).c_str(),
                 static_cast<unsigned long long>(pointed[1]));
      }
      SetEvent(g_rearm);
    }
    c->EFlags |= 0x10000;  // resume flag: the trigger does not fire again on return
  }
  for (int i = 0; i < 3; ++i) {
    if (!(dr6 & (2ull << i))) continue;
    uint64_t value = 0;
    SIZE_T read = 0;
    const uint64_t watched = g_watched[i].load();
    ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(watched), &value, 8, &read);
    if (g_writes_logged.fetch_add(1) < 256) {
      LOG_WARN(kComponent, "write to %p (now %016llx) by thread %lu, after %s", reinterpret_cast<void*>(watched),
               static_cast<unsigned long long>(value), GetCurrentThreadId(),
               describe_address(reinterpret_cast<void*>(c->Rip)).c_str());
      walk_stack(c);
    }
  }
  apply(c);
  c->Dr6 = 0;
  SetLastError(saved_error);
  return EXCEPTION_CONTINUE_EXECUTION;
}

}  // namespace

void start_watch() {
  char text[256] = {};
  if (!GetEnvironmentVariableA("HOLY_WATCH", text, sizeof(text))) return;
  if (!parse(text)) {
    LOG_ERROR(kComponent, "HOLY_WATCH not understood: %s", text);
    return;
  }
  g_active = true;
  g_rearm = CreateEventW(nullptr, FALSE, TRUE, nullptr);
  AddVectoredExceptionHandler(1, &on_single_step);
  HANDLE thread = CreateThread(nullptr, 64 * 1024, &watch_thread, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
  if (thread) CloseHandle(thread);
  LOG_WARN(kComponent, "hardware watch armed at %s", describe_address(reinterpret_cast<void*>(g_spec.at)).c_str());
}

void watch_thread_started() {
  if (g_active) SetEvent(g_rearm);
}

}  // namespace holy::rt
