// Package service.
//
// Observed: interface {0b9d556e-ab70-43f4-baa0-9999ac4cb040}, provider
// {af406016-e850-4aa8-a88d-2f3dcb9dac7e}. Slots identified from their thunks
// and call sites against the documented XPackage.h declarations:
//
//   3 XPackageGetCurrentProcessPackageIdentifier(size, char* buffer)  size 33 observed
//   4 XPackageIsPackagedProcess() -> bool
//   5 XPackageCreateInstallationMonitor(id, selector_count, selectors, interval_ms, queue, monitor*)
//   6 XPackageCloseInstallationMonitorHandle(monitor)
//   7 XPackageGetInstallationProgress(monitor, XPackageInstallationProgress*)
//   9 XPackageRegisterInstallationProgressChanged(monitor, context, callback, token*)
//  39 XPackageEnumeratePackages(kind, scope, context, callback)
//
// Answers: the title runs from its package, which is completely present on
// disk, so the process is packaged and every installation monitor reports a
// finished installation whose size is the package's size on disk. Progress
// never changes, so progress callbacks never fire. The package identifier is
// this runtime's own opaque name for the package (derived from manifest ids);
// only the package functions consume it. Enumerating content packages finds
// none: the runtime never reports add-on content. Enumerating game packages
// is not implemented yet and reports itself.
#include <windows.h>
#include <intrin.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/libs/support.h"
#include "core/libs/xgameruntime/provider.h"
#include "core/libs/xgameruntime/services.h"
#include "core/libs/xgameruntime/task_queue.h"
#include "core/package/manifest.h"

namespace holy::xgr {
namespace {

constexpr uint32_t kMonitorSignature = 0x4E4F4D50;  // "PMON"

struct InstallationProgress {
  uint64_t total_bytes;
  uint64_t installed_bytes;
  uint64_t launch_bytes;
  bool launchable;
  bool completed;
};

struct Monitor {
  uint32_t signature = kMonitorSignature;
};

std::string package_identifier() {
  const wchar_t* dir = HolyPackageDir();
  const auto manifest = dir && *dir ? package::load(dir) : std::nullopt;
  std::string key = manifest ? manifest->storage_key() : std::string("package");
  if (key.size() > 32) key.resize(32);
  return key;
}

// Sum of the sizes of all files under the package directory.
uint64_t package_size() {
  static const uint64_t size = [] {
    uint64_t total = 0;
    std::vector<std::wstring> pending;
    const wchar_t* root = HolyPackageDir();
    if (!root || !*root) return total;
    pending.emplace_back(root);
    while (!pending.empty()) {
      const std::wstring dir = pending.back();
      pending.pop_back();
      WIN32_FIND_DATAW entry;
      const HANDLE find = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &entry, FindExSearchNameMatch,
                                           nullptr, FIND_FIRST_EX_LARGE_FETCH);
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
  }();
  return size;
}

HRESULT STDMETHODCALLTYPE get_current_process_package_identifier(Object*, size_t size, char* buffer) {
  if (!buffer) return E_INVALIDARG;
  const std::string id = package_identifier();
  if (size < id.size() + 1) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
  std::memcpy(buffer, id.c_str(), id.size() + 1);
  return S_OK;
}

bool STDMETHODCALLTYPE is_packaged_process(Object*) {
  return true;
}

HRESULT STDMETHODCALLTYPE create_installation_monitor(Object*, const char* identifier, uint32_t, const void*,
                                                      uint32_t, void*, void** monitor) {
  if (!monitor) return E_INVALIDARG;
  *monitor = nullptr;
  if (identifier && package_identifier() != identifier) return E_INVALIDARG;
  *monitor = new Monitor();
  return S_OK;
}

void STDMETHODCALLTYPE close_installation_monitor(Object*, void* monitor) {
  auto* state = static_cast<Monitor*>(monitor);
  if (state && state->signature == kMonitorSignature) {
    state->signature = 0;
    delete state;
  }
}

void STDMETHODCALLTYPE get_installation_progress(Object*, void*, InstallationProgress* progress) {
  if (!progress) return;
  const uint64_t size = package_size();
  *progress = InstallationProgress{size, size, size, true, true};
}

std::atomic<uint64_t> g_next_token{1};

HRESULT STDMETHODCALLTYPE register_installation_progress_changed(Object*, void* monitor, void*, void* callback,
                                                                 RegistrationToken* token) {
  if (!monitor || !callback || !token) return E_INVALIDARG;
  token->token = g_next_token.fetch_add(1);
  return S_OK;
}

HRESULT STDMETHODCALLTYPE enumerate_packages(Object*, uint32_t kind, uint32_t scope, void*, void* callback) {
  constexpr uint32_t kKindContent = 1;
  if (!callback) return E_INVALIDARG;
  if (kind == kKindContent) {
    LIB_LOG(holy::lib::kDebug, "xpackage", "content package enumeration (scope %u): none", scope);
    return S_OK;
  }
  HolyUnimplementedFrom("xgameruntime.dll", "XPackageEnumeratePackages(game)", _ReturnAddress());
  return E_NOTIMPL;
}

}  // namespace

void register_package() {
  // {0b9d556e-ab70-43f4-baa0-9999ac4cb040}
  constexpr GUID kPackage = {0x0b9d556e, 0xab70, 0x43f4, {0xba, 0xa0, 0x99, 0x99, 0xac, 0x4c, 0xb0, 0x40}};
  Interface& package = define_interface(kPackage, "package");
  set_method(package, 3, "XPackageGetCurrentProcessPackageIdentifier", &get_current_process_package_identifier);
  set_method(package, 4, "XPackageIsPackagedProcess", &is_packaged_process);
  set_method(package, 5, "XPackageCreateInstallationMonitor", &create_installation_monitor);
  set_method(package, 6, "XPackageCloseInstallationMonitorHandle", &close_installation_monitor);
  set_method(package, 7, "XPackageGetInstallationProgress", &get_installation_progress);
  set_method(package, 9, "XPackageRegisterInstallationProgressChanged", &register_installation_progress_changed);
  set_method(package, 39, "XPackageEnumeratePackages", &enumerate_packages);
}

}  // namespace holy::xgr
