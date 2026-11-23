#include <string>

#include "core/package/manifest.h"
#include "tests/unit/test.h"

using namespace holy;

namespace {

constexpr char kManifest[] = R"(<?xml version="1.0" encoding="utf-8"?>
<Game configVersion="1">
  <TitleId>0A0B0C0D</TitleId>
  <StoreId>9ZZZZZZZZZZZ</StoreId>
  <PersistentLocalStorage><SizeMB>512</SizeMB></PersistentLocalStorage>
  <Identity Name="Example.Sample" Publisher="CN=Example" Version="1.2.3.4" />
  <ExecutableList>
    <Executable Name="tools.exe" Id="Tools" TargetDeviceFamily="Scarlett" IsDevOnly="true" />
    <Executable Name="desktop.exe" Id="Desktop" TargetDeviceFamily="PC" />
    <Executable Name="console.exe" Id="Game" TargetDeviceFamily="Scarlett" />
  </ExecutableList>
</Game>)";

}  // namespace

TEST(manifest_extracts_identity_and_executables) {
  const auto manifest = package::parse(kManifest);
  EXPECT(manifest.has_value());
  if (!manifest) return;
  EXPECT(manifest->identity_name == "Example.Sample");
  EXPECT(manifest->identity_version == "1.2.3.4");
  EXPECT(manifest->title_id == "0A0B0C0D");
  EXPECT(manifest->executables.size() == 3);
  EXPECT(manifest->executables[0].dev_only);
  EXPECT(manifest->persistent_local_storage_mb == 512);
}

TEST(manifest_prefers_the_retail_console_executable) {
  const auto manifest = package::parse(kManifest);
  EXPECT(manifest.has_value());
  if (!manifest) return;
  const package::Executable* exe = manifest->default_executable();
  EXPECT(exe != nullptr && exe->name == "console.exe");
}

TEST(manifest_storage_key_is_filesystem_safe) {
  auto manifest = package::parse(kManifest);
  EXPECT(manifest.has_value());
  if (!manifest) return;
  EXPECT(manifest->storage_key() == "0a0b0c0d");

  manifest->title_id = "..\\..\\escape";
  const std::string key = manifest->storage_key();
  EXPECT(key.size() == 16);
  EXPECT(key.find_first_not_of("0123456789abcdef") == std::string::npos);
}

TEST(manifest_requires_an_executable) {
  std::string error;
  EXPECT(!package::parse("<Game><ExecutableList/></Game>", &error).has_value());
  EXPECT(!error.empty());
  EXPECT(!package::parse("<NotAGame/>").has_value());
}
