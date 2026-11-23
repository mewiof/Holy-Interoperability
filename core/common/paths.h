#pragma once

#include <string>
#include <string_view>

namespace holy {

// Full path of a loaded module; nullptr means the process executable.
std::wstring module_path(void* module_handle);

std::wstring parent_dir(std::wstring_view path);
std::wstring base_name(std::wstring_view path);
std::wstring join_path(std::wstring_view a, std::wstring_view b);

// Absolute, normalised path without a trailing separator (except drive roots).
std::wstring absolute_path(const std::wstring& path);

bool file_exists(const std::wstring& path);
bool dir_exists(const std::wstring& path);
bool create_directories(const std::wstring& path);

// Location of the runtime's log file inside a per-title data directory.
std::wstring runtime_log_path(const std::wstring& data_dir);

}  // namespace holy
