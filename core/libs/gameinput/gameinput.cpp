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
// One gamepad is surfaced (see "the host gamepad" below). Readings of other
// kinds fail with GAMEINPUT_E_READING_NOT_FOUND (0x838A0003, observed as the
// "no reading" result). Device enumeration reports the gamepad as connected;
// it never disconnects. Reading callbacks register and do not fire. Other
// slots report themselves.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

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

// --- the host gamepad ------------------------------------------------------------------
//
// One gamepad is surfaced, always connected: the host's first XInput
// controller, and the keyboard while a window of the process has the focus
// (so a title can be driven on a host without a controller; the mapping is in
// docs/USAGE.md). Documented layouts (public GameInput.h): the gamepad state
// is the buttons (uint32_t) and six floats (left and right trigger, left
// stick x and y, right stick x and y); readings and devices keep the v0
// method order.

constexpr uint32_t kKindControllerAxis = 0x2, kKindControllerButton = 0x4, kKindGamepad = 0x40000;
constexpr uint32_t kGamepadKinds = kKindGamepad | kKindControllerAxis | kKindControllerButton;
constexpr uint32_t kStatusConnected = 0x1, kStatusInputEnabled = 0x2;

enum GamepadButton : uint32_t {
  kMenu = 0x1, kView = 0x2, kA = 0x4, kB = 0x8, kX = 0x10, kY = 0x20, kDpadUp = 0x40, kDpadDown = 0x80,
  kDpadLeft = 0x100, kDpadRight = 0x200, kLeftShoulder = 0x400, kRightShoulder = 0x800, kLeftThumb = 0x1000,
  kRightThumb = 0x2000,
};

struct GamepadState {
  uint32_t buttons = 0;
  float left_trigger = 0, right_trigger = 0;
  float left_x = 0, left_y = 0, right_x = 0, right_y = 0;
  bool operator==(const GamepadState&) const = default;
};
static_assert(sizeof(GamepadState) == 28);

// XInput (documented host API, part of Windows): the state of controller 0.
struct XInputGamepad {
  uint16_t buttons;
  uint8_t left_trigger, right_trigger;
  int16_t left_x, left_y, right_x, right_y;
};
struct XInputState {
  uint32_t packet;
  XInputGamepad pad;
};

float stick(int16_t value) { return value < -32767 ? -1.0f : static_cast<float>(value) / 32767.0f; }

bool process_has_focus() {
  DWORD owner = 0;
  const HWND foreground = GetForegroundWindow();
  return foreground && GetWindowThreadProcessId(foreground, &owner) && owner == GetCurrentProcessId();
}

