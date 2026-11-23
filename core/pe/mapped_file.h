#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace holy::pe {

// Read-only memory mapping of a file. The file is opened for reading only and
// shared for reading, so mapping a title binary can never alter it.
class MappedFile {
 public:
  static std::optional<MappedFile> open(const std::wstring& path, std::string* error = nullptr);

  MappedFile(MappedFile&& other) noexcept;
  MappedFile& operator=(MappedFile&& other) noexcept;
  MappedFile(const MappedFile&) = delete;
  MappedFile& operator=(const MappedFile&) = delete;
  ~MappedFile();

  std::span<const uint8_t> bytes() const { return {data_, size_}; }

 private:
  MappedFile() = default;
  void reset();

  void* file_ = nullptr;
  void* mapping_ = nullptr;
  const uint8_t* data_ = nullptr;
  size_t size_ = 0;
};

}  // namespace holy::pe
