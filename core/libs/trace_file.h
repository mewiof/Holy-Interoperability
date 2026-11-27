// A trace file of a console library's traffic, for studying contracts the
// libraries do not carry out yet: on when the named environment variable is
// set (its value, if a number, caps the events written; 4000 otherwise),
// written to <data>/logs/<file name> as text, one event a line: milliseconds
// since the first event, the event's name, then dwords in hexadecimal.
#pragma once

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <mutex>

#include "core/runtime/api.h"

namespace holy::lib {

class TraceFile {
 public:
  TraceFile(const wchar_t* variable, const wchar_t* file_name) : variable_(variable), file_name_(file_name) {}

  bool on() {
    std::call_once(once_, [this] { open(); });
    return file_ != nullptr;
  }

  // One event: `name`, then `count` dwords of `words` (read only where the
  // memory is readable; unreadable dwords print as ????????).
  void event(const char* name, const void* words, size_t count) {
    if (!on()) return;
    std::lock_guard<std::mutex> guard(lock_);
    if (written_ >= cap_) return;
    ++written_;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    std::fprintf(file_, "%10.3f %s", static_cast<double>(now.QuadPart - start_.QuadPart) * 1000.0 / frequency_,
                 name);
    const auto* bytes = static_cast<const uint8_t*>(words);
    for (size_t i = 0; i < count; ++i) {
      uint32_t value;
      if (bytes && readable(bytes + 4 * i, 4)) {
        std::memcpy(&value, bytes + 4 * i, 4);
        std::fprintf(file_, " %08x", value);
      } else {
        std::fprintf(file_, " ????????");
      }
    }
    std::fputc('\n', file_);
    if (written_ == cap_) std::fprintf(file_, "(cap of %u events reached)\n", cap_);
    std::fflush(file_);
  }

 private:
  static bool readable(const void* at, size_t bytes) {
    MEMORY_BASIC_INFORMATION info{};
    const auto* p = static_cast<const uint8_t*>(at);
    for (const uint8_t* end = p + bytes; p < end;) {
      if (!VirtualQuery(p, &info, sizeof(info)) || info.State != MEM_COMMIT ||
          (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) || !(info.Protect & 0xEE)) {
        return false;
      }
      p = static_cast<const uint8_t*>(info.BaseAddress) + info.RegionSize;
    }
    return true;
  }

  void open() {
    wchar_t value[32] = {};
    if (!GetEnvironmentVariableW(variable_, value, 32)) return;
    const unsigned long cap = std::wcstoul(value, nullptr, 10);
    if (cap > 1) cap_ = static_cast<uint32_t>(cap);
    const wchar_t* data = HolyDataDir();
    if (!data || !*data) return;
    wchar_t path[MAX_PATH];
    std::swprintf(path, MAX_PATH, L"%ls\\logs\\%ls", data, file_name_);
    file_ = _wfopen(path, L"w");
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    frequency_ = static_cast<double>(frequency.QuadPart);
    QueryPerformanceCounter(&start_);
  }

  const wchar_t* variable_;
  const wchar_t* file_name_;
  std::once_flag once_;
  std::mutex lock_;
  FILE* file_ = nullptr;
  uint32_t cap_ = 4000, written_ = 0;
  double frequency_ = 1.0;
  LARGE_INTEGER start_{};
};

}  // namespace holy::lib