bool key(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

// --- scripted input ------------------------------------------------------------------
//
// For runs without a person at the controls (regression runs, measurements):
// HOLY_INPUT_SCRIPT lists presses, each `<when>:<controls>[/<ms>]`, separated
// by ';'. `<when>` is seconds since the process started (the runtime log's
// clock): `8.5` once, `10-60/2` every 2 s from 10 s to 60 s. `<controls>` are
// joined by '+': A B X Y Menu View Up Down Left Right LB RB LS RS (buttons),
// LT RT (triggers), LSUp LSDown LSLeft LSRight RSUp RSDown RSLeft RSRight
// (sticks pushed fully). A press lasts `<ms>` (150 by default). Scripted
// presses add to the host controller's and the keyboard's.

struct ScriptedPress {
  double first = 0, last = 0, period = 0;  // seconds; period 0: once
  double hold = 0.15;
  GamepadState controls;
};

bool parse_controls(const std::string& text, GamepadState* out) {
  static const struct {
    const char* name;
    uint32_t button;
  } kNames[] = {{"A", kA},           {"B", kB},       {"X", kX},           {"Y", kY},
                {"Menu", kMenu},     {"View", kView}, {"Up", kDpadUp},     {"Down", kDpadDown},
                {"Left", kDpadLeft}, {"Right", kDpadRight}, {"LB", kLeftShoulder}, {"RB", kRightShoulder},
                {"LS", kLeftThumb},  {"RS", kRightThumb}};
  size_t at = 0;
  while (at <= text.size()) {
    const size_t end = std::min(text.find('+', at), text.size());
    const std::string name = text.substr(at, end - at);
    bool known = false;
    for (const auto& n : kNames) {
      if (name == n.name) out->buttons |= n.button, known = true;
    }
    if (name == "LT") out->left_trigger = 1.0f, known = true;
    if (name == "RT") out->right_trigger = 1.0f, known = true;
    if (name == "LSUp") out->left_y = 1.0f, known = true;
    if (name == "LSDown") out->left_y = -1.0f, known = true;
    if (name == "LSLeft") out->left_x = -1.0f, known = true;
    if (name == "LSRight") out->left_x = 1.0f, known = true;
    if (name == "RSUp") out->right_y = 1.0f, known = true;
    if (name == "RSDown") out->right_y = -1.0f, known = true;
    if (name == "RSLeft") out->right_x = -1.0f, known = true;
    if (name == "RSRight") out->right_x = 1.0f, known = true;
    if (!known) return false;
    at = end + 1;
  }
  return true;
}

const std::vector<ScriptedPress>& script() {
  static const std::vector<ScriptedPress> presses = [] {
    std::vector<ScriptedPress> out;
    char text[4096] = {};
    if (!GetEnvironmentVariableA("HOLY_INPUT_SCRIPT", text, sizeof(text))) return out;
    const std::string all = text;
    size_t at = 0;
    while (at < all.size()) {
      const size_t end = std::min(all.find(';', at), all.size());
      const std::string entry = all.substr(at, end - at);
      at = end + 1;
      if (entry.empty()) continue;
      ScriptedPress press;
      const size_t colon = entry.find(':');
      bool ok = colon != std::string::npos;
      if (ok) {
        const std::string when = entry.substr(0, colon);
        std::string controls = entry.substr(colon + 1);
        const size_t slash = controls.find('/');
        if (slash != std::string::npos) {
          press.hold = std::atof(controls.c_str() + slash + 1) / 1000.0;
          controls.resize(slash);
        }
        const size_t dash = when.find('-');
        press.first = std::atof(when.c_str());
        press.last = press.first;
        if (dash != std::string::npos) {
          press.last = std::atof(when.c_str() + dash + 1);
          const size_t every = when.find('/');
          press.period = every != std::string::npos ? std::atof(when.c_str() + every + 1) : 1.0;
        }
        ok = parse_controls(controls, &press.controls) && press.hold > 0 && press.period >= 0;
      }
      if (!ok) {
        LIB_LOG(holy::lib::kError, "gameinput", "HOLY_INPUT_SCRIPT: cannot read \"%s\"", entry.c_str());
        continue;
      }
      out.push_back(press);
    }
    LIB_LOG(holy::lib::kInfo, "gameinput", "scripted input: %zu entries", out.size());
    return out;
  }();
  return presses;
}

// Seconds since the process started.
double process_seconds() {
  static const uint64_t created = [] {
    FILETIME creation, exit, kernel, user;
    GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user);
    return (static_cast<uint64_t>(creation.dwHighDateTime) << 32) | creation.dwLowDateTime;
  }();
  FILETIME now;
  GetSystemTimeAsFileTime(&now);
  const uint64_t ticks = (static_cast<uint64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
  return static_cast<double>(ticks - created) / 1e7;
}

void add_scripted(GamepadState* s) {
  const std::vector<ScriptedPress>& presses = script();
  if (presses.empty()) return;
  const double now = process_seconds();
  for (const ScriptedPress& p : presses) {
    if (now < p.first || now > p.last + p.hold) continue;
    const double into = p.period > 0 ? std::fmod(now - p.first, p.period) : now - p.first;
    if (into > p.hold) continue;
    s->buttons |= p.controls.buttons;
    if (p.controls.left_trigger) s->left_trigger = p.controls.left_trigger;
    if (p.controls.right_trigger) s->right_trigger = p.controls.right_trigger;
    if (p.controls.left_x) s->left_x = p.controls.left_x;
    if (p.controls.left_y) s->left_y = p.controls.left_y;
    if (p.controls.right_x) s->right_x = p.controls.right_x;
    if (p.controls.right_y) s->right_y = p.controls.right_y;
  }
}

GamepadState host_gamepad() {
  GamepadState s;
  using GetStateFn = DWORD(WINAPI*)(DWORD, XInputState*);
  static const auto get_state = [] {
    const HMODULE xinput = LoadLibraryExW(L"xinput1_4.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    return xinput ? reinterpret_cast<GetStateFn>(GetProcAddress(xinput, "XInputGetState")) : nullptr;
  }();
  XInputState x{};
  if (get_state && get_state(0, &x) == ERROR_SUCCESS) {
    static const struct {
      uint16_t from;
      uint32_t to;
    } kButtons[] = {{0x0001, kDpadUp},       {0x0002, kDpadDown},     {0x0004, kDpadLeft}, {0x0008, kDpadRight},
                    {0x0010, kMenu},         {0x0020, kView},         {0x0040, kLeftThumb}, {0x0080, kRightThumb},
                    {0x0100, kLeftShoulder}, {0x0200, kRightShoulder}, {0x1000, kA},         {0x2000, kB},
                    {0x4000, kX},            {0x8000, kY}};
    for (const auto& b : kButtons) {
      if (x.pad.buttons & b.from) s.buttons |= b.to;
    }
    s.left_trigger = x.pad.left_trigger / 255.0f;
    s.right_trigger = x.pad.right_trigger / 255.0f;
    s.left_x = stick(x.pad.left_x);
    s.left_y = stick(x.pad.left_y);
    s.right_x = stick(x.pad.right_x);
    s.right_y = stick(x.pad.right_y);
  }
  if (process_has_focus()) {
    static const struct {
      int vk;
      uint32_t to;
    } kKeys[] = {{VK_RETURN, kMenu}, {VK_TAB, kView},     {VK_SPACE, kA},      {VK_ESCAPE, kB},      {VK_BACK, kB},
                 {'R', kX},          {'F', kY},           {VK_UP, kDpadUp},    {VK_DOWN, kDpadDown}, {VK_LEFT, kDpadLeft},
                 {VK_RIGHT, kDpadRight}, {'Q', kLeftShoulder}, {'E', kRightShoulder}, {VK_LSHIFT, kLeftThumb},
                 {'V', kRightThumb}};
    for (const auto& k : kKeys) {
      if (key(k.vk)) s.buttons |= k.to;
    }
    if (key('W')) s.left_y = 1.0f;
    if (key('S')) s.left_y = -1.0f;
    if (key('A')) s.left_x = -1.0f;
    if (key('D')) s.left_x = 1.0f;
    if (key('I')) s.right_y = 1.0f;
    if (key('K')) s.right_y = -1.0f;
    if (key('J')) s.right_x = -1.0f;
    if (key('L')) s.right_x = 1.0f;
    if (key('Z')) s.left_trigger = 1.0f;
    if (key('C')) s.right_trigger = 1.0f;
  }
  add_scripted(&s);
  return s;
}

uint64_t now_us() {
  LARGE_INTEGER counter, frequency;
  QueryPerformanceCounter(&counter);
  QueryPerformanceFrequency(&frequency);
  return static_cast<uint64_t>(counter.QuadPart / frequency.QuadPart * 1000000 +
                               counter.QuadPart % frequency.QuadPart * 1000000 / frequency.QuadPart);
}

// The latest state, its sequence number (advanced when the state changes)
// and timestamp.
struct Sample {
  GamepadState state;
  uint64_t sequence = 0;
  uint64_t timestamp = 0;
};
SRWLOCK g_sample_lock = SRWLOCK_INIT;
Sample g_sample;

Sample poll() {
  const GamepadState state = host_gamepad();
  AcquireSRWLockExclusive(&g_sample_lock);
  if (g_sample.sequence == 0 || !(state == g_sample.state)) {
    g_sample.state = state;
    ++g_sample.sequence;
    g_sample.timestamp = now_us();
  }
  const Sample sample = g_sample;
  ReleaseSRWLockExclusive(&g_sample_lock);
  return sample;
}

// IGameInputDevice. Its information block follows the documented
// GameInputDeviceInfo layout: sizes and identities, counts, then pointers to
// per-kind information (all present; those not described are zeroed).
const holy::lib::VtableInfo kDeviceInfoVtable{kLibrary, "IGameInputDevice"};
const holy::lib::VtableInfo kReadingInfo{kLibrary, "IGameInputReading"};

struct DeviceInfo {
  uint32_t info_size;
  uint16_t vendor_id, product_id, revision;
  uint8_t interface_number, collection_number;
  uint16_t usage_page, usage_id;
  uint16_t hardware_version[4], firmware_version[4];
  uint8_t device_id[32], device_root_id[32];
  int32_t device_family;
  uint32_t capabilities;
  uint32_t supported_input;
  uint32_t supported_rumble_motors;
  uint32_t input_report_count, output_report_count, feature_report_count;
  uint32_t axis_count, button_count, switch_count;
  uint32_t touch_point_count, touch_sensor_count;
  uint32_t force_feedback_motor_count, haptic_feedback_motor_count;
  uint32_t device_string_count, device_descriptor_size;
  const void* pointers[20];  // per-kind information, names, strings, descriptor
};
static_assert(offsetof(DeviceInfo, device_family) == 96 && offsetof(DeviceInfo, pointers) == 160);

struct GameInputString {
  uint32_t size_in_bytes;
  uint32_t code_point_count;
  const char* data;
};

const uint8_t kZeroInfo[512] = {};
const GameInputString kDisplayName{9, 8, "Gamepad"};

const DeviceInfo kGamepadInfo = [] {
  DeviceInfo info{};
  info.info_size = sizeof(DeviceInfo);
  info.vendor_id = 0x045E;  // the console's own controllers
  info.product_id = 0x0B12;
  info.usage_page = 0x01;  // generic desktop, game pad
  info.usage_id = 0x05;
  for (int i = 0; i < 32; ++i) info.device_id[i] = info.device_root_id[i] = static_cast<uint8_t>(0x48 + i);
  info.device_family = 1;  // the current console family
  info.supported_input = kGamepadKinds;
  info.supported_rumble_motors = 0x3;  // low and high frequency
  info.axis_count = 6;
  info.button_count = 14;
  for (const void*& pointer : info.pointers) pointer = kZeroInfo;
  info.pointers[17] = &kDisplayName;  // display name
  return info;
}();

struct Device {
  void* const* vtable;
  const holy::lib::VtableInfo* info;
};

const DeviceInfo* STDMETHODCALLTYPE device_get_info(Device*) { return &kGamepadInfo; }
uint32_t STDMETHODCALLTYPE device_get_status(Device*) { return kStatusConnected | kStatusInputEnabled; }
void STDMETHODCALLTYPE device_get_battery_state(Device*, void* state) {
  if (state) std::memset(state, 0, 16);  // unknown
}
void STDMETHODCALLTYPE device_set_rumble_state(Device*, const float* params) {
  using SetStateFn = DWORD(WINAPI*)(DWORD, uint16_t*);
  static const auto set_state = [] {
    const HMODULE xinput = LoadLibraryExW(L"xinput1_4.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    return xinput ? reinterpret_cast<SetStateFn>(GetProcAddress(xinput, "XInputSetState")) : nullptr;
  }();
  if (!set_state) return;
  // Documented parameters: low frequency, high frequency, left trigger,
  // right trigger (0 to 1).
  uint16_t motors[2] = {0, 0};
  if (params) {
    motors[0] = static_cast<uint16_t>(std::clamp(params[0], 0.0f, 1.0f) * 65535.0f);
    motors[1] = static_cast<uint16_t>(std::clamp(params[1], 0.0f, 1.0f) * 65535.0f);
  }
  set_state(0, motors);
}

const auto kDeviceVtable = [] {
  auto table = holy::lib::make_reporting_vtable<kSlots>();
  table[0] = reinterpret_cast<void*>(&query_interface);
  table[1] = reinterpret_cast<void*>(&add_ref);
  table[2] = reinterpret_cast<void*>(&release);
  table[3] = reinterpret_cast<void*>(&device_get_info);
  table[4] = reinterpret_cast<void*>(&device_get_status);
  table[5] = reinterpret_cast<void*>(&device_get_battery_state);
  table[10] = reinterpret_cast<void*>(&device_set_rumble_state);
  return table;
}();

Device g_gamepad{kDeviceVtable.data(), &kDeviceInfoVtable};

// IGameInputReading: one sample of the gamepad.
struct Reading {
  void* const* vtable;
  const holy::lib::VtableInfo* info;
  std::atomic<long> refs{1};
  Sample sample;
};

HRESULT STDMETHODCALLTYPE reading_query_interface(Reading* self, REFIID, void** out) {
  if (!out) return E_POINTER;
  self->refs.fetch_add(1);
  *out = self;
  return S_OK;
}
ULONG STDMETHODCALLTYPE reading_add_ref(Reading* self) { return static_cast<ULONG>(self->refs.fetch_add(1) + 1); }
ULONG STDMETHODCALLTYPE reading_release(Reading* self) {
  const long left = self->refs.fetch_sub(1) - 1;
  if (left == 0) delete self;
  return static_cast<ULONG>(left);
}
uint32_t STDMETHODCALLTYPE reading_get_input_kind(Reading*) { return kGamepadKinds; }
uint64_t STDMETHODCALLTYPE reading_get_sequence_number(Reading* self, uint32_t) { return self->sample.sequence; }
uint64_t STDMETHODCALLTYPE reading_get_timestamp(Reading* self) { return self->sample.timestamp; }
void STDMETHODCALLTYPE reading_get_device(Reading*, void** device) {
  if (device) *device = &g_gamepad;
}
bool STDMETHODCALLTYPE reading_get_raw_report(Reading*, void** report) {
  if (report) *report = nullptr;
  return false;
}
uint32_t STDMETHODCALLTYPE reading_zero_count(Reading*) { return 0; }
uint32_t STDMETHODCALLTYPE reading_zero_state(Reading*, uint32_t, void*) { return 0; }
bool STDMETHODCALLTYPE reading_no_state(Reading*, void*) { return false; }
bool STDMETHODCALLTYPE reading_get_gamepad_state(Reading* self, GamepadState* state) {
  if (state) *state = self->sample.state;
  return true;
}

const auto kReadingVtable = [] {
  auto table = holy::lib::make_reporting_vtable<kSlots>();
  table[0] = reinterpret_cast<void*>(&reading_query_interface);
  table[1] = reinterpret_cast<void*>(&reading_add_ref);
  table[2] = reinterpret_cast<void*>(&reading_release);
  table[3] = reinterpret_cast<void*>(&reading_get_input_kind);
  table[4] = reinterpret_cast<void*>(&reading_get_sequence_number);
  table[5] = reinterpret_cast<void*>(&reading_get_timestamp);
  table[6] = reinterpret_cast<void*>(&reading_get_device);
  table[7] = reinterpret_cast<void*>(&reading_get_raw_report);
  // Controller axes, buttons and switches (counts, states), keys, mouse,
  // touch, motion, arcade stick, flight stick: none in a gamepad reading.
  table[8] = reinterpret_cast<void*>(&reading_zero_count);
  table[9] = reinterpret_cast<void*>(&reading_zero_state);
  table[10] = reinterpret_cast<void*>(&reading_zero_count);
  table[11] = reinterpret_cast<void*>(&reading_zero_state);
  table[12] = reinterpret_cast<void*>(&reading_zero_count);
  table[13] = reinterpret_cast<void*>(&reading_zero_state);
  table[14] = reinterpret_cast<void*>(&reading_zero_count);
  table[15] = reinterpret_cast<void*>(&reading_zero_state);
  table[16] = reinterpret_cast<void*>(&reading_no_state);
  table[17] = reinterpret_cast<void*>(&reading_zero_count);
  table[18] = reinterpret_cast<void*>(&reading_zero_state);
  table[19] = reinterpret_cast<void*>(&reading_no_state);
  table[20] = reinterpret_cast<void*>(&reading_no_state);
  table[21] = reinterpret_cast<void*>(&reading_no_state);
  table[22] = reinterpret_cast<void*>(&reading_get_gamepad_state);
  table[23] = reinterpret_cast<void*>(&reading_no_state);
  table[24] = reinterpret_cast<void*>(&reading_no_state);
  return table;
}();

Reading* new_reading(const Sample& sample) {
  auto* reading = new Reading();
  reading->vtable = kReadingVtable.data();
  reading->info = &kReadingInfo;
  reading->sample = sample;
  return reading;
}

bool wants_gamepad(uint32_t kind, const void* device) {
  return (kind & kGamepadKinds) != 0 && (!device || device == &g_gamepad);
}

uint64_t STDMETHODCALLTYPE get_current_timestamp(GameInput*) { return now_us(); }

HRESULT STDMETHODCALLTYPE get_current_reading(GameInput*, uint32_t kind, void* device, void** reading) {
  if (!reading) return E_POINTER;
  *reading = nullptr;
  if (!wants_gamepad(kind, device)) return kReadingNotFound;
  *reading = new_reading(poll());
  return S_OK;
}

// The reading after `reference`: the latest sample when the state changed
// since (intermediate states between polls are not kept).
HRESULT STDMETHODCALLTYPE get_next_reading(GameInput*, void* reference, uint32_t kind, void* device, void** reading) {
  if (!reading) return E_POINTER;
  *reading = nullptr;
  if (!wants_gamepad(kind, device)) return kReadingNotFound;
  const Sample sample = poll();
  const auto* previous = static_cast<const Reading*>(reference);
  if (previous && previous->vtable == kReadingVtable.data() && sample.sequence <= previous->sample.sequence) {
    return kReadingNotFound;
  }
  *reading = new_reading(sample);
  return S_OK;
}

HRESULT STDMETHODCALLTYPE get_previous_reading(GameInput*, void*, uint32_t, void*, void** reading) {
  if (reading) *reading = nullptr;
  return kReadingNotFound;
}

HRESULT STDMETHODCALLTYPE register_reading_callback(GameInput*, void*, uint32_t, float, void*, void* callback,
                                                    uint64_t* token) {
  if (!callback) return E_INVALIDARG;
  if (token) *token = g_next_token.fetch_add(1);
  return S_OK;
}

// Documented callback: (token, context, device, timestamp, current status,
// previous status). Enumeration 1 reports the devices present
// asynchronously, 2 before the registration returns, 0 not at all.
using DeviceCallback = void(CALLBACK*)(uint64_t, void*, void*, uint64_t, uint32_t, uint32_t);

HRESULT STDMETHODCALLTYPE register_device_callback(GameInput*, void* device, uint32_t kind, uint32_t status,
                                                   uint32_t enumeration, void* context, void* callback,
                                                   uint64_t* token) {
  if (!callback) return E_INVALIDARG;
  const uint64_t id = g_next_token.fetch_add(1);
  if (token) *token = id;
  const uint32_t present = kStatusConnected | kStatusInputEnabled;
  const bool reported = wants_gamepad(kind, device) && (status == 0 || (status & present)) && enumeration != 0;
  LIB_LOG(holy::lib::kDebug, "gameinput", "device callback registered (kind 0x%x, status 0x%x, enumeration %u)%s",
          kind, status, enumeration, reported ? ": the gamepad is reported" : "");
  if (!reported) return S_OK;
  const auto fn = reinterpret_cast<DeviceCallback>(callback);
  if (enumeration == 2) {
    fn(id, context, &g_gamepad, now_us(), present, 0);
  } else {
    std::thread([fn, id, context] { fn(id, context, &g_gamepad, now_us(), kStatusConnected | kStatusInputEnabled, 0); }).detach();
  }
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
  table[5] = reinterpret_cast<void*>(&get_next_reading);
  table[6] = reinterpret_cast<void*>(&get_previous_reading);
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
  LIB_LOG(holy::lib::kInfo, "gameinput", "GameInputCreate: one gamepad (host controller 0 and the keyboard)");
  return S_OK;
}
