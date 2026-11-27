#include "core/runtime/crash.h"
#include "core/runtime/stats.h"

#include <atomic>
#include <cstdio>

#include "core/common/log.h"
#include "core/runtime/state.h"

namespace holy::rt {
namespace {

constexpr char kComponent[] = "crash";
constexpr DWORD kCppException = 0xE06D7363;

std::atomic<LPTOP_LEVEL_EXCEPTION_FILTER> g_title_filter{nullptr};
std::atomic<int> g_first_chance_budget{16};

// Reads memory that may be unmapped without faulting (no exception reaches
// the title's or the memory library's handlers).
bool read_memory(DWORD64 address, void* out, size_t bytes) {
  SIZE_T read = 0;
  return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), out, bytes, &read) &&
         read == bytes;
}

void log_frame(int index, DWORD64 address) {
  LOG_ERROR(kComponent, "  #%02d %s", index,
            describe_address(reinterpret_cast<const void*>(address)).c_str());
}

// May fault on a corrupted stack; only call through walk_stack().
void walk_stack_unguarded(CONTEXT context) {
  for (int index = 0; index < 40 && context.Rip != 0; ++index) {
    log_frame(index, context.Rip);
    DWORD64 image_base = 0;
    PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &image_base, nullptr);
    if (!function) {
      // Leaf function (or generated code): the return address is on top of the stack.
      context.Rip = *reinterpret_cast<const DWORD64*>(context.Rsp);
      context.Rsp += sizeof(DWORD64);
    } else {
      void* handler_data = nullptr;
      DWORD64 establisher_frame = 0;
      RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, function, &context, &handler_data,
                       &establisher_frame, nullptr);
    }
  }
}

}  // namespace

