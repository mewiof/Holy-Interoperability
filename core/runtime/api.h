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

// Non-zero when a call into unimplemented functionality ends the title
// (the default); zero in exploration mode, where such calls return.
HOLYRT_API int HolyTrapsUnimplemented(void);

// Writes "title+0x…", "module.dll+0x…" or a bare address for `address` into
// `buffer` (always terminated). The title image is never named.
HOLYRT_API void HolyDescribeAddress(const void* address, char* buffer, size_t size);

// Registers a reserved range of title memory whose pages are committed on
// first touch with `protect` (see core/runtime/memory.h). Used when the host
// cannot commit a title's memory pool up front.
HOLYRT_API void HolyCommitOnDemand(void* base, size_t size, uint32_t protect);

// The range at `base` (registered with HolyCommitOnDemand) was released.
HOLYRT_API void HolyCommitOnDemandReleased(void* base, size_t size);

// Commits the on-demand parts of a buffer before it is handed to the kernel.
// Returns non-zero on success.
HOLYRT_API int HolyEnsureCommitted(const void* address, size_t size);

// Lets a library that backs title memory lazily (physical pages) back a
// buffer before it is handed to the kernel; called by HolyEnsureCommitted.
// The hook returns non-zero on success.
typedef int (*HolyBackingHook)(const void* address, size_t size);
HOLYRT_API void HolySetBackingHook(HolyBackingHook hook);

// Lets a library that keeps host objects over title memory (the graphics
// library's heaps) let go of them before the memory is released or
// decommitted: the title's VirtualFree, and libraries releasing title memory
// themselves (HolyBeforeRelease), call the hook first.
typedef void (*HolyReleaseHook)(const void* address, size_t size);
HOLYRT_API void HolySetReleaseHook(HolyReleaseHook hook);
HOLYRT_API void HolyBeforeRelease(const void* address, size_t size);

// Lets a library that keeps copies of title memory (the graphics library's
// textures) hear of writes the host does not track for it: the kernel's into
// the title's buffers (ReadFile) and libraries' own (HolyBeforeWrite).
typedef void (*HolyWriteHook)(const void* address, size_t size);
HOLYRT_API void HolySetWriteHook(HolyWriteHook hook);
HOLYRT_API void HolyBeforeWrite(const void* address, size_t size);

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

// Non-zero when a local user is present: a profile of this host that is
// signed in to no service (holy run --user local).
HOLYRT_API int HolyHasLocalUser(void);

// Named process-wide counters for measurement and leak checks (live objects,
// bytes held, events), reported with the process's memory at a fixed interval
// and when the process ends. Returns the stable cell for `name` ("component.what"),
// created at zero on first use; update it with interlocked operations.
HOLYRT_API volatile int64_t* HolyCounter(const char* name);

// Logs the statistics report now.
HOLYRT_API void HolyLogStats(const char* reason);

// Non-zero if the runtime interposes `symbol` when a title imports it from a
// host system library. Lets tools predict binding without running anything.
HOLYRT_API int HolyHasOverride(const char* library, const char* symbol);

}  // extern "C"
