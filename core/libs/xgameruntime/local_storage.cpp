// Persistent local storage.
//
// Observed: interface {41a4e10c-5a7e-41d9-8c37-37bde62a07d6}, provider
// {f4faf4d4-2d04-4fce-b3e0-474a713a3e84}. Slots identified from their thunks
// and call sites against the documented XPersistentLocalStorage.h:
//
//   4 XPersistentLocalStorageGetPath(size, char* path, size_t* used)   size 256 observed
//   5 XPersistentLocalStorageGetSpaceInfo(XPersistentLocalStorageSpaceInfo*)
//
// The storage is the directory "persistent" in the title's data directory,
// which survives runs like the console's persistent local storage does. Its
// quota is the manifest's <PersistentLocalStorage><SizeMB>. Space info follows
// the documented struct: available (quota left, bounded by the disk's free
// space), free space on the disk, bytes in use, and the quota.
#include <windows.h>

#include <cstring>
#include <string>
#include <vector>

#include "core/libs/support.h"
#include "core/libs/xgameruntime/provider.h"
#include "core/libs/xgameruntime/services.h"
#include "core/package/manifest.h"

namespace holy::xgr {
namespace {

struct SpaceInfo {
  uint64_t available_free_bytes;
  uint64_t total_free_bytes;
  uint64_t used_bytes;
  uint64_t total_bytes;
};

std::wstring storage_dir() {
  const wchar_t* data = HolyDataDir();
  if (!data || !*data) return {};
  std::wstring dir = std::wstring(data) + L"\\persistent";
  CreateDirectoryW(dir.c_str(), nullptr);
  return dir;
}

uint64_t directory_size(const std::wstring& root) {
  uint64_t total = 0;
  std::vector<std::wstring> pending{root};
  while (!pending.empty()) {
    const std::wstring dir = pending.back();
    pending.pop_back();
    WIN32_FIND_DATAW entry;
    const HANDLE find = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &entry, FindExSearchNameMatch,
                                         nullptr, 0);
    if (find == INVALID_HANDLE_VALUE) continue;
    do {
      if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        if (std::wcscmp(entry.cFileName, L".") != 0 && std::wcscmp(entry.cFileName, L"..") != 0) {
          pending.push_back(dir + L"\\" + entry.cFileName);
        }
      } else {
        total += (static_cast<uint64_t>(entry.nFileSizeHigh) << 32) | entry.nFileSizeLow;
      }
    } while (FindNextFileW(find, &entry));
    FindClose(find);
  }
  return total;
}

uint64_t quota_bytes() {
  static const uint64_t quota = [] {
    const wchar_t* dir = HolyPackageDir();
    const auto manifest = dir && *dir ? package::load(dir) : std::nullopt;
    return manifest ? static_cast<uint64_t>(manifest->persistent_local_storage_mb) << 20 : 0;
  }();
  return quota;
}

HRESULT STDMETHODCALLTYPE get_path(Object*, size_t size, char* path, size_t* used) {
  if (!path) return E_INVALIDARG;
  const std::wstring dir = storage_dir();
  if (dir.empty()) return E_FAIL;
  const int needed = WideCharToMultiByte(CP_UTF8, 0, dir.c_str(), -1, nullptr, 0, nullptr, nullptr);
  if (needed <= 0) return E_FAIL;
  if (size < static_cast<size_t>(needed)) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
  WideCharToMultiByte(CP_UTF8, 0, dir.c_str(), -1, path, static_cast<int>(size), nullptr, nullptr);
  if (used) *used = static_cast<size_t>(needed);
  return S_OK;
}

HRESULT STDMETHODCALLTYPE get_space_info(Object*, SpaceInfo* info) {
  if (!info) return E_INVALIDARG;
  const std::wstring dir = storage_dir();
  ULARGE_INTEGER available{}, total{}, free{};
  if (dir.empty() || !GetDiskFreeSpaceExW(dir.c_str(), &available, &total, &free)) return E_FAIL;
  const uint64_t used = directory_size(dir);
  const uint64_t quota = quota_bytes();
  const uint64_t left = quota > used ? quota - used : 0;
  info->available_free_bytes = left < available.QuadPart ? left : available.QuadPart;
  info->total_free_bytes = available.QuadPart;
  info->used_bytes = used;
  info->total_bytes = quota;
  return S_OK;
}

}  // namespace

void register_local_storage() {
  // {41a4e10c-5a7e-41d9-8c37-37bde62a07d6}
  constexpr GUID kStorage = {0x41a4e10c, 0x5a7e, 0x41d9, {0x8c, 0x37, 0x37, 0xbd, 0xe6, 0x2a, 0x07, 0xd6}};
  Interface& storage = define_interface(kStorage, "persistent-local-storage");
  set_method(storage, 4, "XPersistentLocalStorageGetPath", &get_path);
  set_method(storage, 5, "XPersistentLocalStorageGetSpaceInfo", &get_space_info);
}

}  // namespace holy::xgr
