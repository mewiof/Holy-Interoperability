#include "core/loader/profiler.h"

// clang-format off
#include <windows.h>
#include <dbghelp.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <timeapi.h>
// clang-format on

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cwctype>
#include <map>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/common/strings.h"

namespace holy::loader {

namespace {

constexpr int kMaxDepth = 48;
constexpr ULONGLONG kReportEveryMs = 30000;

struct ThreadInfo {
  HANDLE handle = nullptr;
  uint64_t busy = 0;
  uint64_t blocked = 0;
  std::wstring name;
  std::unordered_map<DWORD64, uint64_t> exclusive;  // busy samples by function
};

struct Module {
  DWORD64 base = 0;
  std::string name;  // "title" for the executable
  bool system = false;
};

std::wstring lower(std::wstring text) {
  for (wchar_t& c : text) c = static_cast<wchar_t>(std::towlower(c));
  return text;
}

}  // namespace

struct Profiler::State {
  HANDLE process = nullptr;
  DWORD pid = 0;
  uint32_t interval_ms = 1;
  std::wstring report_path;
  std::wstring exe_path;
  std::wstring system_dir;
  std::atomic<bool> stop{false};
  std::thread worker;

  ULONGLONG started = 0;
  uint64_t samples = 0, busy_samples = 0, blocked_samples = 0;
  std::unordered_map<DWORD, ThreadInfo> threads;
  std::unordered_map<DWORD64, uint64_t> exclusive, inclusive, wait_sites;
  std::unordered_map<DWORD64, Module> modules;  // by base
  std::unordered_map<DWORD64, DWORD64> function_of;  // pc -> function start (cache)
  std::unordered_map<DWORD64, bool> syscall_stub;    // function start -> is an Nt/Zw stub

  void run();
  void sample_all();
  void refresh_threads();
  const Module& module_at(DWORD64 pc);
  DWORD64 function_start(DWORD64 pc);
  std::string describe(DWORD64 function);
  bool is_syscall(DWORD64 function);
  void write_report(bool final_report);
};

Profiler::Profiler(HANDLE process, uint32_t interval_ms, std::wstring report_path, std::wstring exe_path)
    : state_(std::make_unique<State>()) {
  State& s = *state_;
  DuplicateHandle(GetCurrentProcess(), process, GetCurrentProcess(), &s.process, 0, FALSE, DUPLICATE_SAME_ACCESS);
  s.pid = GetProcessId(process);
  s.interval_ms = interval_ms ? interval_ms : 1;
  s.report_path = std::move(report_path);
  s.exe_path = lower(std::move(exe_path));
  wchar_t windows[MAX_PATH] = {};
  GetSystemWindowsDirectoryW(windows, MAX_PATH);
  s.system_dir = lower(windows);
  s.worker = std::thread([&s] { s.run(); });
}

Profiler::~Profiler() {
  State& s = *state_;
  s.stop.store(true);
  if (s.worker.joinable()) s.worker.join();
  for (auto& [tid, info] : s.threads) {
    if (info.handle) CloseHandle(info.handle);
  }
  if (s.process) CloseHandle(s.process);
}

void Profiler::State::run() {
  timeBeginPeriod(1);
  SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS);
  // Symbols of this project's DLLs are found next to them (the runtime and
  // libs/ directories are on the path the title process loads them from).
  SymInitializeW(process, nullptr, FALSE);
  started = GetTickCount64();
  ULONGLONG last_refresh = 0, last_report = started;
  while (!stop.load()) {
    Sleep(interval_ms);
    const ULONGLONG now = GetTickCount64();
    if (now - last_refresh > 1000) {
      SymRefreshModuleList(process);
      refresh_threads();
      last_refresh = now;
    }
    DWORD code = 0;
    if (!GetExitCodeProcess(process, &code) || code != STILL_ACTIVE) break;
    sample_all();
    if (now - last_report > kReportEveryMs) {
      write_report(false);
      last_report = now;
    }
  }
  write_report(true);
  SymCleanup(process);
  timeEndPeriod(1);
}

void Profiler::State::refresh_threads() {
  const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return;
  THREADENTRY32 entry{};
  entry.dwSize = sizeof(entry);
  std::unordered_set<DWORD> alive;
  for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
    if (entry.th32OwnerProcessID != pid) continue;
    alive.insert(entry.th32ThreadID);
    ThreadInfo& info = threads[entry.th32ThreadID];
    if (!info.handle) {
      info.handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
                               entry.th32ThreadID);
    }
    if (info.handle && info.name.empty()) {
      PWSTR description = nullptr;
      if (SUCCEEDED(GetThreadDescription(info.handle, &description)) && description) {
        info.name = description;
        LocalFree(description);
      }
    }
  }
  CloseHandle(snapshot);
  // Exited threads keep their counts; their handles are closed.
  for (auto& [tid, info] : threads) {
    if (!alive.count(tid) && info.handle) {
      CloseHandle(info.handle);
      info.handle = nullptr;
    }
  }
}

