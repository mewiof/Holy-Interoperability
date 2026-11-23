// Contract between the launcher and the in-process runtime.
//
// The launcher creates the title process suspended and, before the OS loader
// has looked at the image, replaces the title's import directory (in memory
// only) with a table that imports a single symbol from the runtime DLL. The
// replacement table is preceded by a BootBlock describing where the original
// import directory lives, so the runtime can bind it itself.
//
// Memory layout of the block the launcher writes into the title process:
//
//   +0                      BootBlock
//   +kBootDescriptorOffset  IMAGE_IMPORT_DESCRIPTOR[2]   <- import directory points here
//   ...                     lookup table, address table, hint/name, DLL path
#pragma once

#include <cstddef>
#include <cstdint>

namespace holy {

inline constexpr uint64_t kBootMagic = 0x544F4F42594C4F48ull;  // "HOLYBOOT"
inline constexpr uint32_t kBootVersion = 3;

// Console model the title is told it runs on. The values are the ones the
// console OS publishes in its "ConsoleMode" registry value.
enum class ConsoleModel : uint32_t {
  SeriesS = 0x60,
  SeriesX = 0x64,
};
inline constexpr char kBootAnchorSymbol[] = "HolyRuntimeAnchor";

enum BootFlags : uint32_t {
  kBootTrapUnimplemented = 1u << 0,  // terminate on the first call to an unimplemented import
  kBootSandboxed = 1u << 1,          // informational: the process runs at low integrity
  kBootOffline = 1u << 2,            // the title sees a host without network connectivity
};

struct BootBlock {
  uint64_t magic;
  uint32_t version;
  uint32_t struct_size;
  uint32_t original_import_rva;
  uint32_t original_import_size;
  uint32_t original_iat_rva;
  uint32_t original_iat_size;
  uint32_t flags;
  uint32_t log_level;
  uint32_t console_model;  // ConsoleModel
  uint32_t reserved;
  wchar_t package_dir[520];
  wchar_t data_dir[520];
  wchar_t trace_filter[256];  // comma separated wildcard patterns of import library names
};

inline constexpr size_t kBootDescriptorOffset = (sizeof(BootBlock) + 15) & ~static_cast<size_t>(15);

// Process exit codes used by the runtime for conditions it detects itself.
inline constexpr uint32_t kExitUnimplementedImport = 0xE0484F01;
inline constexpr uint32_t kExitRuntimeFailure = 0xE0484F02;

}  // namespace holy
