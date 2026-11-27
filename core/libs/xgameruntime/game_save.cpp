// Game saves.
//
// Observed: interface {1bfff3af-f14a-40a3-8e35-9ada906593f9}, provider
// {704c3f58-e629-4cc2-b197-30511b996fe2}. The slots follow the documented
// XGameSave.h declarations in order: every slot the reference title's client
// library calls (4-6, 8, 9, 11-14, 16, 17, 19-25, 28, 29) has a thunk whose
// argument count and widths match the declaration (slot 21's count is 32
// bits, slot 4's flag 8), and the others fall between them:
//
//    3 XGameSaveInitializeProvider(user, configurationId, bool syncOnDemand, provider*)
//    4 XGameSaveInitializeProviderAsync(user, configurationId, bool syncOnDemand, async)
//    5 XGameSaveInitializeProviderResult(async, provider*)
//    6 XGameSaveCloseProvider(provider)
//    7 XGameSaveGetRemainingQuota(provider, int64_t*)
//    8 XGameSaveGetRemainingQuotaAsync(provider, async)
//    9 XGameSaveGetRemainingQuotaResult(async, int64_t*)
//   10 XGameSaveDeleteContainer(provider, name)
//   11 XGameSaveDeleteContainerAsync(provider, name, async)
//   12 XGameSaveDeleteContainerResult(async)
//   13 XGameSaveGetContainerInfo(provider, name, context, callback)
//   14 XGameSaveEnumerateContainerInfo(provider, context, callback)
//   15 XGameSaveEnumerateContainerInfoByName(provider, prefix, context, callback)
//   16 XGameSaveCreateContainer(provider, name, container*)
//   17 XGameSaveCloseContainer(container)
//   18 XGameSaveEnumerateBlobInfo(container, context, callback)
//   19 XGameSaveEnumerateBlobInfoByName(container, prefix, context, callback)
//   20 XGameSaveReadBlobData(container, names, uint32_t* count, size_t size, XGameSaveBlob*)
//   21 XGameSaveReadBlobDataAsync(container, names, uint32_t count, async)
//   22 XGameSaveReadBlobDataResult(async, size_t size, XGameSaveBlob*, uint32_t* count)
//   23 XGameSaveCreateUpdate(container, displayName, update*)
//   24 XGameSaveCloseUpdate(update)
//   25 XGameSaveSubmitBlobWrite(update, name, data, size)
//   26 XGameSaveSubmitBlobDelete(update, name)
//   27 XGameSaveSubmitUpdate(update)
//   28 XGameSaveSubmitUpdateAsync(update, async)
//   29 XGameSaveSubmitUpdateResult(async)
//
// Local equivalent: saves are files of the host, in the title's data
// directory: saves\<user id>\<configuration id>\<container>\<blob> (names
// encoded so that any name is a file name, case kept apart), a container's
// display name in a file of its own. Nothing is synchronised with a service:
// containers never need syncing. An update's writes go to temporary files that
// replace the blobs when it is submitted, so a blob is never half written;
// an update interrupted between blobs leaves some written (partial: no
// journal). Reads lay blobs out as documented: the XGameSaveBlob array, then
// each blob's data and name, the size titles compute from the blob infos.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <ctime>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "core/libs/support.h"
#include "core/libs/xgameruntime/provider.h"
#include "core/libs/xgameruntime/services.h"
#include "core/libs/xgameruntime/task_queue.h"

