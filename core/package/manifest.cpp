#include "core/package/manifest.h"

#include <windows.h>

#include <cstdio>

#include "core/common/paths.h"
#include "core/common/strings.h"
#include "core/common/xml.h"

namespace holy::package {
namespace {

constexpr size_t kMaxManifestBytes = 4 * 1024 * 1024;

std::string trim(std::string_view text) {
  while (!text.empty() &&
         (text.front() == ' ' || text.front() == '\t' || text.front() == '\r' || text.front() == '\n')) {
    text.remove_prefix(1);
  }
  while (!text.empty() &&
         (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' || text.back() == '\n')) {
    text.remove_suffix(1);
  }
  return std::string(text);
}

std::string child_text(const xml::Node& node, std::string_view name) {
  const xml::Node* child = node.child(name);
  return child ? trim(child->text) : std::string();
}

bool read_small_file(const std::wstring& path, std::string& out, std::string* error) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    if (error) *error = "cannot open the package manifest (error " + std::to_string(GetLastError()) + ")";
    return false;
  }
  LARGE_INTEGER size;
  bool ok = GetFileSizeEx(file, &size) && size.QuadPart > 0 &&
            static_cast<uint64_t>(size.QuadPart) <= kMaxManifestBytes;
  if (ok) {
    out.resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    ok = ReadFile(file, out.data(), static_cast<DWORD>(out.size()), &read, nullptr) && read == out.size();
  }
  CloseHandle(file);
  if (!ok && error) *error = "cannot read the package manifest";
  return ok;
}

}  // namespace

bool is_console_family(std::string_view target_device_family) {
  return iequals(target_device_family, "Scarlett") || iequals(target_device_family, "XboxOne") ||
         iequals(target_device_family, "XboxOneGen9Aware");
}

const Executable* Manifest::default_executable() const {
  for (const Executable& exe : executables) {
    if (!exe.dev_only && is_console_family(exe.target_device_family)) return &exe;
  }
  for (const Executable& exe : executables) {
    if (!exe.dev_only) return &exe;
  }
  return executables.empty() ? nullptr : &executables.front();
}

std::string Manifest::storage_key() const {
  const auto is_safe = [](const std::string& text) {
    if (text.empty() || text.size() > 32) return false;
    for (const char c : text) {
      const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
      if (!ok) return false;
    }
    return true;
  };
  if (is_safe(title_id)) return to_lower(title_id);

  // FNV-1a over the identity so unrelated packages do not share storage.
  uint64_t hash = 1469598103934665603ull;
  for (const char c : identity_name + "|" + identity_publisher) {
    hash ^= static_cast<unsigned char>(c);
    hash *= 1099511628211ull;
  }
  char buffer[24];
  std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(hash));
  return buffer;
}

std::optional<Manifest> parse(std::string_view xml_text, std::string* error) {
  auto root = xml::parse(xml_text, error);
  if (!root) return std::nullopt;
  if (root->name != "Game") {
    if (error) *error = "unexpected manifest root element <" + root->name + ">";
    return std::nullopt;
  }

  Manifest manifest;
  manifest.title_id = child_text(*root, "TitleId");
  manifest.store_id = child_text(*root, "StoreId");
  if (const xml::Node* storage = root->child("PersistentLocalStorage")) {
    const std::string size = child_text(*storage, "SizeMB");
    unsigned long value = 0;
    for (const char c : size) {
      if (c < '0' || c > '9' || value > 0x10000000) {
        value = 0;
        break;
      }
      value = value * 10 + static_cast<unsigned long>(c - '0');
    }
    manifest.persistent_local_storage_mb = static_cast<uint32_t>(value);
  }
  if (const xml::Node* identity = root->child("Identity")) {
    manifest.identity_name = identity->attribute_or("Name");
    manifest.identity_publisher = identity->attribute_or("Publisher");
    manifest.identity_version = identity->attribute_or("Version");
  }
  if (const xml::Node* list = root->child("ExecutableList")) {
    for (const xml::Node* node : list->children_named("Executable")) {
      Executable exe;
      exe.name = node->attribute_or("Name");
      exe.id = node->attribute_or("Id");
      exe.target_device_family = node->attribute_or("TargetDeviceFamily");
      exe.dev_only = iequals(node->attribute_or("IsDevOnly", "false"), "true");
      if (!exe.name.empty()) manifest.executables.push_back(std::move(exe));
    }
  }
  if (manifest.executables.empty()) {
    if (error) *error = "the manifest lists no executables";
    return std::nullopt;
  }
  return manifest;
}

std::optional<Manifest> load(const std::wstring& package_dir, std::string* error) {
  const std::wstring root = absolute_path(package_dir);
  if (!dir_exists(root)) {
    if (error) *error = "the package directory does not exist";
    return std::nullopt;
  }
  std::string text;
  if (!read_small_file(join_path(root, kManifestFileName), text, error)) return std::nullopt;
  auto manifest = parse(text, error);
  if (manifest) manifest->root = root;
  return manifest;
}

}  // namespace holy::package
