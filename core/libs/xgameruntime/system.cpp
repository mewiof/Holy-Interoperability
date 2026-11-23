// System and game identity.
//
// Observed interfaces and slots, identified from their thunks and call sites
// against the documented XSystem.h and XGame.h declarations:
//
//   {b884675d-b738-4a9c-815d-9a9a1e0c6c9b}
//      3 XSystemGetAnalyticsInfo() -> XSystemAnalyticsInfo, returned through a
//        hidden result pointer (0x90 bytes: two XVersion words, then the
//        64-character family and form strings)
//   {1861cf2e-e18b-4834-a9f5-b4a4e6efb4cf} (provider {e349bd1a-fc20-4e40-b99c-4178cc6b409f})
//      3 XSystemGetConsoleId(size, buffer, used)           size 39 observed
//      4 XSystemGetXboxLiveSandboxId(size, buffer, used)   size 16 observed
//      5 XSystemGetAppSpecificDeviceId(size, buffer, used) size 45 observed
//   {2549f142-6419-4a06-97b5-931aab7c2f34} (provider {973a344e-24bf-4d0f-8457-56c534892b29})
//      3 XGameGetXboxTitleId(uint32_t*)
//
// Answers:
//   - versions: the host's own Windows version for both the OS and the
//     hosting OS; family and form follow the console model the runtime
//     reports elsewhere (assumed spellings, as documented examples give them).
//   - title id: the TitleId of the package's own manifest.
//   - sandbox: "RETAIL", the sandbox of every retail device.
//   - device identifiers: stable per host and per title, derived by hashing
//     the host's machine id with the title id, so nothing identifying leaves
//     the machine and different titles see different values. The
//     app-specific id is base64 of a SHA-256 digest (44 characters, matching
//     the documented size); the console id is formatted as a braced GUID,
//     which fits the documented 39-byte size (assumed format).
#include <windows.h>
#include <bcrypt.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "core/libs/support.h"
#include "core/libs/xgameruntime/provider.h"
#include "core/libs/xgameruntime/services.h"
#include "core/package/manifest.h"

namespace holy::xgr {
namespace {

struct Version {
  uint16_t major;
  uint16_t minor;
  uint16_t build;
  uint16_t revision;
};

struct AnalyticsInfo {
  Version os_version;
  Version hosting_os_version;
  char family[64];
  char form[64];
};
static_assert(sizeof(AnalyticsInfo) == 0x90);

Version host_version() {
  using Fn = LONG(WINAPI*)(OSVERSIONINFOW*);
  const auto fn = reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
  OSVERSIONINFOW info{};
  info.dwOSVersionInfoSize = sizeof(info);
  if (fn) fn(&info);
  DWORD ubr = 0;
  DWORD size = sizeof(ubr);
  RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"UBR", RRF_RT_REG_DWORD,
               nullptr, &ubr, &size);
  return Version{static_cast<uint16_t>(info.dwMajorVersion), static_cast<uint16_t>(info.dwMinorVersion),
                 static_cast<uint16_t>(info.dwBuildNumber), static_cast<uint16_t>(ubr)};
}

bool is_series_s() {
  // The registry interposer answers the console model; ask it the same way
  // title code does, so both answers always agree.
  HKEY key = nullptr;
  DWORD mode = 0, size = sizeof(mode);
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows NT\\CurrentVersion\\OEM", 0, KEY_READ,
                    &key) == ERROR_SUCCESS) {
    RegQueryValueExW(key, L"ConsoleMode", nullptr, nullptr, reinterpret_cast<BYTE*>(&mode), &size);
    RegCloseKey(key);
  }
  return mode == 0x60;
}

AnalyticsInfo* STDMETHODCALLTYPE get_analytics_info(Object*, AnalyticsInfo* result) {
  std::memset(result, 0, sizeof(*result));
  result->os_version = host_version();
  result->hosting_os_version = result->os_version;
  std::snprintf(result->family, sizeof(result->family), "Windows.Xbox");
  std::snprintf(result->form, sizeof(result->form), "%s", is_series_s() ? "Xbox Series S" : "Xbox Series X");
  return result;
}

const package::Manifest* manifest() {
  static const auto* loaded = [] {
    const wchar_t* dir = HolyPackageDir();
    auto parsed = dir && *dir ? package::load(dir) : std::nullopt;
    return parsed ? new package::Manifest(*parsed) : nullptr;
  }();
  return loaded;
}

