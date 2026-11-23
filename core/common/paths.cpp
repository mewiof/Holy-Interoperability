#include "core/common/paths.h"

#include <windows.h>

namespace holy {
namespace {

bool is_separator(wchar_t c) {
  return c == L'\\' || c == L'/';
}

}  // namespace

std::wstring module_path(void* module_handle) {
  std::wstring buffer(512, L'\0');
  for (;;) {
    const DWORD length = GetModuleFileNameW(static_cast<HMODULE>(module_handle), buffer.data(),
                                            static_cast<DWORD>(buffer.size()));
    if (length == 0) return {};
    if (length < buffer.size()) {
      buffer.resize(length);
      return buffer;
    }
    buffer.resize(buffer.size() * 2);
  }
}

std::wstring parent_dir(std::wstring_view path) {
  size_t end = path.size();
  while (end > 0 && is_separator(path[end - 1])) --end;
  while (end > 0 && !is_separator(path[end - 1])) --end;
  while (end > 1 && is_separator(path[end - 1]) && path[end - 2] != L':') --end;
  return std::wstring(path.substr(0, end));
}

std::wstring base_name(std::wstring_view path) {
  size_t end = path.size();
  while (end > 0 && is_separator(path[end - 1])) --end;
  size_t start = end;
  while (start > 0 && !is_separator(path[start - 1]) && path[start - 1] != L':') --start;
  return std::wstring(path.substr(start, end - start));
}

std::wstring join_path(std::wstring_view a, std::wstring_view b) {
  if (a.empty()) return std::wstring(b);
  std::wstring out(a);
  if (!is_separator(out.back())) out.push_back(L'\\');
  while (!b.empty() && is_separator(b.front())) b.remove_prefix(1);
  out.append(b);
  return out;
}

std::wstring absolute_path(const std::wstring& path) {
  if (path.empty()) return {};
  const DWORD needed = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
  if (needed == 0) return path;
  std::wstring out(needed, L'\0');
  const DWORD length = GetFullPathNameW(path.c_str(), needed, out.data(), nullptr);
  out.resize(length);
  while (out.size() > 3 && is_separator(out.back())) out.pop_back();
  return out;
}

bool file_exists(const std::wstring& path) {
  const DWORD attributes = GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

bool dir_exists(const std::wstring& path) {
  const DWORD attributes = GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

bool create_directories(const std::wstring& path) {
  if (path.empty()) return false;
  if (dir_exists(path)) return true;
  const std::wstring parent = parent_dir(path);
  if (!parent.empty() && parent.size() < path.size() && !dir_exists(parent)) {
    if (!create_directories(parent)) return false;
  }
  return CreateDirectoryW(path.c_str(), nullptr) != 0 || GetLastError() == ERROR_ALREADY_EXISTS;
}

std::wstring runtime_log_path(const std::wstring& data_dir) {
  return join_path(join_path(data_dir, L"logs"), L"runtime.log");
}

}  // namespace holy
