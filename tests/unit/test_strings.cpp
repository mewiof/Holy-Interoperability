#include "core/common/paths.h"
#include "core/common/strings.h"
#include "tests/unit/test.h"

using namespace holy;

TEST(strings_fold_ascii_only) {
  EXPECT(to_lower("KERNEL32.DLL") == "kernel32.dll");
  EXPECT(iequals("XMem.dll", "xmem.DLL"));
  EXPECT(!iequals("xmem.dll", "xmem.dl"));
  EXPECT(istarts_with("api-ms-win-core-file-l1-1-0.dll", "API-MS-WIN-"));
  EXPECT(iends_with("Something.DLL", ".dll"));
}

TEST(strings_round_trip_utf8) {
  const std::wstring wide = L"caf\u00e9 \u65e5\u672c";
  EXPECT(widen(narrow(wide)) == wide);
  EXPECT(narrow(L"").empty());
}

TEST(strings_split_skips_empty_pieces) {
  const auto parts = split("a,,b,", ',');
  EXPECT(parts.size() == 2);
  EXPECT(parts.size() == 2 && parts[0] == "a" && parts[1] == "b");
}

TEST(strings_wildcards) {
  EXPECT(wildcard_match("*", "anything.dll"));
  EXPECT(wildcard_match("d3d12*", "D3D12_xs.dll"));
  EXPECT(wildcard_match("*.dll", "xmem.dll"));
  EXPECT(wildcard_match("x*m*.dll", "xmem.dll"));
  EXPECT(!wildcard_match("xmem.dll", "xmem.dll.bak"));
  EXPECT(!wildcard_match("a*b", "acd"));
}

TEST(paths_split_and_join) {
  EXPECT(parent_dir(L"C:\\a\\b\\c.txt") == L"C:\\a\\b");
  EXPECT(base_name(L"C:\\a\\b\\c.txt") == L"c.txt");
  EXPECT(base_name(L"c.txt") == L"c.txt");
  EXPECT(join_path(L"C:\\a", L"b") == L"C:\\a\\b");
  EXPECT(join_path(L"C:\\a\\", L"\\b") == L"C:\\a\\b");
}
