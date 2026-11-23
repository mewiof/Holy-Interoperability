#include "core/runtime/crash.h"

#include <atomic>

#include "core/common/log.h"
#include "core/runtime/state.h"

namespace holy::rt {
namespace {

constexpr char kComponent[] = "crash";
constexpr DWORD kCppException = 0xE06D7363;

std::atomic<LPTOP_LEVEL_EXCEPTION_FILTER> g_title_filter{nullptr};
std::atomic<int> g_first_chance_budget{16};

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

void walk_stack(const CONTEXT* context) {
  __try {
    walk_stack_unguarded(*context);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

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

}  // namespace holy::rt
