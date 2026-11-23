#include "core/common/log.h"

#include <windows.h>

#include <atomic>
#include <cstdio>

#include "core/common/strings.h"

namespace holy::log {
namespace {

std::atomic<uint32_t> g_level{static_cast<uint32_t>(Level::Info)};
std::atomic<bool> g_stderr{true};
HANDLE g_file = INVALID_HANDLE_VALUE;
SRWLOCK g_lock = SRWLOCK_INIT;

double elapsed_seconds() {
  static LARGE_INTEGER freq = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return f;
  }();
  static LARGE_INTEGER start = [] {
    LARGE_INTEGER s;
    QueryPerformanceCounter(&s);
    return s;
  }();
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  return static_cast<double>(now.QuadPart - start.QuadPart) / static_cast<double>(freq.QuadPart);
}

const char* level_tag(Level lvl) {
  switch (lvl) {
    case Level::Trace:
      return "T";
    case Level::Debug:
      return "D";
    case Level::Info:
      return "I";
    case Level::Warn:
      return "W";
    case Level::Error:
      return "E";
    default:
      return "?";
  }
}

void write_all(HANDLE handle, const char* data, DWORD size) {
  if (handle == nullptr || handle == INVALID_HANDLE_VALUE) return;
  DWORD written = 0;
  WriteFile(handle, data, size, &written, nullptr);
}

}  // namespace

void set_level(Level lvl) {
  g_level.store(static_cast<uint32_t>(lvl));
}
Level level() {
  return static_cast<Level>(g_level.load(std::memory_order_relaxed));
}

bool parse_level(std::string_view text, Level& out) {
  const std::string lower = to_lower(text);
  if (lower == "trace")
    out = Level::Trace;
  else if (lower == "debug")
    out = Level::Debug;
  else if (lower == "info")
    out = Level::Info;
  else if (lower == "warn" || lower == "warning")
    out = Level::Warn;
  else if (lower == "error")
    out = Level::Error;
  else if (lower == "off" || lower == "none")
    out = Level::Off;
  else
    return false;
  return true;
}

bool open_file(const std::wstring& path) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  AcquireSRWLockExclusive(&g_lock);
  if (g_file != INVALID_HANDLE_VALUE) CloseHandle(g_file);
  g_file = file;
  ReleaseSRWLockExclusive(&g_lock);
  return true;
}

void close_file() {
  AcquireSRWLockExclusive(&g_lock);
  if (g_file != INVALID_HANDLE_VALUE) CloseHandle(g_file);
  g_file = INVALID_HANDLE_VALUE;
  ReleaseSRWLockExclusive(&g_lock);
}

void set_stderr_enabled(bool enabled) {
  g_stderr.store(enabled);
}

void vwrite(Level lvl, const char* component, const char* fmt, va_list args) {
  if (!enabled(lvl)) return;
  const DWORD saved_error = GetLastError();

  char line[2048];
  int prefix = std::snprintf(line, sizeof(line), "[%9.3f] %s [%s] ", elapsed_seconds(), level_tag(lvl),
                             component ? component : "-");
  if (prefix < 0) prefix = 0;
  const size_t room = sizeof(line) - static_cast<size_t>(prefix) - 2;
  int body = std::vsnprintf(line + prefix, room, fmt, args);
  if (body < 0) body = 0;
  size_t length =
      static_cast<size_t>(prefix) + (static_cast<size_t>(body) < room ? static_cast<size_t>(body) : room - 1);
  line[length++] = '\n';

  AcquireSRWLockExclusive(&g_lock);
  write_all(g_file, line, static_cast<DWORD>(length));
  if (g_stderr.load(std::memory_order_relaxed)) {
    write_all(GetStdHandle(STD_ERROR_HANDLE), line, static_cast<DWORD>(length));
  }
  ReleaseSRWLockExclusive(&g_lock);

  SetLastError(saved_error);
}

void write(Level lvl, const char* component, const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vwrite(lvl, component, fmt, args);
  va_end(args);
}

}  // namespace holy::log