const Module& Profiler::State::module_at(DWORD64 pc) {
  static const Module kUnknown{0, "?", true};
  const DWORD64 base = SymGetModuleBase64(process, pc);
  if (!base) return kUnknown;
  auto found = modules.find(base);
  if (found != modules.end()) return found->second;
  Module m;
  m.base = base;
  // The module's path as the process loaded it (the debugging library's
  // own record of it may be a bare name until symbols are loaded).
  wchar_t loaded[MAX_PATH] = {};
  GetModuleFileNameExW(process, reinterpret_cast<HMODULE>(base), loaded, MAX_PATH);
  const std::wstring path = lower(loaded);
  IMAGEHLP_MODULEW64 info{};
  info.SizeOfStruct = sizeof(info);
  if (!exe_path.empty() && path == exe_path) {
    m.name = "title";
  } else if (SymGetModuleInfoW64(process, base, &info)) {
    m.name = narrow(info.ModuleName);
  } else {
    m.name = "?";
  }
  m.system = !path.empty() && path.compare(0, system_dir.size(), system_dir) == 0;
  return modules.emplace(base, std::move(m)).first->second;
}

DWORD64 Profiler::State::function_start(DWORD64 pc) {
  auto found = function_of.find(pc);
  if (found != function_of.end()) return found->second;
  DWORD64 start = pc;
  const DWORD64 base = SymGetModuleBase64(process, pc);
  if (!base) return pc;  // module not known yet: asked again later
  if (const auto* entry = static_cast<const RUNTIME_FUNCTION*>(SymFunctionTableAccess64(process, pc))) {
    start = base + entry->BeginAddress;
  }
  if (function_of.size() < 1u << 20) function_of.emplace(pc, start);
  return start;
}

std::string Profiler::State::describe(DWORD64 function) {
  const Module& m = module_at(function);
  char text[512];
  alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256];
  auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
  symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
  symbol->MaxNameLen = 255;
  DWORD64 displacement = 0;
  // Symbol names only for modules that are not the title's (its exports
  // would name its internals by the title's own words).
  if (m.name != "title" && SymFromAddr(process, function, &displacement, symbol)) {
    if (displacement) {
      std::snprintf(text, sizeof(text), "%s!%s+0x%llx", m.name.c_str(), symbol->Name,
                    static_cast<unsigned long long>(displacement));
    } else {
      std::snprintf(text, sizeof(text), "%s!%s", m.name.c_str(), symbol->Name);
    }
  } else {
    std::snprintf(text, sizeof(text), "%s+0x%llx", m.name.c_str(), static_cast<unsigned long long>(function - m.base));
  }
  return text;
}

bool Profiler::State::is_syscall(DWORD64 function) {
  auto found = syscall_stub.find(function);
  if (found != syscall_stub.end()) return found->second;
  bool stub = false;
  const Module& m = module_at(function);
  if (m.system && (_stricmp(m.name.c_str(), "ntdll") == 0 || _stricmp(m.name.c_str(), "win32u") == 0)) {
    alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 128];
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = 127;
    DWORD64 displacement = 0;
    if (!SymFromAddr(process, function, &displacement, symbol)) return false;  // asked again later
    stub = (symbol->Name[0] == 'N' && symbol->Name[1] == 't') || (symbol->Name[0] == 'Z' && symbol->Name[1] == 'w');
  } else if (!m.base) {
    return false;  // module not known yet: asked again later
  }
  syscall_stub.emplace(function, stub);
  return stub;
}

void Profiler::State::sample_all() {
  for (auto& [tid, info] : threads) {
    if (!info.handle || tid == GetCurrentThreadId()) continue;
    if (SuspendThread(info.handle) == static_cast<DWORD>(-1)) continue;
    CONTEXT context{};
    context.ContextFlags = CONTEXT_FULL;
    DWORD64 frames[kMaxDepth];
    int depth = 0;
    if (GetThreadContext(info.handle, &context)) {
      STACKFRAME64 frame{};
      frame.AddrPC.Offset = context.Rip;
      frame.AddrPC.Mode = AddrModeFlat;
      frame.AddrFrame.Offset = context.Rbp;
      frame.AddrFrame.Mode = AddrModeFlat;
      frame.AddrStack.Offset = context.Rsp;
      frame.AddrStack.Mode = AddrModeFlat;
      while (depth < kMaxDepth &&
             StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, info.handle, &frame, &context, nullptr,
                         SymFunctionTableAccess64, SymGetModuleBase64, nullptr) &&
             frame.AddrPC.Offset) {
        frames[depth++] = frame.AddrPC.Offset;
      }
    }
    ResumeThread(info.handle);
    if (depth == 0) continue;
    ++samples;
    DWORD64 functions[kMaxDepth];
    for (int i = 0; i < depth; ++i) functions[i] = function_start(i == 0 ? frames[i] : frames[i] - 1);
    if (is_syscall(frames[0])) {
      ++blocked_samples;
      ++info.blocked;
      ++info.exclusive[functions[0]];  // which system call
      for (int i = 0; i < depth; ++i) {
        if (!module_at(functions[i]).system) {
          ++wait_sites[functions[i]];
          break;
        }
      }
      continue;
    }
    ++busy_samples;
    ++info.busy;
    ++exclusive[functions[0]];
    ++info.exclusive[functions[0]];
    // Inclusive: each function once per sample.
    for (int i = 0; i < depth; ++i) {
      bool seen = false;
      for (int j = 0; j < i && !seen; ++j) seen = functions[j] == functions[i];
      if (!seen) ++inclusive[functions[i]];
    }
  }
}

