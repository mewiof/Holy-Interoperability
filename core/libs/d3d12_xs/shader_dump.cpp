// Saves shader blobs a title passes to pipeline creation into its data
// directory (shaders/<hash>.bin), for studying the console shader format.
// Never written anywhere else.
#include "core/libs/d3d12_xs/shader_dump.h"

#include <windows.h>

#include <cstdio>
#include <mutex>
#include <set>
#include <string>

#include "core/common/paths.h"
#include "core/libs/support.h"

namespace holy::d3d12 {

uint64_t shader_hash(const void* bytes, size_t size) {
  uint64_t hash = 0xcbf29ce484222325ull;
  const auto* data = static_cast<const uint8_t*>(bytes);
  for (size_t i = 0; i < size; ++i) hash = (hash ^ data[i]) * 0x100000001b3ull;
  return hash;
}

void discard_host_caches_after_run(long refusal) {
  if (refusal != E_OUTOFMEMORY) return;
  const wchar_t* data_dir = HolyDataDir();
  if (!data_dir || !*data_dir) return;
  const std::wstring path = std::wstring(data_dir) + L"\\" + kHostCacheDir + L"\\" + kHostCacheDiscard;
  const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
}

namespace {

std::mutex g_refused_lock;

std::wstring refused_list_path() {
  const wchar_t* data_dir = HolyDataDir();
  if (!data_dir || !*data_dir) return {};
  return std::wstring(data_dir) + L"\\refused-pipelines.txt";
}

std::set<uint64_t>& refused() {
  static std::set<uint64_t>* set = [] {
    auto* loaded = new std::set<uint64_t>();
    const std::wstring path = refused_list_path();
    FILE* file = path.empty() ? nullptr : _wfopen(path.c_str(), L"r");
    if (file) {
      unsigned long long hash = 0;
      while (std::fscanf(file, "%llx", &hash) == 1) loaded->insert(hash);
      std::fclose(file);
    }
    return loaded;
  }();
  return *set;
}

}  // namespace

bool host_refused_before(uint64_t hash) {
  std::lock_guard<std::mutex> guard(g_refused_lock);
  return refused().count(hash) != 0;
}

void remember_host_refusal(uint64_t hash) {
  std::lock_guard<std::mutex> guard(g_refused_lock);
  if (!refused().insert(hash).second) return;
  const std::wstring path = refused_list_path();
  FILE* file = path.empty() ? nullptr : _wfopen(path.c_str(), L"a");
  if (!file) return;
  std::fprintf(file, "%016llx\n", static_cast<unsigned long long>(hash));
  std::fclose(file);
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
