// Bounds-checked reader for PE32+ (x64) images.
//
// Works on two layouts: a file as it sits on disk and an image as the OS
// loader mapped it. Images are treated as untrusted input; every access is
// range-checked and iteration counts are capped.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace holy::pe {

enum class Layout { File, Mapped };

// Data directory indices (subset).
inline constexpr unsigned kDirExport = 0;
inline constexpr unsigned kDirImport = 1;
inline constexpr unsigned kDirException = 3;
inline constexpr unsigned kDirBaseReloc = 5;
inline constexpr unsigned kDirTls = 9;
inline constexpr unsigned kDirLoadConfig = 10;
inline constexpr unsigned kDirBoundImport = 11;
inline constexpr unsigned kDirIat = 12;
inline constexpr unsigned kDirDelayImport = 13;

struct DataDirectory {
  uint32_t rva = 0;
  uint32_t size = 0;
};

struct Section {
  std::string name;
  uint32_t virtual_address = 0;
  uint32_t virtual_size = 0;
  uint32_t raw_offset = 0;
  uint32_t raw_size = 0;
  uint32_t characteristics = 0;
};

struct ImportSymbol {
  std::string name;  // empty when imported by ordinal
  uint16_t ordinal = 0;
  bool by_ordinal = false;
  uint32_t iat_rva = 0;  // RVA of the address-table slot to fill

  std::string display_name() const;
};

struct ImportLibrary {
  std::string name;
  bool delay_loaded = false;
  std::vector<ImportSymbol> symbols;
};

struct ExportSymbol {
  std::string name;  // empty for ordinal-only exports
  uint32_t ordinal = 0;
  uint32_t rva = 0;
  std::string forwarder;  // non-empty when the export forwards to another library
};

struct TlsInfo {
  uint64_t raw_data_start_va = 0;
  uint64_t raw_data_end_va = 0;
  uint64_t index_va = 0;
  uint64_t callbacks_va = 0;
  uint32_t zero_fill_size = 0;
};

class Image {
 public:
  // `bytes` must outlive the returned Image. Fails for anything that is not a
  // well-formed PE32+ image for x64.
  static std::optional<Image> parse(std::span<const uint8_t> bytes, Layout layout,
                                    std::string* error = nullptr);

  uint64_t image_base() const { return image_base_; }
  uint32_t size_of_image() const { return size_of_image_; }
  uint32_t size_of_headers() const { return size_of_headers_; }
  uint32_t entry_point_rva() const { return entry_point_rva_; }
  uint32_t timestamp() const { return timestamp_; }
  uint16_t subsystem() const { return subsystem_; }
  uint16_t dll_characteristics() const { return dll_characteristics_; }
  uint16_t file_characteristics() const { return file_characteristics_; }
  bool is_dll() const { return (file_characteristics_ & 0x2000) != 0; }
  bool has_relocations() const { return directory(kDirBaseReloc).size != 0; }
  // Offset of the NT headers from the start of the image.
  uint32_t nt_headers_offset() const { return nt_offset_; }

  DataDirectory directory(unsigned index) const;
  const std::vector<Section>& sections() const { return sections_; }

  // Pointer to `size` readable bytes at `rva`, or nullptr if out of range.
  const uint8_t* rva_to_ptr(uint32_t rva, size_t size) const;
  std::optional<std::string> string_at(uint32_t rva, size_t max_length = 1024) const;

  // Regular imports. `override_directory` reads the table from a different
  // location (used when the header's import directory was replaced).
  std::vector<ImportLibrary> imports(const DataDirectory* override_directory = nullptr) const;
  std::vector<ImportLibrary> delay_imports() const;
  std::vector<ExportSymbol> exports() const;
  std::optional<TlsInfo> tls() const;

 private:
  Image() = default;
  bool read_thunks(uint32_t lookup_rva, uint32_t address_rva, ImportLibrary& library) const;

  std::span<const uint8_t> bytes_;
  Layout layout_ = Layout::File;
  uint32_t nt_offset_ = 0;
  uint64_t image_base_ = 0;
  uint32_t size_of_image_ = 0;
  uint32_t size_of_headers_ = 0;
  uint32_t entry_point_rva_ = 0;
  uint32_t timestamp_ = 0;
  uint16_t subsystem_ = 0;
  uint16_t dll_characteristics_ = 0;
  uint16_t file_characteristics_ = 0;
  std::vector<DataDirectory> directories_;
  std::vector<Section> sections_;
};

}  // namespace holy::pe
