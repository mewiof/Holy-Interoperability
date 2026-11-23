#include "core/common/vfs.h"
#include "tests/unit/test.h"

using namespace holy;

namespace {

vfs::Config sample_config() {
  return vfs::make_default_config(L"C:\\pkg\\title", L"C:\\data\\title");
}

}  // namespace

TEST(vfs_translates_mounted_drives) {
  const vfs::Config config = sample_config();
  EXPECT(vfs::translate(config, L"G:\\data\\file.bin") == L"C:\\pkg\\title\\data\\file.bin");
  EXPECT(vfs::translate(config, L"g:/data/file.bin") == L"C:\\pkg\\title\\data\\file.bin");
  EXPECT(vfs::translate(config, L"G:") == L"C:\\pkg\\title");
  EXPECT(vfs::translate(config, L"G:\\") == L"C:\\pkg\\title");
  EXPECT(vfs::translate(config, L"\\\\?\\G:\\x") == L"C:\\pkg\\title\\x");
  EXPECT(vfs::translate(config, L"T:\\cache.tmp") == L"C:\\data\\title\\temp\\cache.tmp");
  EXPECT(vfs::translate(config, L"D:\\scratch.bin") == L"C:\\data\\title\\scratch\\scratch.bin");
}

TEST(vfs_leaves_other_paths_alone) {
  const vfs::Config config = sample_config();
  EXPECT(vfs::translate(config, L"data\\file.bin") == L"data\\file.bin");
  EXPECT(vfs::translate(config, L"C:\\Windows\\win.ini") == L"C:\\Windows\\win.ini");
  EXPECT(vfs::translate(config, L"\\\\.\\pipe\\name") == L"\\\\.\\pipe\\name");
  EXPECT(vfs::translate(config, L"") == L"");
}

TEST(vfs_detects_paths_inside_a_root) {
  const std::wstring root = L"C:\\pkg\\title";
  EXPECT(vfs::is_inside(root, L"C:\\pkg\\title"));
  EXPECT(vfs::is_inside(root, L"C:\\pkg\\title\\a\\b.bin"));
  EXPECT(vfs::is_inside(root, L"c:\\PKG\\Title\\a"));
  EXPECT(vfs::is_inside(root, L"C:\\pkg\\title\\sub\\..\\file"));
  EXPECT(vfs::is_inside(root, L"\\\\?\\C:\\pkg\\title\\file"));
  EXPECT(!vfs::is_inside(root, L"C:\\pkg\\title2\\file"));
  EXPECT(!vfs::is_inside(root, L"C:\\pkg\\title\\..\\other"));
  EXPECT(!vfs::is_inside(root, L"C:\\pkg"));
}

TEST(vfs_redacts_known_roots) {
  const vfs::Config config = sample_config();
  EXPECT(vfs::redact(config, L"C:\\pkg\\title\\data\\x") == L"<package>\\data\\x");
  EXPECT(vfs::redact(config, L"C:\\data\\title\\temp\\y") == L"<data>\\temp\\y");
  EXPECT(vfs::redact(config, L"C:\\elsewhere\\z") == L"C:\\elsewhere\\z");
}
