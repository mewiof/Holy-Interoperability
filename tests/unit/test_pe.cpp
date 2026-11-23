#include <windows.h>

#include <cstring>
#include <vector>

#include "core/analysis/import_report.h"
#include "core/common/paths.h"
#include "core/common/strings.h"
#include "core/pe/image.h"
#include "core/pe/mapped_file.h"
#include "tests/unit/test.h"

using namespace holy;

namespace {

bool imports_library(const std::vector<pe::ImportLibrary>& libraries, const char* name) {
  for (const pe::ImportLibrary& library : libraries) {
    if (iequals(library.name, name)) return true;
  }
  return false;
}

}  // namespace

// The test executable itself is a convenient, always-available PE32+ image.
TEST(pe_reads_its_own_file_and_mapped_layouts) {
  const std::wstring self = module_path(nullptr);
  std::string error;
  auto file = pe::MappedFile::open(self, &error);
  EXPECT(file.has_value());
  if (!file) return;

  const auto on_disk = pe::Image::parse(file->bytes(), pe::Layout::File, &error);
  EXPECT(on_disk.has_value());
  if (!on_disk) return;
  EXPECT(!on_disk->is_dll());
  EXPECT(on_disk->entry_point_rva() != 0);
  EXPECT(!on_disk->sections().empty());
  const auto disk_imports = on_disk->imports();
  EXPECT(imports_library(disk_imports, "KERNEL32.dll"));

  auto* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
  const auto mapped = pe::Image::parse({base, on_disk->size_of_image()}, pe::Layout::Mapped, &error);
  EXPECT(mapped.has_value());
  if (!mapped) return;
  const auto mapped_imports = mapped->imports();
  EXPECT(mapped_imports.size() == disk_imports.size());
  EXPECT(mapped->entry_point_rva() == on_disk->entry_point_rva());
}

TEST(pe_reads_exports_of_a_system_library) {
  wchar_t system_dir[MAX_PATH];
  GetSystemDirectoryW(system_dir, MAX_PATH);
  auto file = pe::MappedFile::open(join_path(system_dir, L"kernel32.dll"));
  EXPECT(file.has_value());
  if (!file) return;
  const auto image = pe::Image::parse(file->bytes(), pe::Layout::File);
  EXPECT(image.has_value());
  if (!image) return;
  EXPECT(image->is_dll());
  bool found = false;
  for (const pe::ExportSymbol& symbol : image->exports()) {
    if (symbol.name == "CreateFileW") found = true;
  }
  EXPECT(found);
}

TEST(pe_rejects_garbage_and_truncated_input) {
  std::string error;
  const std::vector<uint8_t> garbage(4096, 0x41);
  EXPECT(!pe::Image::parse(garbage, pe::Layout::File, &error).has_value());
  EXPECT(!error.empty());

  const std::vector<uint8_t> empty;
  EXPECT(!pe::Image::parse(empty, pe::Layout::File).has_value());

  auto file = pe::MappedFile::open(module_path(nullptr));
  EXPECT(file.has_value());
  if (!file) return;
  // Headers only: parsing succeeds, but nothing behind them is reachable.
  std::vector<uint8_t> truncated(file->bytes().begin(), file->bytes().begin() + 1024);
  const auto image = pe::Image::parse(truncated, pe::Layout::File);
  if (image) {
    EXPECT(image->imports().empty());
    EXPECT(image->exports().empty());
  }
}

TEST(pe_tolerates_hostile_directory_values) {
  auto file = pe::MappedFile::open(module_path(nullptr));
  EXPECT(file.has_value());
  if (!file) return;
  std::vector<uint8_t> copy(file->bytes().begin(), file->bytes().end());
  const auto pristine = pe::Image::parse(copy, pe::Layout::File);
  EXPECT(pristine.has_value());
  if (!pristine) return;

  // Point the import and export directories far outside the file.
  const size_t directories =
      pristine->nt_headers_offset() + offsetof(IMAGE_NT_HEADERS64, OptionalHeader.DataDirectory);
  IMAGE_DATA_DIRECTORY wild{0xFFFFFFF0u, 0xFFFFFFFFu};
  std::memcpy(copy.data() + directories + sizeof(IMAGE_DATA_DIRECTORY) * IMAGE_DIRECTORY_ENTRY_IMPORT, &wild,
              sizeof(wild));
  std::memcpy(copy.data() + directories + sizeof(IMAGE_DATA_DIRECTORY) * IMAGE_DIRECTORY_ENTRY_EXPORT, &wild,
              sizeof(wild));
  const auto hostile = pe::Image::parse(copy, pe::Layout::File);
  EXPECT(hostile.has_value());
  if (!hostile) return;
  EXPECT(hostile->imports().empty());
  EXPECT(hostile->exports().empty());
}

TEST(analysis_resolves_host_imports_of_this_executable) {
  auto file = pe::MappedFile::open(module_path(nullptr));
  EXPECT(file.has_value());
  if (!file) return;
  const auto image = pe::Image::parse(file->bytes(), pe::Layout::File);
  EXPECT(image.has_value());
  if (!image) return;
  const ProviderContext context{L"Z:\\no\\such\\libs", L"Z:\\no\\such\\package"};
  const analysis::ImageReport report = analysis::check_imports(*image, context);
  EXPECT(report.total > 0);
  EXPECT(report.resolved == report.total);
  EXPECT(report.console_total == 0);
}