void Profiler::State::write_report(bool final_report) {
  FILE* out = _wfopen(report_path.c_str(), L"w");
  if (!out) return;
  const double seconds = static_cast<double>(GetTickCount64() - started) / 1000.0;
  std::fprintf(out, "Holy Interoperability profile (%s): %.1f s, %llu samples every %u ms (%llu busy, %llu blocked)\n\n",
               final_report ? "final" : "in progress", seconds, static_cast<unsigned long long>(samples), interval_ms,
               static_cast<unsigned long long>(busy_samples), static_cast<unsigned long long>(blocked_samples));
  auto table = [&](const char* title, const std::unordered_map<DWORD64, uint64_t>& counts, uint64_t total,
                   size_t rows) {
    std::vector<std::pair<uint64_t, DWORD64>> sorted;
    sorted.reserve(counts.size());
    for (const auto& [function, count] : counts) sorted.emplace_back(count, function);
    std::sort(sorted.rbegin(), sorted.rend());
    std::fprintf(out, "%s\n", title);
    for (size_t i = 0; i < sorted.size() && i < rows; ++i) {
      std::fprintf(out, "  %7.2f%%  %8llu  %s\n", total ? 100.0 * sorted[i].first / total : 0.0,
                   static_cast<unsigned long long>(sorted[i].first), describe(sorted[i].second).c_str());
    }
    std::fprintf(out, "\n");
  };
  // Threads by processor time (user and kernel mode).
  struct Row {
    double user, kernel;
    DWORD tid;
  };
  std::vector<Row> rows;
  for (const auto& [tid, info] : threads) {
    if (info.busy == 0 && info.blocked == 0) continue;
    Row row{0, 0, tid};
    FILETIME created, exited, kernel, user;
    if (info.handle && GetThreadTimes(info.handle, &created, &exited, &kernel, &user)) {
      const auto ticks = [](FILETIME t) { return (static_cast<uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
      row.user = static_cast<double>(ticks(user)) / 1e7;
      row.kernel = static_cast<double>(ticks(kernel)) / 1e7;
    }
    rows.push_back(row);
  }
  std::sort(rows.begin(), rows.end(),
            [](const Row& a, const Row& b) { return a.user + a.kernel > b.user + b.kernel; });
  std::fprintf(out, "Threads (user s, kernel s, busy samples, samples in system calls)\n");
  for (const Row& row : rows) {
    const ThreadInfo& info = threads[row.tid];
    std::fprintf(out, "  %6lu  %8.2f  %8.2f  %8llu  %8llu  %s\n", row.tid, row.user, row.kernel,
                 static_cast<unsigned long long>(info.busy), static_cast<unsigned long long>(info.blocked),
                 narrow(info.name).c_str());
  }
  std::fprintf(out, "\n");
  for (size_t i = 0; i < rows.size() && i < 12; ++i) {
    const ThreadInfo& info = threads[rows[i].tid];
    if (rows[i].user + rows[i].kernel < 0.5) break;
    char title[160];
    std::snprintf(title, sizeof(title), "Thread %lu %s: where it is (functions and system calls)", rows[i].tid,
                  narrow(info.name).c_str());
    table(title, info.exclusive, info.busy + info.blocked, 10);
  }
  table("Busy: functions where threads run (exclusive)", exclusive, busy_samples, 60);
  table("Busy: functions threads run inside of (inclusive)", inclusive, busy_samples, 80);
  std::fprintf(out, "Modules seen\n");
  for (const auto& [base, m] : modules) {
    std::fprintf(out, "  %016llx  %s%s\n", static_cast<unsigned long long>(base), m.name.c_str(),
                 m.system ? " (system)" : "");
  }
  std::fprintf(out, "\n");
  table("Blocked: where threads wait from (first frame outside system DLLs)", wait_sites, blocked_samples, 40);
  std::fclose(out);
}

}  // namespace holy::loader
