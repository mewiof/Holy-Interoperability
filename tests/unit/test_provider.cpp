#include "core/common/provider.h"
#include "tests/unit/test.h"

using namespace holy;

TEST(provider_accepts_only_bare_library_names) {
  EXPECT(is_plain_library_name("xmem.dll"));
  EXPECT(is_plain_library_name("api-ms-win-core-file-l1-1-0.dll"));
  EXPECT(!is_plain_library_name(""));
  EXPECT(!is_plain_library_name(".."));
  EXPECT(!is_plain_library_name("..\\evil.dll"));
  EXPECT(!is_plain_library_name("sub/evil.dll"));
  EXPECT(!is_plain_library_name("C:evil.dll"));
  EXPECT(!is_plain_library_name("\\\\server\\share\\evil.dll"));
}

TEST(provider_aliases_names_that_collide_with_host_dlls) {
  EXPECT(runtime_file_name("XMem.dll") == L"xmem.dll");
  EXPECT(runtime_file_name("GameInput.dll") == L"holy_gameinput.dll");
  EXPECT(runtime_file_name("xgameruntime.dll") == L"holy_xgameruntime.dll");
}

TEST(provider_never_sends_console_libraries_to_the_host) {
  const ProviderContext context{L"Z:\\no\\such\\libs", L"Z:\\no\\such\\package"};
  EXPECT(locate_provider(context, "xgameruntime.dll").kind == ProviderKind::Missing);
  EXPECT(locate_provider(context, "GameInput.dll").kind == ProviderKind::Missing);
  EXPECT(locate_provider(context, "d3d12_xs.dll").kind == ProviderKind::Missing);
  EXPECT(locate_provider(context, "kernel32.dll").kind == ProviderKind::Host);
  EXPECT(locate_provider(context, "..\\kernel32.dll").kind == ProviderKind::Missing);
}