void walk_stack(const CONTEXT* context) {
  __try {
    walk_stack_unguarded(*context);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

namespace {

void log_exception(const char* kind, const EXCEPTION_POINTERS* info) {
  const EXCEPTION_RECORD* record = info->ExceptionRecord;
  LOG_ERROR(kComponent, "%s exception 0x%08lX at %s", kind, record->ExceptionCode,
            describe_address(record->ExceptionAddress).c_str());
  if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) {
    const ULONG_PTR operation = record->ExceptionInformation[0];
    LOG_ERROR(kComponent, "  %s address 0x%016llx",
              operation == 0 ? "reading" : (operation == 1 ? "writing" : "executing"),
              static_cast<unsigned long long>(record->ExceptionInformation[1]));
  }
}

void log_registers(const CONTEXT* c) {
  LOG_ERROR(kComponent, "  rax=%016llx rbx=%016llx rcx=%016llx rdx=%016llx", c->Rax, c->Rbx, c->Rcx, c->Rdx);
  LOG_ERROR(kComponent, "  rsi=%016llx rdi=%016llx rbp=%016llx rsp=%016llx", c->Rsi, c->Rdi, c->Rbp, c->Rsp);
  LOG_ERROR(kComponent, "  r8 =%016llx r9 =%016llx r10=%016llx r11=%016llx", c->R8, c->R9, c->R10, c->R11);
  LOG_ERROR(kComponent, "  r12=%016llx r13=%016llx r14=%016llx r15=%016llx", c->R12, c->R13, c->R14, c->R15);
  // Register values inside loaded images (code and static data), by image.
  const struct {
    const char* name;
    DWORD64 value;
  } registers[] = {{"rax", c->Rax}, {"rbx", c->Rbx}, {"rcx", c->Rcx}, {"rdx", c->Rdx}, {"rsi", c->Rsi},
                   {"rdi", c->Rdi}, {"rbp", c->Rbp}, {"r8", c->R8},   {"r9", c->R9},   {"r10", c->R10},
                   {"r11", c->R11}, {"r12", c->R12}, {"r13", c->R13}, {"r14", c->R14}, {"r15", c->R15}};
  for (const auto& r : registers) {
    if (r.value < 0x10000) continue;
    const std::string where = describe_address(reinterpret_cast<const void*>(r.value));
    if (where.find('+') != std::string::npos) LOG_ERROR(kComponent, "  %s = %s", r.name, where.c_str());
  }
  // The first 128 bytes of readable memory registers point at (objects the
  // faulting code was working on), and the first 64 bytes of what those
  // point at in turn (one level), with values inside images named.
  auto dump = [](const char* label, uint64_t address, size_t count) {
    uint64_t words[16] = {};
    if (count > 16 || !read_memory(address, words, count * 8)) return false;
    for (size_t row = 0; row < count; row += 8) {
      LOG_ERROR(kComponent, "  %s+%02zx %016llx %016llx %016llx %016llx %016llx %016llx %016llx %016llx", label,
                row * 8, words[row], words[row + 1], words[row + 2], words[row + 3], words[row + 4], words[row + 5],
                words[row + 6], words[row + 7]);
    }
    return true;
  };
  for (const auto& r : registers) {
    if (r.value < 0x10000) continue;
    char label[32];
    std::snprintf(label, sizeof(label), "[%s]", r.name);
    if (!dump(label, r.value, 16)) continue;
    uint64_t words[16];
    read_memory(r.value, words, sizeof(words));
    for (size_t i = 0; i < 16; ++i) {
      if (words[i] < 0x10000 || (words[i] >> 48) != 0) continue;
      const std::string where = describe_address(reinterpret_cast<const void*>(words[i]));
      if (where.find('+') != std::string::npos) continue;  // code or static data: named below instead
      std::snprintf(label, sizeof(label), "[[%s+%02zx]]", r.name, i * 8);
      dump(label, words[i], 8);
    }
    for (size_t i = 0; i < 16; ++i) {
      if (words[i] < 0x10000) continue;
      const std::string where = describe_address(reinterpret_cast<const void*>(words[i]));
      if (where.find('+') != std::string::npos) LOG_ERROR(kComponent, "  [%s+%02zx] = %s", r.name, i * 8, where.c_str());
    }
  }
}

LONG CALLBACK first_chance_handler(EXCEPTION_POINTERS* info) {
  const DWORD code = info->ExceptionRecord->ExceptionCode;
  // Only error-severity codes are interesting; C++ exceptions are routine.
  if (code < 0xC0000000u || code == kCppException) return EXCEPTION_CONTINUE_SEARCH;
  if (g_first_chance_budget.fetch_sub(1) > 0) log_exception("first-chance", info);
  return EXCEPTION_CONTINUE_SEARCH;
}

LONG WINAPI unhandled_filter(EXCEPTION_POINTERS* info) {
  log_exception("unhandled", info);
  log_registers(info->ContextRecord);
  walk_stack(info->ContextRecord);
  log_stats("at the unhandled exception");
  if (const LPTOP_LEVEL_EXCEPTION_FILTER title_filter = g_title_filter.load()) {
    LOG_ERROR(kComponent, "passing the exception to the title's own filter");
    return title_filter(info);
  }
  log::close_file();
  return EXCEPTION_EXECUTE_HANDLER;
}

}  // namespace

void install_crash_handlers() {
  AddVectoredExceptionHandler(0, &first_chance_handler);
  SetUnhandledExceptionFilter(&unhandled_filter);
}

LPTOP_LEVEL_EXCEPTION_FILTER exchange_title_filter(LPTOP_LEVEL_EXCEPTION_FILTER filter) {
  return g_title_filter.exchange(filter);
}

void log_title_exit(const char* how, unsigned code) {
  const DWORD saved_error = GetLastError();
  LOG_ERROR(kComponent, "the title ends the process (%s, code %u)", how, code);
  CONTEXT context{};
  RtlCaptureContext(&context);
  walk_stack(&context);
  log_stats("at exit");
  SetLastError(saved_error);
}

}  // namespace holy::rt
