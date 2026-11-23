// C interface that holyrt.dll exports to the console library implementations
// under core/libs and to the tools.
#pragma once

#include <cstdarg>
#include <cstddef>
#include <cstdint>

#if defined(HOLYRT_BUILD)
#define HOLYRT_API __declspec(dllexport)
#else
#define HOLYRT_API __declspec(dllimport)
#endif

extern "C" {

// The single symbol the launcher makes a title import; it only anchors the DLL.
HOLYRT_API void HolyRuntimeAnchor(void);

// Logging into the runtime's log. `level` is a holy::log::Level value.
HOLYRT_API int HolyLogEnabled(uint32_t level);
HOLYRT_API void HolyLog(uint32_t level, const char* component, const char* fmt, ...);
HOLYRT_API void HolyLogV(uint32_t level, const char* component, const char* fmt, va_list args);

// Reports a call into functionality that is not implemented yet and applies
// the launch policy: by default the title is terminated; in exploration mode
// the call returns so the caller can report failure to the title.
HOLYRT_API void HolyUnimplemented(const char* library, const char* symbol);

// As HolyUnimplemented, for calls that do not arrive through an import (an
// interface method, a callback table): `caller` is the return address in the
// code that made the call, so the report points at the title.
HOLYRT_API void HolyUnimplementedFrom(const char* library, const char* symbol, const void* caller);

// Writes "title+0x…", "module.dll+0x…" or a bare address for `address` into
// `buffer` (always terminated). The title image is never named.
HOLYRT_API void HolyDescribeAddress(const void* address, char* buffer, size_t size);

// Registers a reserved range of title memory whose pages are committed on
// first touch with `protect` (see core/runtime/memory.h). Used when the host
// cannot commit a title's memory pool up front.
HOLYRT_API void HolyCommitOnDemand(void* base, size_t size, uint32_t protect);

// Commits the on-demand parts of a buffer before it is handed to the kernel.
// Returns non-zero on success.
HOLYRT_API int HolyEnsureCommitted(const void* address, size_t size);

// Translates a path as the title sees it (mounted drives, package-relative
// paths) into a host path. Returns the length written, or 0 if `count` is too
// small.
HOLYRT_API size_t HolyTranslatePath(const wchar_t* title_path, wchar_t* out, size_t count);

// Absolute host paths of the running title's package and data directories.
// Empty strings when the runtime was not booted by the launcher.
HOLYRT_API const wchar_t* HolyPackageDir(void);
HOLYRT_API const wchar_t* HolyDataDir(void);

// Non-zero while the offline switch is on: the title must see a host without
// network connectivity.
HOLYRT_API int HolyIsOffline(void);

// Non-zero if the runtime interposes `symbol` when a title imports it from a
// host system library. Lets tools predict binding without running anything.
HOLYRT_API int HolyHasOverride(const char* library, const char* symbol);

}  // extern "C"