HRESULT STDMETHODCALLTYPE get_xbox_title_id(Object*, uint32_t* title_id) {
  if (!title_id) return E_INVALIDARG;
  *title_id = 0;
  const package::Manifest* m = manifest();
  if (!m || m->title_id.empty()) return E_FAIL;
  *title_id = static_cast<uint32_t>(std::strtoul(m->title_id.c_str(), nullptr, 16));
  return S_OK;
}

// SHA-256 of the host's machine id and the title id.
bool device_digest(uint8_t (&digest)[32]) {
  wchar_t machine[64] = {};
  DWORD size = sizeof(machine);
  if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Cryptography", L"MachineGuid", RRF_RT_REG_SZ, nullptr,
                   machine, &size) != ERROR_SUCCESS) {
    return false;
  }
  const package::Manifest* m = manifest();
  std::string input = "holy-interop-device-id:";
  for (const wchar_t* c = machine; *c; ++c) input += static_cast<char>(*c);
  input += ':';
  if (m) input += m->title_id;
  return BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, reinterpret_cast<PUCHAR>(input.data()),
                    static_cast<ULONG>(input.size()), digest, sizeof(digest)) >= 0;
}

HRESULT copy_string(const std::string& value, size_t size, char* buffer, size_t* used) {
  if (!buffer || !used) return E_INVALIDARG;
  if (size < value.size() + 1) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
  std::memcpy(buffer, value.c_str(), value.size() + 1);
  *used = value.size() + 1;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE get_console_id(Object*, size_t size, char* buffer, size_t* used) {
  uint8_t digest[32];
  if (!device_digest(digest)) return E_FAIL;
  char text[40];
  std::snprintf(text, sizeof(text), "{%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
                digest[0], digest[1], digest[2], digest[3], digest[4], digest[5], digest[6], digest[7], digest[8],
                digest[9], digest[10], digest[11], digest[12], digest[13], digest[14], digest[15]);
  return copy_string(text, size, buffer, used);
}

HRESULT STDMETHODCALLTYPE get_sandbox_id(Object*, size_t size, char* buffer, size_t* used) {
  return copy_string("RETAIL", size, buffer, used);
}

HRESULT STDMETHODCALLTYPE get_app_specific_device_id(Object*, size_t size, char* buffer, size_t* used) {
  uint8_t digest[32];
  if (!device_digest(digest)) return E_FAIL;
  static const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string text;
  for (size_t i = 0; i < sizeof(digest); i += 3) {
    const uint32_t chunk = (static_cast<uint32_t>(digest[i]) << 16) |
                           (i + 1 < sizeof(digest) ? static_cast<uint32_t>(digest[i + 1]) << 8 : 0) |
                           (i + 2 < sizeof(digest) ? digest[i + 2] : 0);
    text += kAlphabet[(chunk >> 18) & 63];
    text += kAlphabet[(chunk >> 12) & 63];
    text += i + 1 < sizeof(digest) ? kAlphabet[(chunk >> 6) & 63] : '=';
    text += i + 2 < sizeof(digest) ? kAlphabet[chunk & 63] : '=';
  }
  return copy_string(text, size, buffer, used);
}

}  // namespace

void register_system() {
  // {b884675d-b738-4a9c-815d-9a9a1e0c6c9b}
  constexpr GUID kAnalytics = {0xb884675d, 0xb738, 0x4a9c, {0x81, 0x5d, 0x9a, 0x9a, 0x1e, 0x0c, 0x6c, 0x9b}};
  Interface& analytics = define_interface(kAnalytics, "system-analytics");
  set_method(analytics, 3, "XSystemGetAnalyticsInfo", &get_analytics_info);

  // {1861cf2e-e18b-4834-a9f5-b4a4e6efb4cf}
  constexpr GUID kIdentity = {0x1861cf2e, 0xe18b, 0x4834, {0xa9, 0xf5, 0xb4, 0xa4, 0xe6, 0xef, 0xb4, 0xcf}};
  Interface& identity = define_interface(kIdentity, "system-identity");
  set_method(identity, 3, "XSystemGetConsoleId", &get_console_id);
  set_method(identity, 4, "XSystemGetXboxLiveSandboxId", &get_sandbox_id);
  set_method(identity, 5, "XSystemGetAppSpecificDeviceId", &get_app_specific_device_id);

  // {2549f142-6419-4a06-97b5-931aab7c2f34}
  constexpr GUID kGame = {0x2549f142, 0x6419, 0x4a06, {0x97, 0xb5, 0x93, 0x1a, 0xab, 0x7c, 0x2f, 0x34}};
  Interface& game = define_interface(kGame, "game");
  set_method(game, 3, "XGameGetXboxTitleId", &get_xbox_title_id);
}

}  // namespace holy::xgr
