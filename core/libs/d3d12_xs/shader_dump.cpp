// Saves shader blobs a title passes to pipeline creation into its data
// directory (shaders/<hash>.bin), for studying the console shader format.
// Never written anywhere else.
#include "core/libs/d3d12_xs/shader_dump.h"

#include <windows.h>

#include <cstdio>
#include <string>

#include "core/libs/support.h"

namespace holy::d3d12 {

uint64_t shader_hash(const void* bytes, size_t size) {
  uint64_t hash = 0xcbf29ce484222325ull;
  const auto* data = static_cast<const uint8_t*>(bytes);
  for (size_t i = 0; i < size; ++i) hash = (hash ^ data[i]) * 0x100000001b3ull;
  return hash;
}

void dump_shader(const void* bytes, size_t size, const char* stage) {
  if (!bytes || !size) return;
  const wchar_t* data_dir = HolyDataDir();
  if (!data_dir || !*data_dir) return;
  const std::wstring dir = std::wstring(data_dir) + L"\\shaders";
  CreateDirectoryW(dir.c_str(), nullptr);
  wchar_t name[64];
  std::swprintf(name, 64, L"\\%016llx.bin", static_cast<unsigned long long>(shader_hash(bytes, size)));
  const std::wstring path = dir + name;
  const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return;  // already saved
  DWORD written = 0;
  WriteFile(file, bytes, static_cast<DWORD>(size), &written, nullptr);
  CloseHandle(file);
  LIB_LOG(lib::kDebug, "d3d12", "%s shader %016llx (%zu bytes) saved", stage,
          static_cast<unsigned long long>(shader_hash(bytes, size)), size);
}

}  // namespace holy::d3d12
