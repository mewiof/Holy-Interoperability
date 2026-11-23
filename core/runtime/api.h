// C interface that holyrt.dll exports to the console library implementations
// under core/libs and to the tools.
#pragma once

#include <cstdarg>
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

// Absolute host paths of the running title's package and data directories.
// Empty strings when the runtime was not booted by the launcher.
HOLYRT_API const wchar_t* HolyPackageDir(void);
HOLYRT_API const wchar_t* HolyDataDir(void);

// Non-zero if the runtime interposes `symbol` when a title imports it from a
// host system library. Lets tools predict binding without running anything.
HOLYRT_API int HolyHasOverride(const char* library, const char* symbol);

}  // extern "C"