namespace holy::xgr {
namespace {

// Documented XGameSave error codes.
constexpr HRESULT kInvalidContainerName = static_cast<HRESULT>(0x80830001);  // E_GS_INVALID_CONTAINER_NAME
constexpr HRESULT kOutOfLocalStorage = static_cast<HRESULT>(0x80830003);     // E_GS_OUT_OF_LOCAL_STORAGE
constexpr HRESULT kQuotaExceeded = static_cast<HRESULT>(0x80830006);         // E_GS_QUOTA_EXCEEDED
constexpr HRESULT kBufferTooSmall = static_cast<HRESULT>(0x80830007);        // E_GS_PROVIDED_BUFFER_TOO_SMALL
constexpr HRESULT kBlobNotFound = static_cast<HRESULT>(0x80830008);          // E_GS_BLOB_NOT_FOUND

// The quota a configuration's saves have (assumed: the service's usual
// allowance; nothing enforces it but this runtime).
constexpr int64_t kQuotaBytes = 256ll << 20;

// Documented structures (x64 layouts).
struct ContainerInfo {
  const char* name;
  const char* display_name;
  uint32_t blob_count;
  uint64_t total_size;
  int64_t last_modified;  // time_t
  bool needs_sync;
};
struct BlobInfo {
  const char* name;
  uint32_t size;
};
struct Blob {
  BlobInfo info;
  uint8_t* data;
};
static_assert(sizeof(ContainerInfo) == 48 && sizeof(BlobInfo) == 16 && sizeof(Blob) == 24);
using ContainerCallback = bool(CALLBACK*)(const ContainerInfo* info, void* context);
using BlobCallback = bool(CALLBACK*)(const BlobInfo* info, void* context);

std::mutex g_lock;  // file operations of all providers

// --- names and paths -------------------------------------------------------------------

// A name as a file name: lower-case letters, digits, '-', '_' and '.' stay;
// an upper-case letter becomes '^' and the letter in lower case (the host's
// names ignore case, saves' do not); every other byte becomes %XX. A prefix
// keeps names clear of the host's reserved device names.
std::wstring encode(const std::string& name, wchar_t kind) {
  std::wstring out{kind, L'.'};
  for (unsigned char c : name) {
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.') {
      out += static_cast<wchar_t>(c);
    } else if (c >= 'A' && c <= 'Z') {
      out += L'^';
      out += static_cast<wchar_t>(c - 'A' + 'a');
    } else {
      wchar_t hex[4];
      swprintf(hex, 4, L"%%%02X", c);
      out += hex;
    }
  }
  return out;
}

bool decode(const std::wstring& file, wchar_t kind, std::string* name) {
  if (file.size() < 2 || file[0] != kind || file[1] != L'.') return false;
  name->clear();
  for (size_t i = 2; i < file.size(); ++i) {
    const wchar_t c = file[i];
    if (c == L'^' && i + 1 < file.size()) {
      *name += static_cast<char>(file[++i] - L'a' + 'A');
    } else if (c == L'%' && i + 2 < file.size()) {
      *name += static_cast<char>(std::wcstoul(file.substr(i + 1, 2).c_str(), nullptr, 16));
      i += 2;
    } else {
      *name += static_cast<char>(c);
    }
  }
  return true;
}

// Documented: container names are up to 256 characters; blob names up to 64.
bool valid_name(const char* name, size_t limit) {
  if (!name || !*name) return false;
  const size_t length = strnlen(name, limit + 1);
  return length <= limit;
}

void make_directories(const std::wstring& path) {
  for (size_t at = path.find(L'\\', 7); at != std::wstring::npos; at = path.find(L'\\', at + 1)) {
    CreateDirectoryW(path.substr(0, at).c_str(), nullptr);
  }
  CreateDirectoryW(path.c_str(), nullptr);
}

struct FileEntry {
  std::wstring file;
  uint64_t size = 0;
  int64_t modified = 0;  // time_t
  bool directory = false;
};

int64_t to_time_t(const FILETIME& time) {
  const uint64_t ticks = (static_cast<uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
  return static_cast<int64_t>(ticks / 10000000ull) - 11644473600ll;
}

std::vector<FileEntry> list(const std::wstring& dir) {
  std::vector<FileEntry> entries;
  WIN32_FIND_DATAW data;
  const HANDLE find =
      FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, 0);
  if (find == INVALID_HANDLE_VALUE) return entries;
  do {
    if (!std::wcscmp(data.cFileName, L".") || !std::wcscmp(data.cFileName, L"..")) continue;
    FileEntry e;
    e.file = data.cFileName;
    e.size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    e.modified = to_time_t(data.ftLastWriteTime);
    e.directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    entries.push_back(std::move(e));
  } while (FindNextFileW(find, &data));
  FindClose(find);
  return entries;
}

bool read_file(const std::wstring& path, std::vector<uint8_t>* out) {
  const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                  FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER size{};
  bool ok = GetFileSizeEx(file, &size) && size.QuadPart < (1ll << 31);
  if (ok) {
    out->resize(static_cast<size_t>(size.QuadPart));
    DWORD got = 0;
    ok = out->empty() || (ReadFile(file, out->data(), static_cast<DWORD>(out->size()), &got, nullptr) &&
                          got == out->size());
  }
  CloseHandle(file);
  return ok;
}

bool write_file(const std::wstring& path, const void* data, size_t size) {
  const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  DWORD wrote = 0;
  const bool ok = !size || (WriteFile(file, data, static_cast<DWORD>(size), &wrote, nullptr) && wrote == size);
  const bool flushed = ok && FlushFileBuffers(file);
  CloseHandle(file);
  return ok && flushed;
}

// --- providers, containers, updates -----------------------------------------------------

struct Provider {
  uint32_t magic = 0x56415350;  // "PSAV"
  std::wstring dir;             // \\?\<data>\saves\<user>\<configuration>
};
struct Container {
  uint32_t magic = 0x544E4F43;  // "CONT"
  Provider* provider = nullptr;
  std::string name;
  std::wstring dir;
};
struct Update {
  uint32_t magic = 0x54445055;  // "UPDT"
  Container* container = nullptr;
  std::string display_name;
  std::vector<std::pair<std::string, std::vector<uint8_t>>> writes;
  std::vector<std::string> deletes;
};

std::mutex g_handles_lock;
std::unordered_set<void*>& handles() {
  static auto* set = new std::unordered_set<void*>();
  return *set;
}
template <typename T>
T* handle_of(void* handle, uint32_t magic) {
  std::lock_guard<std::mutex> guard(g_handles_lock);
  if (!handle || !handles().count(handle)) return nullptr;
  auto* object = static_cast<T*>(handle);
  return object->magic == magic ? object : nullptr;
}
template <typename T>
T* track(T* object) {
  std::lock_guard<std::mutex> guard(g_handles_lock);
  handles().insert(object);
  return object;
}
template <typename T>
void untrack(T* object) {
  {
    std::lock_guard<std::mutex> guard(g_handles_lock);
    if (!handles().erase(object)) return;
  }
  delete object;
}

Provider* provider_of(void* h) { return handle_of<Provider>(h, 0x56415350); }
Container* container_of(void* h) { return handle_of<Container>(h, 0x544E4F43); }
Update* update_of(void* h) { return handle_of<Update>(h, 0x54445055); }

HRESULT open_provider(void* user, const char* configuration, Provider** out) {
  *out = nullptr;
  uint64_t id = 0;
  if (!user || !configuration || !local_user_id(user, &id)) return E_INVALIDARG;
  const wchar_t* data = HolyDataDir();
  if (!data || !*data) return E_FAIL;
  wchar_t user_dir[24];
  swprintf(user_dir, 24, L"%016llx", static_cast<unsigned long long>(id));
  auto* provider = new Provider();
  provider->dir = std::wstring(L"\\\\?\\") + data + L"\\saves\\" + user_dir + L"\\" + encode(configuration, L'g');
  make_directories(provider->dir);
  LIB_LOG(holy::lib::kInfo, "gamesave", "saves of the local user (configuration %s) are in <data>\\saves", configuration);
  *out = track(provider);
  return S_OK;
}

uint64_t used_bytes(const Provider& provider) {
  uint64_t total = 0;
  for (const FileEntry& c : list(provider.dir)) {
    if (!c.directory) continue;
    for (const FileEntry& b : list(provider.dir + L"\\" + c.file)) total += b.size;
  }
  return total;
}

struct ContainerData {
  std::string name, display_name;
  uint32_t blobs = 0;
  uint64_t size = 0;
  int64_t modified = 0;
};

bool container_data(const Provider& provider, const std::wstring& file, ContainerData* out) {
  if (!decode(file, L'c', &out->name)) return false;
  out->display_name = out->name;
  out->blobs = 0;
  out->size = 0;
  out->modified = 0;
  for (const FileEntry& e : list(provider.dir + L"\\" + file)) {
    std::string blob;
    if (e.file == L"display") {
      std::vector<uint8_t> text;
      if (read_file(provider.dir + L"\\" + file + L"\\display", &text)) out->display_name.assign(text.begin(), text.end());
    } else if (decode(e.file, L'b', &blob)) {
      ++out->blobs;
      out->size += e.size;
      out->modified = std::max(out->modified, e.modified);
    }
  }
  return true;
}

// Calls `callback` for the containers whose names start with `prefix` (null:
// every container) until it returns false.
HRESULT enumerate_containers(Provider* provider, const char* prefix, void* context, ContainerCallback callback) {
  if (!provider || !callback) return E_INVALIDARG;
  std::vector<ContainerData> found;
  {
    std::lock_guard<std::mutex> guard(g_lock);
    for (const FileEntry& e : list(provider->dir)) {
      ContainerData data;
      if (!e.directory || !container_data(*provider, e.file, &data)) continue;
      if (prefix && data.name.compare(0, std::strlen(prefix), prefix) != 0) continue;
      found.push_back(std::move(data));
    }
  }
  std::sort(found.begin(), found.end(), [](const ContainerData& a, const ContainerData& b) { return a.name < b.name; });
  for (const ContainerData& data : found) {
    const ContainerInfo info{data.name.c_str(), data.display_name.c_str(), data.blobs, data.size, data.modified, false};
    if (!callback(&info, context)) break;
  }
  return S_OK;
}

struct BlobData {
  std::string name;
  std::vector<uint8_t> bytes;
};

// The blobs named (all of the container's when `names` is null), read.
HRESULT read_blobs(Container* container, const char** names, uint32_t count, std::vector<BlobData>* out) {
  std::lock_guard<std::mutex> guard(g_lock);
  out->clear();
  if (!names) {
    for (const FileEntry& e : list(container->dir)) {
      BlobData blob;
      if (e.directory || !decode(e.file, L'b', &blob.name)) continue;
      if (!read_file(container->dir + L"\\" + e.file, &blob.bytes)) return E_FAIL;
      out->push_back(std::move(blob));
    }
    std::sort(out->begin(), out->end(), [](const BlobData& a, const BlobData& b) { return a.name < b.name; });
    return S_OK;
  }
  for (uint32_t i = 0; i < count; ++i) {
    if (!names[i]) return E_INVALIDARG;
    BlobData blob;
    blob.name = names[i];
    if (!read_file(container->dir + L"\\" + encode(blob.name, L'b'), &blob.bytes)) return kBlobNotFound;
    out->push_back(std::move(blob));
  }
  return S_OK;
}

// Bytes the documented layout of `blobs` takes: the XGameSaveBlob array, then
// each blob's data and name.
size_t layout_size(const std::vector<BlobData>& blobs) {
  size_t size = blobs.size() * sizeof(Blob);
  for (const BlobData& b : blobs) size += b.bytes.size() + b.name.size() + 1;
  return size;
}

void lay_out(const std::vector<BlobData>& blobs, Blob* out) {
  auto* at = reinterpret_cast<uint8_t*>(out + blobs.size());
  for (size_t i = 0; i < blobs.size(); ++i) {
    const BlobData& b = blobs[i];
    out[i].data = at;
    if (!b.bytes.empty()) std::memcpy(at, b.bytes.data(), b.bytes.size());
    at += b.bytes.size();
    out[i].info.name = reinterpret_cast<const char*>(at);
    std::memcpy(at, b.name.c_str(), b.name.size() + 1);
    at += b.name.size() + 1;
    out[i].info.size = static_cast<uint32_t>(b.bytes.size());
  }
}

HRESULT submit(Update* update) {
  std::lock_guard<std::mutex> guard(g_lock);
  Container* container = update->container;
  uint64_t growth = 0;
  for (const auto& [name, bytes] : update->writes) growth += bytes.size();
  if (static_cast<int64_t>(used_bytes(*container->provider) + growth) > kQuotaBytes) return kQuotaExceeded;
  make_directories(container->dir);
  // Every write to a temporary file first, then the files replace the blobs.
  std::vector<std::pair<std::wstring, std::wstring>> renames;
  for (size_t i = 0; i < update->writes.size(); ++i) {
    const auto& [name, bytes] = update->writes[i];
    const std::wstring target = container->dir + L"\\" + encode(name, L'b');
    const std::wstring temporary = container->dir + L"\\pending-" + std::to_wstring(i);
    if (!write_file(temporary, bytes.data(), bytes.size())) {
      for (const auto& r : renames) DeleteFileW(r.first.c_str());
      DeleteFileW(temporary.c_str());
      return kOutOfLocalStorage;
    }
    renames.emplace_back(temporary, target);
  }
  for (const auto& [from, to] : renames) {
    if (!MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return E_FAIL;
  }
  for (const std::string& name : update->deletes) DeleteFileW((container->dir + L"\\" + encode(name, L'b')).c_str());
  if (!update->display_name.empty()) {
    write_file(container->dir + L"\\display", update->display_name.data(), update->display_name.size());
  }
  LIB_LOG(holy::lib::kInfo, "gamesave", "container %s updated: %zu blobs written, %zu deleted",
          container->name.c_str(), update->writes.size(), update->deletes.size());
  return S_OK;
}

// --- asynchronous calls ---------------------------------------------------------------

// An asynchronous call carried out on the task queue: `work` produces the
// result bytes, copied out when the title asks for the result.
struct AsyncCall {
  std::function<HRESULT(std::vector<uint8_t>*)> work;
  std::vector<uint8_t> result;
};

HRESULT CALLBACK async_call_provider(AsyncOp op, const AsyncProviderData* data) {
  auto* call = static_cast<AsyncCall*>(data->context);
  switch (op) {
    case AsyncOp::Begin:
      return async_schedule(data->async, 0);
    case AsyncOp::DoWork: {
      const HRESULT hr = call->work(&call->result);
      if (FAILED(hr)) return hr;
      async_complete(data->async, hr, call->result.size());
      return E_PENDING;
    }
    case AsyncOp::GetResult:
      if (data->buffer_size < call->result.size() || (!data->buffer && !call->result.empty())) return kBufferTooSmall;
      if (!call->result.empty()) std::memcpy(data->buffer, call->result.data(), call->result.size());
      return S_OK;
    case AsyncOp::Cleanup:
      delete call;
      return S_OK;
    default:
      return S_OK;
  }
}

HRESULT begin(AsyncBlock* block, const void* identity, const char* name,
              std::function<HRESULT(std::vector<uint8_t>*)> work) {
  auto* call = new AsyncCall{std::move(work), {}};
  const HRESULT hr = async_begin(block, call, identity, name, &async_call_provider);
  if (FAILED(hr)) delete call;
  return hr;
}

template <typename T>
std::vector<uint8_t> bytes_of(const T& value) {
  std::vector<uint8_t> out(sizeof(T));
  std::memcpy(out.data(), &value, sizeof(T));
  return out;
}

const char kInitializeIdentity = 0, kQuotaIdentity = 0, kDeleteIdentity = 0, kReadIdentity = 0, kSubmitIdentity = 0;

// --- methods ----------------------------------------------------------------------------

HRESULT STDMETHODCALLTYPE initialize_provider(Object*, void* user, const char* configuration, bool, void** provider) {
  if (!provider) return E_INVALIDARG;
  Provider* made = nullptr;
  const HRESULT hr = open_provider(user, configuration, &made);
  *provider = made;
  return hr;
}

HRESULT STDMETHODCALLTYPE initialize_provider_async(Object*, void* user, const char* configuration, bool,
                                                    AsyncBlock* block) {
  const std::string config = configuration ? configuration : "";
  const bool has_configuration = configuration != nullptr;
  return begin(block, &kInitializeIdentity, "XGameSaveInitializeProviderAsync",
               [user, config, has_configuration](std::vector<uint8_t>* result) {
                 Provider* made = nullptr;
                 const HRESULT hr = open_provider(user, has_configuration ? config.c_str() : nullptr, &made);
                 if (SUCCEEDED(hr)) *result = bytes_of(static_cast<void*>(made));
                 return hr;
               });
}

HRESULT STDMETHODCALLTYPE initialize_provider_result(Object*, AsyncBlock* block, void** provider) {
  if (!provider) return E_INVALIDARG;
  *provider = nullptr;
  return async_get_result(block, &kInitializeIdentity, sizeof(void*), provider, nullptr);
}

void STDMETHODCALLTYPE close_provider(Object*, void* handle) {
  if (Provider* provider = provider_of(handle)) untrack(provider);
}

HRESULT remaining_quota(Provider* provider, int64_t* remaining) {
  if (!provider || !remaining) return E_INVALIDARG;
  std::lock_guard<std::mutex> guard(g_lock);
  *remaining = std::max<int64_t>(0, kQuotaBytes - static_cast<int64_t>(used_bytes(*provider)));
  return S_OK;
}

HRESULT STDMETHODCALLTYPE get_remaining_quota(Object*, void* provider, int64_t* remaining) {
  return remaining_quota(provider_of(provider), remaining);
}

HRESULT STDMETHODCALLTYPE get_remaining_quota_async(Object*, void* handle, AsyncBlock* block) {
  Provider* provider = provider_of(handle);
  if (!provider) return E_INVALIDARG;
  return begin(block, &kQuotaIdentity, "XGameSaveGetRemainingQuotaAsync", [provider](std::vector<uint8_t>* result) {
    int64_t remaining = 0;
    const HRESULT hr = remaining_quota(provider, &remaining);
    if (SUCCEEDED(hr)) *result = bytes_of(remaining);
    return hr;
  });
}

HRESULT STDMETHODCALLTYPE get_remaining_quota_result(Object*, AsyncBlock* block, int64_t* remaining) {
  if (!remaining) return E_INVALIDARG;
  return async_get_result(block, &kQuotaIdentity, sizeof(int64_t), remaining, nullptr);
}

HRESULT delete_container(Provider* provider, const std::string& name) {
  if (!provider) return E_INVALIDARG;
  std::lock_guard<std::mutex> guard(g_lock);
  const std::wstring dir = provider->dir + L"\\" + encode(name, L'c');
  for (const FileEntry& e : list(dir)) DeleteFileW((dir + L"\\" + e.file).c_str());
  RemoveDirectoryW(dir.c_str());
  LIB_LOG(holy::lib::kInfo, "gamesave", "container %s deleted", name.c_str());
  return S_OK;
}

HRESULT STDMETHODCALLTYPE delete_container_sync(Object*, void* provider, const char* name) {
  if (!valid_name(name, 256)) return kInvalidContainerName;
  return delete_container(provider_of(provider), name);
}

HRESULT STDMETHODCALLTYPE delete_container_async(Object*, void* handle, const char* name, AsyncBlock* block) {
  Provider* provider = provider_of(handle);
  if (!provider) return E_INVALIDARG;
  if (!valid_name(name, 256)) return kInvalidContainerName;
  const std::string container = name;
  return begin(block, &kDeleteIdentity, "XGameSaveDeleteContainerAsync",
               [provider, container](std::vector<uint8_t>*) { return delete_container(provider, container); });
}

HRESULT STDMETHODCALLTYPE delete_container_result(Object*, AsyncBlock* block) {
  return async_get_result(block, &kDeleteIdentity, 0, nullptr, nullptr);
}

HRESULT STDMETHODCALLTYPE get_container_info(Object*, void* handle, const char* name, void* context,
                                             ContainerCallback callback) {
  Provider* provider = provider_of(handle);
  if (!provider || !callback) return E_INVALIDARG;
  if (!valid_name(name, 256)) return kInvalidContainerName;
  ContainerData data;
  {
    std::lock_guard<std::mutex> guard(g_lock);
    const std::wstring file = encode(name, L'c');
    if (GetFileAttributesW((provider->dir + L"\\" + file).c_str()) == INVALID_FILE_ATTRIBUTES ||
        !container_data(*provider, file, &data)) {
      return S_OK;  // no such container: the callback is not called
    }
  }
  const ContainerInfo info{data.name.c_str(), data.display_name.c_str(), data.blobs, data.size, data.modified, false};
  callback(&info, context);
  return S_OK;
}

HRESULT STDMETHODCALLTYPE enumerate_container_info(Object*, void* provider, void* context, ContainerCallback callback) {
  return enumerate_containers(provider_of(provider), nullptr, context, callback);
}

HRESULT STDMETHODCALLTYPE enumerate_container_info_by_name(Object*, void* provider, const char* prefix, void* context,
                                                           ContainerCallback callback) {
  return enumerate_containers(provider_of(provider), prefix ? prefix : "", context, callback);
}

HRESULT STDMETHODCALLTYPE create_container(Object*, void* handle, const char* name, void** container) {
  Provider* provider = provider_of(handle);
  if (!provider || !container) return E_INVALIDARG;
  *container = nullptr;
  if (!valid_name(name, 256)) return kInvalidContainerName;
  auto* made = new Container();
  made->provider = provider;
  made->name = name;
  made->dir = provider->dir + L"\\" + encode(name, L'c');
  *container = track(made);
  return S_OK;
}

void STDMETHODCALLTYPE close_container(Object*, void* handle) {
  if (Container* container = container_of(handle)) untrack(container);
}

HRESULT enumerate_blobs(Container* container, const char* prefix, void* context, BlobCallback callback) {
  if (!container || !callback) return E_INVALIDARG;
  std::vector<std::pair<std::string, uint32_t>> found;
  {
    std::lock_guard<std::mutex> guard(g_lock);
    for (const FileEntry& e : list(container->dir)) {
      std::string name;
      if (e.directory || !decode(e.file, L'b', &name)) continue;
      if (prefix && name.compare(0, std::strlen(prefix), prefix) != 0) continue;
      found.emplace_back(std::move(name), static_cast<uint32_t>(e.size));
    }
  }
  std::sort(found.begin(), found.end());
  for (const auto& [name, size] : found) {
    const BlobInfo info{name.c_str(), size};
    if (!callback(&info, context)) break;
  }
  return S_OK;
}

HRESULT STDMETHODCALLTYPE enumerate_blob_info(Object*, void* container, void* context, BlobCallback callback) {
  return enumerate_blobs(container_of(container), nullptr, context, callback);
}

HRESULT STDMETHODCALLTYPE enumerate_blob_info_by_name(Object*, void* container, const char* prefix, void* context,
                                                      BlobCallback callback) {
  return enumerate_blobs(container_of(container), prefix ? prefix : "", context, callback);
}

HRESULT STDMETHODCALLTYPE read_blob_data(Object*, void* handle, const char** names, uint32_t* count, size_t size,
                                         Blob* blobs) {
  Container* container = container_of(handle);
  if (!container || !count || !blobs) return E_INVALIDARG;
  std::vector<BlobData> read;
  const HRESULT hr = read_blobs(container, names, *count, &read);
  if (FAILED(hr)) return hr;
  if (layout_size(read) > size) return kBufferTooSmall;
  lay_out(read, blobs);
  *count = static_cast<uint32_t>(read.size());
  return S_OK;
}

// The result of an asynchronous read: the blob count, then the blobs laid out
// as if at address 0 (pointers fixed up when copied to the title's buffer).
HRESULT STDMETHODCALLTYPE read_blob_data_async(Object*, void* handle, const char** names, uint32_t count,
                                               AsyncBlock* block) {
  Container* container = container_of(handle);
  if (!container) return E_INVALIDARG;
  std::vector<std::string> wanted;
  if (names) {
    for (uint32_t i = 0; i < count; ++i) {
      if (!names[i]) return E_INVALIDARG;
      wanted.emplace_back(names[i]);
    }
  }
  const bool all = names == nullptr;
  return begin(block, &kReadIdentity, "XGameSaveReadBlobDataAsync",
               [container, wanted, all](std::vector<uint8_t>* result) {
                 std::vector<const char*> pointers;
                 for (const std::string& name : wanted) pointers.push_back(name.c_str());
                 std::vector<BlobData> read;
                 const HRESULT hr = read_blobs(container, all ? nullptr : pointers.data(),
                                               static_cast<uint32_t>(pointers.size()), &read);
                 if (FAILED(hr)) return hr;
                 result->assign(layout_size(read), 0);
                 lay_out(read, reinterpret_cast<Blob*>(result->data()));
                 // Pointers relative to the start, and the count in front.
                 auto* laid = reinterpret_cast<Blob*>(result->data());
                 for (size_t i = 0; i < read.size(); ++i) {
                   laid[i].data = reinterpret_cast<uint8_t*>(laid[i].data - result->data());
                   laid[i].info.name = reinterpret_cast<const char*>(
                       reinterpret_cast<const uint8_t*>(laid[i].info.name) - result->data());
                 }
                 const uint32_t n = static_cast<uint32_t>(read.size());
                 result->insert(result->begin(), reinterpret_cast<const uint8_t*>(&n),
                                reinterpret_cast<const uint8_t*>(&n) + sizeof(n));
                 return S_OK;
               });
}

HRESULT STDMETHODCALLTYPE read_blob_data_result(Object*, AsyncBlock* block, size_t size, Blob* blobs, uint32_t* count) {
  if (!blobs || !count) return E_INVALIDARG;
  size_t required = 0;
  HRESULT hr = async_get_result_size(block, &required);
  if (FAILED(hr)) return hr;
  if (required < sizeof(uint32_t)) return E_FAIL;
  if (required - sizeof(uint32_t) > size) return kBufferTooSmall;
  std::vector<uint8_t> result(required);
  hr = async_get_result(block, &kReadIdentity, result.size(), result.data(), nullptr);
  if (FAILED(hr)) return hr;
  uint32_t n = 0;
  std::memcpy(&n, result.data(), sizeof(n));
  std::memcpy(blobs, result.data() + sizeof(n), result.size() - sizeof(n));
  auto* base = reinterpret_cast<uint8_t*>(blobs);
  for (uint32_t i = 0; i < n; ++i) {
    blobs[i].data = base + reinterpret_cast<uintptr_t>(blobs[i].data);
    blobs[i].info.name = reinterpret_cast<const char*>(base + reinterpret_cast<uintptr_t>(blobs[i].info.name));
  }
  *count = n;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE create_update(Object*, void* handle, const char* display_name, void** update) {
  Container* container = container_of(handle);
  if (!container || !update) return E_INVALIDARG;
  auto* made = new Update();
  made->container = container;
  if (display_name) made->display_name = display_name;
  *update = track(made);
  return S_OK;
}

void STDMETHODCALLTYPE close_update(Object*, void* handle) {
  if (Update* update = update_of(handle)) untrack(update);
}

HRESULT STDMETHODCALLTYPE submit_blob_write(Object*, void* handle, const char* name, const uint8_t* data, size_t size) {
  Update* update = update_of(handle);
  if (!update || !valid_name(name, 64) || (!data && size)) return E_INVALIDARG;
  update->writes.emplace_back(name, std::vector<uint8_t>(data, data + size));
  return S_OK;
}

HRESULT STDMETHODCALLTYPE submit_blob_delete(Object*, void* handle, const char* name) {
  Update* update = update_of(handle);
  if (!update || !valid_name(name, 64)) return E_INVALIDARG;
  update->deletes.emplace_back(name);
  return S_OK;
}

HRESULT STDMETHODCALLTYPE submit_update(Object*, void* handle) {
  Update* update = update_of(handle);
  return update ? submit(update) : E_INVALIDARG;
}

HRESULT STDMETHODCALLTYPE submit_update_async(Object*, void* handle, AsyncBlock* block) {
  Update* update = update_of(handle);
  if (!update) return E_INVALIDARG;
  return begin(block, &kSubmitIdentity, "XGameSaveSubmitUpdateAsync",
               [update](std::vector<uint8_t>*) { return submit(update); });
}

HRESULT STDMETHODCALLTYPE submit_update_result(Object*, AsyncBlock* block) {
  return async_get_result(block, &kSubmitIdentity, 0, nullptr, nullptr);
}

}  // namespace

void register_game_save() {
  // {1bfff3af-f14a-40a3-8e35-9ada906593f9} (provider {704c3f58-e629-4cc2-b197-30511b996fe2})
  constexpr GUID kGameSave = {0x1bfff3af, 0xf14a, 0x40a3, {0x8e, 0x35, 0x9a, 0xda, 0x90, 0x65, 0x93, 0xf9}};
  Interface& save = define_interface(kGameSave, "game-save");
  set_method(save, 3, "XGameSaveInitializeProvider", &initialize_provider);
  set_method(save, 4, "XGameSaveInitializeProviderAsync", &initialize_provider_async);
  set_method(save, 5, "XGameSaveInitializeProviderResult", &initialize_provider_result);
  set_method(save, 6, "XGameSaveCloseProvider", &close_provider);
  set_method(save, 7, "XGameSaveGetRemainingQuota", &get_remaining_quota);
  set_method(save, 8, "XGameSaveGetRemainingQuotaAsync", &get_remaining_quota_async);
  set_method(save, 9, "XGameSaveGetRemainingQuotaResult", &get_remaining_quota_result);
  set_method(save, 10, "XGameSaveDeleteContainer", &delete_container_sync);
  set_method(save, 11, "XGameSaveDeleteContainerAsync", &delete_container_async);
  set_method(save, 12, "XGameSaveDeleteContainerResult", &delete_container_result);
  set_method(save, 13, "XGameSaveGetContainerInfo", &get_container_info);
  set_method(save, 14, "XGameSaveEnumerateContainerInfo", &enumerate_container_info);
  set_method(save, 15, "XGameSaveEnumerateContainerInfoByName", &enumerate_container_info_by_name);
  set_method(save, 16, "XGameSaveCreateContainer", &create_container);
  set_method(save, 17, "XGameSaveCloseContainer", &close_container);
  set_method(save, 18, "XGameSaveEnumerateBlobInfo", &enumerate_blob_info);
  set_method(save, 19, "XGameSaveEnumerateBlobInfoByName", &enumerate_blob_info_by_name);
  set_method(save, 20, "XGameSaveReadBlobData", &read_blob_data);
  set_method(save, 21, "XGameSaveReadBlobDataAsync", &read_blob_data_async);
  set_method(save, 22, "XGameSaveReadBlobDataResult", &read_blob_data_result);
  set_method(save, 23, "XGameSaveCreateUpdate", &create_update);
  set_method(save, 24, "XGameSaveCloseUpdate", &close_update);
  set_method(save, 25, "XGameSaveSubmitBlobWrite", &submit_blob_write);
  set_method(save, 26, "XGameSaveSubmitBlobDelete", &submit_blob_delete);
  set_method(save, 27, "XGameSaveSubmitUpdate", &submit_update);
  set_method(save, 28, "XGameSaveSubmitUpdateAsync", &submit_update_async);
  set_method(save, 29, "XGameSaveSubmitUpdateResult", &submit_update_result);
}

}  // namespace holy::xgr
