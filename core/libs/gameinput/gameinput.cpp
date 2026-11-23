// gameinput.dll: the console input library.
//
// Entry point (observed): ordinal 1 is GameInputCreate(IGameInput**), with
// the documented signature of the public GameInput.h.
//
// IGameInput (documented interface; slot order of the public v0 header,
// confirmed where the title's calls pin it down, e.g. slot 9 is called with
// RegisterDeviceCallback's seven arguments):
//
//    3 GetCurrentTimestamp()                      -> uint64_t microseconds
//    4 GetCurrentReading(kind, device, reading**)
//    5 GetNextReading(reference, kind, device, reading**)
//    6 GetPreviousReading(reference, kind, device, reading**)
//    8 RegisterReadingCallback(device, kind, threshold, context, callback, token*)
//    9 RegisterDeviceCallback(device, kind, status, enumeration, context, callback, token*)
//   12 StopCallback(token)
//   13 UnregisterCallback(token, timeout_us)       -> bool
//
// Present state: input devices are not surfaced yet. The library answers as
// the platform does when nothing is attached: device enumeration reports no
// device, and reading queries fail with GAMEINPUT_E_READING_NOT_FOUND
// (0x838A0003, observed as the "no reading" result). Callback registrations
// succeed; with no devices they never fire. Other slots report themselves.
#include <windows.h>

#include <atomic>

#include "core/libs/support.h"
#include "core/libs/vtable.h"

namespace {

constexpr char kLibrary[] = "gameinput.dll";
constexpr HRESULT kReadingNotFound = static_cast<HRESULT>(0x838A0003);
constexpr int kSlots = 32;

const holy::lib::VtableInfo kGameInputInfo{kLibrary, "IGameInput"};

struct GameInput {
  void* const* vtable;
  const holy::lib::VtableInfo* info;
};

std::atomic<uint64_t> g_next_token{1};

HRESULT STDMETHODCALLTYPE query_interface(GameInput* self, REFIID, void** out) {
  // The library hands out one interface; any query for it (or IUnknown)
  // returns the same singleton.
  if (!out) return E_POINTER;
  *out = self;
  return S_OK;
}
ULONG STDMETHODCALLTYPE add_ref(GameInput*) {
  return 2;
}
ULONG STDMETHODCALLTYPE release(GameInput*) {
  return 1;
}

uint64_t STDMETHODCALLTYPE get_current_timestamp(GameInput*) {
  LARGE_INTEGER counter, frequency;
  QueryPerformanceCounter(&counter);
  QueryPerformanceFrequency(&frequency);
  return static_cast<uint64_t>(counter.QuadPart / frequency.QuadPart * 1000000 +
                               counter.QuadPart % frequency.QuadPart * 1000000 / frequency.QuadPart);
}

HRESULT STDMETHODCALLTYPE get_current_reading(GameInput*, uint32_t, void*, void** reading) {
  if (reading) *reading = nullptr;
  return kReadingNotFound;
}

HRESULT STDMETHODCALLTYPE get_adjacent_reading(GameInput*, void*, uint32_t, void*, void** reading) {
  if (reading) *reading = nullptr;
  return kReadingNotFound;
}

HRESULT STDMETHODCALLTYPE register_reading_callback(GameInput*, void*, uint32_t, float, void*, void* callback,
                                                    uint64_t* token) {
  if (!callback) return E_INVALIDARG;
  if (token) *token = g_next_token.fetch_add(1);
  return S_OK;
}

HRESULT STDMETHODCALLTYPE register_device_callback(GameInput*, void*, uint32_t kind, uint32_t status,
                                                   uint32_t enumeration, void*, void* callback, uint64_t* token) {
  if (!callback) return E_INVALIDARG;
  if (token) *token = g_next_token.fetch_add(1);
  LIB_LOG(holy::lib::kDebug, "gameinput",
          "device callback registered (kind 0x%x, status 0x%x, enumeration %u): no devices attached", kind, status,
          enumeration);
  return S_OK;
}

void STDMETHODCALLTYPE stop_callback(GameInput*, uint64_t) {}

bool STDMETHODCALLTYPE unregister_callback(GameInput*, uint64_t, uint64_t) {
  return true;
}

const auto kGameInputVtable = [] {
  auto table = holy::lib::make_reporting_vtable<kSlots>();
  table[0] = reinterpret_cast<void*>(&query_interface);
  table[1] = reinterpret_cast<void*>(&add_ref);
  table[2] = reinterpret_cast<void*>(&release);
  table[3] = reinterpret_cast<void*>(&get_current_timestamp);
  table[4] = reinterpret_cast<void*>(&get_current_reading);
  table[5] = reinterpret_cast<void*>(&get_adjacent_reading);
  table[6] = reinterpret_cast<void*>(&get_adjacent_reading);
  table[8] = reinterpret_cast<void*>(&register_reading_callback);
  table[9] = reinterpret_cast<void*>(&register_device_callback);
  table[12] = reinterpret_cast<void*>(&stop_callback);
  table[13] = reinterpret_cast<void*>(&unregister_callback);
  return table;
}();

GameInput g_game_input{kGameInputVtable.data(), &kGameInputInfo};

}  // namespace

extern "C" HRESULT WINAPI GameInputCreate(void** game_input) {
  if (!game_input) return E_POINTER;
  *game_input = &g_game_input;
  LIB_LOG(holy::lib::kInfo, "gameinput", "GameInputCreate: no input devices are surfaced yet");
  return S_OK;
}
