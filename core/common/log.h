// Minimal leveled logger shared by the launcher, the tools and the in-process
// runtime. It only depends on kernel32 so it is safe to use very early in a
// title process (including under the loader lock) and it preserves the calling
// thread's last-error value so it can be used inside API interposers.
#pragma once

#include <cstdarg>
#include <cstdint>
#include <string>
#include <string_view>

namespace holy::log {

enum class Level : uint32_t { Trace = 0, Debug = 1, Info = 2, Warn = 3, Error = 4, Off = 5 };

void set_level(Level level);
Level level();
bool parse_level(std::string_view text, Level& out);

// Opens (truncating) a log file that receives every line in addition to
// stderr. Returns false if the file cannot be created.
bool open_file(const std::wstring& path);
void close_file();
void set_stderr_enabled(bool enabled);

void write(Level level, const char* component, const char* fmt, ...);
void vwrite(Level level, const char* component, const char* fmt, va_list args);

inline bool enabled(Level lvl) {
  return static_cast<uint32_t>(lvl) >= static_cast<uint32_t>(level());
}

}  // namespace holy::log

#define HOLY_LOG(lvl, component, ...)                                               \
  do {                                                                              \
    if (::holy::log::enabled(lvl)) ::holy::log::write(lvl, component, __VA_ARGS__); \
  } while (0)

#define LOG_TRACE(component, ...) HOLY_LOG(::holy::log::Level::Trace, component, __VA_ARGS__)
#define LOG_DEBUG(component, ...) HOLY_LOG(::holy::log::Level::Debug, component, __VA_ARGS__)
#define LOG_INFO(component, ...) HOLY_LOG(::holy::log::Level::Info, component, __VA_ARGS__)
#define LOG_WARN(component, ...) HOLY_LOG(::holy::log::Level::Warn, component, __VA_ARGS__)
#define LOG_ERROR(component, ...) HOLY_LOG(::holy::log::Level::Error, component, __VA_ARGS__)
