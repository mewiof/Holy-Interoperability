#include "core/pe/mapped_file.h"

#include <windows.h>

#include <utility>

namespace holy::pe {

std::optional<MappedFile> MappedFile::open(const std::wstring& path, std::string* error) {
  const auto fail = [&](const char* what) -> std::optional<MappedFile> {
    if (error) *error = std::string(what) + " (error " + std::to_string(GetLastError()) + ")";
    return std::nullopt;
  };

  MappedFile file;
  HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return fail("cannot open file");
  file.file_ = handle;

  LARGE_INTEGER size;
  if (!GetFileSizeEx(handle, &size)) return fail("cannot query file size");
  if (size.QuadPart == 0) {
    if (error) *error = "file is empty";
    return std::nullopt;
  }

  file.mapping_ = CreateFileMappingW(handle, nullptr, PAGE_READONLY, 0, 0, nullptr);
  if (!file.mapping_) return fail("cannot map file");
  file.data_ = static_cast<const uint8_t*>(MapViewOfFile(file.mapping_, FILE_MAP_READ, 0, 0, 0));
  if (!file.data_) return fail("cannot map file view");
  file.size_ = static_cast<size_t>(size.QuadPart);
  return file;
}

MappedFile::MappedFile(MappedFile&& other) noexcept
    : file_(std::exchange(other.file_, nullptr)),
      mapping_(std::exchange(other.mapping_, nullptr)),
      data_(std::exchange(other.data_, nullptr)),
      size_(std::exchange(other.size_, 0)) {}

MappedFile& MappedFile::operator=(MappedFile&& other) noexcept {
  if (this != &other) {
    reset();
    file_ = std::exchange(other.file_, nullptr);
    mapping_ = std::exchange(other.mapping_, nullptr);
    data_ = std::exchange(other.data_, nullptr);
    size_ = std::exchange(other.size_, 0);
  }
  return *this;
}

MappedFile::~MappedFile() {
  reset();
}

void MappedFile::reset() {
  if (data_) UnmapViewOfFile(data_);
  if (mapping_) CloseHandle(static_cast<HANDLE>(mapping_));
  if (file_) CloseHandle(static_cast<HANDLE>(file_));
  data_ = nullptr;
  mapping_ = nullptr;
  file_ = nullptr;
  size_ = 0;
}

}  // namespace holy::pe
