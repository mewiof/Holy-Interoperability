#include "core/common/vfs.h"

#include "core/common/paths.h"
#include "core/common/strings.h"

namespace holy::vfs {
namespace {

bool is_separator(wchar_t c) {
  return c == L'\\' || c == L'/';
}

wchar_t upper(wchar_t c) {
  return (c >= L'a' && c <= L'z') ? static_cast<wchar_t>(c - L'a' + L'A') : c;
}

bool is_drive_letter(wchar_t c) {
  return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z');
}

}  // namespace

Config make_default_config(const std::wstring& package_root, const std::wstring& data_root) {
  Config config;
  config.package_root = package_root;
  config.data_root = data_root;
  config.mounts.push_back({L'G', package_root, true});
  config.mounts.push_back({L'T', join_path(data_root, L"temp"), false});
  config.mounts.push_back({L'D', join_path(data_root, L"scratch"), false});
  return config;
}

std::wstring translate(const Config& config, std::wstring_view path) {
  std::wstring_view rest = path;
  if (rest.size() >= 4 && rest[0] == L'\\' && rest[1] == L'\\' && rest[2] == L'?' && rest[3] == L'\\') {
    rest.remove_prefix(4);
  }
  if (rest.size() < 2 || rest[1] != L':' || !is_drive_letter(rest[0])) return std::wstring(path);

  const wchar_t drive = upper(rest[0]);
  for (const Mount& mount : config.mounts) {
    if (mount.drive != drive) continue;
    rest.remove_prefix(2);
    while (!rest.empty() && is_separator(rest.front())) rest.remove_prefix(1);
    std::wstring out = mount.target;
    if (!rest.empty()) {
      out.push_back(L'\\');
      for (const wchar_t c : rest) out.push_back(c == L'/' ? L'\\' : c);
    }
    return out;
  }
  return std::wstring(path);
}

bool is_inside(const std::wstring& root, const std::wstring& host_path) {
  if (root.empty() || host_path.empty()) return false;
  std::wstring_view candidate_view = host_path;
  if (candidate_view.size() >= 4 && candidate_view.substr(0, 4) == L"\\\\?\\")
    candidate_view.remove_prefix(4);
  const std::wstring candidate = absolute_path(std::wstring(candidate_view));
  if (!istarts_with(candidate, root)) return false;
  return candidate.size() == root.size() || is_separator(candidate[root.size()]) || is_separator(root.back());
}

std::wstring redact(const Config& config, std::wstring_view host_path) {
  const auto replace_root = [&](const std::wstring& root, const wchar_t* placeholder) -> std::wstring {
    if (root.empty() || !istarts_with(host_path, root)) return {};
    if (host_path.size() > root.size() && !is_separator(host_path[root.size()])) return {};
    return std::wstring(placeholder) + std::wstring(host_path.substr(root.size()));
  };
  std::wstring out = replace_root(config.package_root, L"<package>");
  if (out.empty()) out = replace_root(config.data_root, L"<data>");
  return out.empty() ? std::wstring(host_path) : out;
}

}  // namespace holy::vfs
