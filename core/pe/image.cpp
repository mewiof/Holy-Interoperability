#include "core/pe/image.h"

#include <windows.h>

#include <cstring>

namespace holy::pe {
namespace {

constexpr size_t kMaxImportLibraries = 4096;
constexpr size_t kMaxSymbolsPerLibrary = 65536;
constexpr size_t kMaxExports = 1 << 20;

bool set_error(std::string* error, const char* message) {
  if (error) *error = message;
  return false;
}

template <typename T>
bool read_at(std::span<const uint8_t> bytes, size_t offset, T& out) {
  if (offset > bytes.size() || bytes.size() - offset < sizeof(T)) return false;
  std::memcpy(&out, bytes.data() + offset, sizeof(T));
  return true;
}

}  // namespace

std::string ImportSymbol::display_name() const {
  return by_ordinal ? "#" + std::to_string(ordinal) : name;
}

std::optional<Image> Image::parse(std::span<const uint8_t> bytes, Layout layout, std::string* error) {
  IMAGE_DOS_HEADER dos;
  if (!read_at(bytes, 0, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0) {
    set_error(error, "not a PE image (bad DOS header)");
    return std::nullopt;
  }
  IMAGE_NT_HEADERS64 nt;
  const size_t nt_offset = static_cast<size_t>(dos.e_lfanew);
  if (!read_at(bytes, nt_offset, nt) || nt.Signature != IMAGE_NT_SIGNATURE) {
    set_error(error, "not a PE image (bad NT signature)");
    return std::nullopt;
  }
  if (nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
      nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
    set_error(error, "unsupported image: only PE32+ for x64 is handled");
    return std::nullopt;
  }
  if (nt.FileHeader.SizeOfOptionalHeader < offsetof(IMAGE_OPTIONAL_HEADER64, DataDirectory)) {
    set_error(error, "truncated optional header");
    return std::nullopt;
  }

  Image image;
  image.bytes_ = bytes;
  image.layout_ = layout;
  image.nt_offset_ = static_cast<uint32_t>(nt_offset);
  image.image_base_ = nt.OptionalHeader.ImageBase;
  image.size_of_image_ = nt.OptionalHeader.SizeOfImage;
  image.size_of_headers_ = nt.OptionalHeader.SizeOfHeaders;
  image.entry_point_rva_ = nt.OptionalHeader.AddressOfEntryPoint;
  image.timestamp_ = nt.FileHeader.TimeDateStamp;
  image.subsystem_ = nt.OptionalHeader.Subsystem;
  image.dll_characteristics_ = nt.OptionalHeader.DllCharacteristics;
  image.file_characteristics_ = nt.FileHeader.Characteristics;

  const size_t available_directories =
      (nt.FileHeader.SizeOfOptionalHeader - offsetof(IMAGE_OPTIONAL_HEADER64, DataDirectory)) /
      sizeof(IMAGE_DATA_DIRECTORY);
  size_t directory_count = nt.OptionalHeader.NumberOfRvaAndSizes;
  if (directory_count > IMAGE_NUMBEROF_DIRECTORY_ENTRIES) directory_count = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
  if (directory_count > available_directories) directory_count = available_directories;
  for (size_t i = 0; i < directory_count; ++i) {
    image.directories_.push_back(
        {nt.OptionalHeader.DataDirectory[i].VirtualAddress, nt.OptionalHeader.DataDirectory[i].Size});
  }

  const size_t section_table =
      nt_offset + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) + nt.FileHeader.SizeOfOptionalHeader;
  for (unsigned i = 0; i < nt.FileHeader.NumberOfSections; ++i) {
    IMAGE_SECTION_HEADER header;
    if (!read_at(bytes, section_table + i * sizeof(IMAGE_SECTION_HEADER), header)) {
      set_error(error, "truncated section table");
      return std::nullopt;
    }
    Section section;
    section.name.assign(reinterpret_cast<const char*>(header.Name),
                        strnlen(reinterpret_cast<const char*>(header.Name), sizeof(header.Name)));
    section.virtual_address = header.VirtualAddress;
    section.virtual_size = header.Misc.VirtualSize;
    section.raw_offset = header.PointerToRawData;
    section.raw_size = header.SizeOfRawData;
    section.characteristics = header.Characteristics;
    image.sections_.push_back(std::move(section));
  }
  return image;
}

DataDirectory Image::directory(unsigned index) const {
  return index < directories_.size() ? directories_[index] : DataDirectory{};
}

const uint8_t* Image::rva_to_ptr(uint32_t rva, size_t size) const {
  if (layout_ == Layout::Mapped) {
    if (rva > bytes_.size() || bytes_.size() - rva < size) return nullptr;
    return bytes_.data() + rva;
  }
  if (rva < size_of_headers_) {
    if (rva > bytes_.size() || bytes_.size() - rva < size) return nullptr;
    return bytes_.data() + rva;
  }
  for (const Section& section : sections_) {
    if (rva < section.virtual_address) continue;
    const uint64_t delta = static_cast<uint64_t>(rva) - section.virtual_address;
    if (delta >= section.raw_size || section.raw_size - delta < size) continue;
    const uint64_t offset = static_cast<uint64_t>(section.raw_offset) + delta;
    if (offset > bytes_.size() || bytes_.size() - offset < size) return nullptr;
    return bytes_.data() + offset;
  }
  return nullptr;
}

std::optional<std::string> Image::string_at(uint32_t rva, size_t max_length) const {
  std::string out;
  for (size_t i = 0; i < max_length; ++i) {
    if (rva + i < rva) return std::nullopt;
    const uint8_t* p = rva_to_ptr(static_cast<uint32_t>(rva + i), 1);
    if (!p) return std::nullopt;
    if (*p == 0) return out;
    out.push_back(static_cast<char>(*p));
  }
  return std::nullopt;
}

bool Image::read_thunks(uint32_t lookup_rva, uint32_t address_rva, ImportLibrary& library) const {
  for (size_t i = 0; i < kMaxSymbolsPerLibrary; ++i) {
    const uint64_t slot_offset = i * sizeof(uint64_t);
    if (lookup_rva + slot_offset > UINT32_MAX) return false;
    const uint8_t* p = rva_to_ptr(static_cast<uint32_t>(lookup_rva + slot_offset), sizeof(uint64_t));
    if (!p) return false;
    uint64_t thunk;
    std::memcpy(&thunk, p, sizeof(thunk));
    if (thunk == 0) return true;

    ImportSymbol symbol;
    symbol.iat_rva = static_cast<uint32_t>(address_rva + slot_offset);
    if (thunk & IMAGE_ORDINAL_FLAG64) {
      symbol.by_ordinal = true;
      symbol.ordinal = static_cast<uint16_t>(thunk & 0xFFFF);
    } else {
      if (thunk > UINT32_MAX - 2) return false;
      auto name = string_at(static_cast<uint32_t>(thunk) + 2);
      if (!name) return false;
      symbol.name = std::move(*name);
    }
    library.symbols.push_back(std::move(symbol));
  }
  return false;
}

std::vector<ImportLibrary> Image::imports(const DataDirectory* override_directory) const {
  std::vector<ImportLibrary> out;
  const DataDirectory dir = override_directory ? *override_directory : directory(kDirImport);
  if (dir.rva == 0) return out;

  for (size_t i = 0; i < kMaxImportLibraries; ++i) {
    const uint64_t rva = static_cast<uint64_t>(dir.rva) + i * sizeof(IMAGE_IMPORT_DESCRIPTOR);
    if (rva > UINT32_MAX) break;
    const uint8_t* p = rva_to_ptr(static_cast<uint32_t>(rva), sizeof(IMAGE_IMPORT_DESCRIPTOR));
    if (!p) break;
    IMAGE_IMPORT_DESCRIPTOR descriptor;
    std::memcpy(&descriptor, p, sizeof(descriptor));
    if (descriptor.Name == 0 && descriptor.FirstThunk == 0) break;

    ImportLibrary library;
    auto name = string_at(descriptor.Name, 260);
    if (!name) break;
    library.name = std::move(*name);
    const uint32_t lookup =
        descriptor.OriginalFirstThunk ? descriptor.OriginalFirstThunk : descriptor.FirstThunk;
    read_thunks(lookup, descriptor.FirstThunk, library);
    out.push_back(std::move(library));
  }
  return out;
}

std::vector<ImportLibrary> Image::delay_imports() const {
  std::vector<ImportLibrary> out;
  const DataDirectory dir = directory(kDirDelayImport);
  if (dir.rva == 0) return out;

  for (size_t i = 0; i < kMaxImportLibraries; ++i) {
    const uint64_t rva = static_cast<uint64_t>(dir.rva) + i * sizeof(IMAGE_DELAYLOAD_DESCRIPTOR);
    if (rva > UINT32_MAX) break;
    const uint8_t* p = rva_to_ptr(static_cast<uint32_t>(rva), sizeof(IMAGE_DELAYLOAD_DESCRIPTOR));
    if (!p) break;
    IMAGE_DELAYLOAD_DESCRIPTOR descriptor;
    std::memcpy(&descriptor, p, sizeof(descriptor));
    if (descriptor.DllNameRVA == 0) break;
    // Only the RVA-based form (attribute bit 0 set) exists for x64 images.
    if (!(descriptor.Attributes.AllAttributes & 1)) break;

    ImportLibrary library;
    library.delay_loaded = true;
    auto name = string_at(descriptor.DllNameRVA, 260);
    if (!name) break;
    library.name = std::move(*name);
    read_thunks(descriptor.ImportNameTableRVA, descriptor.ImportAddressTableRVA, library);
    out.push_back(std::move(library));
  }
  return out;
}

std::vector<ExportSymbol> Image::exports() const {
  std::vector<ExportSymbol> out;
  const DataDirectory dir = directory(kDirExport);
  if (dir.rva == 0) return out;
  const uint8_t* p = rva_to_ptr(dir.rva, sizeof(IMAGE_EXPORT_DIRECTORY));
  if (!p) return out;
  IMAGE_EXPORT_DIRECTORY header;
  std::memcpy(&header, p, sizeof(header));
  if (header.NumberOfFunctions > kMaxExports || header.NumberOfNames > kMaxExports) return out;

  const auto read_u32 = [&](uint32_t base_rva, uint32_t index, uint32_t& value) {
    const uint64_t rva = static_cast<uint64_t>(base_rva) + static_cast<uint64_t>(index) * 4;
    const uint8_t* q = rva <= UINT32_MAX ? rva_to_ptr(static_cast<uint32_t>(rva), 4) : nullptr;
    if (!q) return false;
    std::memcpy(&value, q, 4);
    return true;
  };
  const auto read_u16 = [&](uint32_t base_rva, uint32_t index, uint16_t& value) {
    const uint64_t rva = static_cast<uint64_t>(base_rva) + static_cast<uint64_t>(index) * 2;
    const uint8_t* q = rva <= UINT32_MAX ? rva_to_ptr(static_cast<uint32_t>(rva), 2) : nullptr;
    if (!q) return false;
    std::memcpy(&value, q, 2);
    return true;
  };

  std::vector<std::string> names(header.NumberOfFunctions);
  for (uint32_t i = 0; i < header.NumberOfNames; ++i) {
    uint32_t name_rva = 0;
    uint16_t function_index = 0;
    if (!read_u32(header.AddressOfNames, i, name_rva) ||
        !read_u16(header.AddressOfNameOrdinals, i, function_index)) {
      break;
    }
    if (function_index >= names.size()) continue;
    if (auto name = string_at(name_rva)) names[function_index] = std::move(*name);
  }

  for (uint32_t i = 0; i < header.NumberOfFunctions; ++i) {
    uint32_t function_rva = 0;
    if (!read_u32(header.AddressOfFunctions, i, function_rva)) break;
    if (function_rva == 0) continue;
    ExportSymbol symbol;
    symbol.name = std::move(names[i]);
    symbol.ordinal = header.Base + i;
    symbol.rva = function_rva;
    if (function_rva >= dir.rva && function_rva - dir.rva < dir.size) {
      if (auto forwarder = string_at(function_rva)) symbol.forwarder = std::move(*forwarder);
    }
    out.push_back(std::move(symbol));
  }
  return out;
}

std::optional<TlsInfo> Image::tls() const {
  const DataDirectory dir = directory(kDirTls);
  if (dir.rva == 0) return std::nullopt;
  const uint8_t* p = rva_to_ptr(dir.rva, sizeof(IMAGE_TLS_DIRECTORY64));
  if (!p) return std::nullopt;
  IMAGE_TLS_DIRECTORY64 tls;
  std::memcpy(&tls, p, sizeof(tls));
  TlsInfo info;
  info.raw_data_start_va = tls.StartAddressOfRawData;
  info.raw_data_end_va = tls.EndAddressOfRawData;
  info.index_va = tls.AddressOfIndex;
  info.callbacks_va = tls.AddressOfCallBacks;
  info.zero_fill_size = tls.SizeOfZeroFill;
  return info;
}

}  // namespace holy::pe
