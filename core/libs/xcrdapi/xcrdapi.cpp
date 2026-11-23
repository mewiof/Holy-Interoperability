// xcrdapi.dll: the console's fast-storage API (a DirectStorage variant).
//
// Entry point (observed): DStorageGetFactory(REFIID, void**), the signature of
// the public desktop dstorage.h function of the same name. The console factory
// has its own interface id; its methods are COM-style.
//
// Factory slots (observed from the title's calls):
//    4 OpenFile(path, riid, void** file)
//    5 CreateStatusArray(uint32_t capacity, const char* name, riid, void** array)
//    8 called once with 32 MiB before any queue exists: the staging buffer size
//   10 CreateQueue(const QueueDesc*, riid, void** queue); the descriptor has the
//      layout of the public DSTORAGE_QUEUE_DESC (source type, capacity,
//      priority, name) followed by a field the title sets to 1
//
// Present state: work in progress. Unbound slots report themselves.
#include <windows.h>

#include <atomic>
#include <cstring>

#include "core/libs/support.h"
#include "core/libs/vtable.h"

namespace {

constexpr char kLibrary[] = "xcrdapi.dll";
constexpr int kSlots = 48;

struct Object {
  void* const* vtable;
  const holy::lib::VtableInfo* info;
};

HRESULT STDMETHODCALLTYPE query_interface(Object* self, REFIID, void** out) {
  if (!out) return E_POINTER;
  *out = self;
  return S_OK;
}
ULONG STDMETHODCALLTYPE add_ref(Object*) {
  return 2;
}
ULONG STDMETHODCALLTYPE release(Object*) {
  return 1;
}

template <int N>
std::array<void*, N> com_vtable() {
  auto table = holy::lib::make_reporting_vtable<N>();
  table[0] = reinterpret_cast<void*>(&query_interface);
  table[1] = reinterpret_cast<void*>(&add_ref);
  table[2] = reinterpret_cast<void*>(&release);
  return table;
}

// --- files ------------------------------------------------------------------------

const holy::lib::VtableInfo kFileInfo{kLibrary, "IDStorageFileX"};

struct File {
  void* const* vtable;
  const holy::lib::VtableInfo* info;
  std::atomic<long> refs{1};
  HANDLE handle = INVALID_HANDLE_VALUE;
};

ULONG STDMETHODCALLTYPE file_add_ref(File* self) {
  return static_cast<ULONG>(self->refs.fetch_add(1) + 1);
}
ULONG STDMETHODCALLTYPE file_release(File* self) {
  const long left = self->refs.fetch_sub(1) - 1;
  if (left == 0) {
    if (self->handle != INVALID_HANDLE_VALUE) CloseHandle(self->handle);
    delete self;
  }
  return static_cast<ULONG>(left);
}

const auto kFileVtable = [] {
  auto table = com_vtable<kSlots>();
  table[1] = reinterpret_cast<void*>(&file_add_ref);
  table[2] = reinterpret_cast<void*>(&file_release);
  return table;
}();

// Opens a package or scratch file for reading. Paths are the title's (mounted
// drives, package-relative) and are translated like every other file access.
HRESULT STDMETHODCALLTYPE open_file(Object*, const wchar_t* path, REFIID, void** file) {
  if (!file) return E_POINTER;
  *file = nullptr;
  if (!path) return E_INVALIDARG;
  wchar_t host[1024];
  if (!HolyTranslatePath(path, host, std::size(host))) return HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);
  const HANDLE handle = CreateFileW(host, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    const DWORD error = GetLastError();
    LIB_LOG(holy::lib::kDebug, "dstorage", "open failed (error %lu)", error);
    return HRESULT_FROM_WIN32(error);
  }
  auto* object = new File();
  object->vtable = kFileVtable.data();
  object->info = &kFileInfo;
  object->handle = handle;
  *file = object;
  return S_OK;
}

// --- factory ------------------------------------------------------------------

const holy::lib::VtableInfo kFactoryInfo{kLibrary, "IDStorageFactoryX"};

HRESULT STDMETHODCALLTYPE set_staging_buffer_size(Object*, uint32_t size) {
  LIB_LOG(holy::lib::kDebug, "dstorage", "staging buffer size 0x%x", size);
  return S_OK;
}

const auto kFactoryVtable = [] {
  auto table = com_vtable<kSlots>();
  table[4] = reinterpret_cast<void*>(&open_file);
  table[8] = reinterpret_cast<void*>(&set_staging_buffer_size);
  return table;
}();

Object g_factory{kFactoryVtable.data(), &kFactoryInfo};

}  // namespace

extern "C" __declspec(dllexport) HRESULT WINAPI DStorageGetFactory(REFIID riid, void** factory) {
  if (!factory) return E_POINTER;
  (void)riid;
  *factory = &g_factory;
  return S_OK;
}
