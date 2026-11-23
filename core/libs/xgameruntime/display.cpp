// Display service.
//
// Observed: interface {35f07670-706e-4bfb-9476-090798c5ebf3}, provider
// {03f0fe74-fdd9-4e5c-b630-f9339c47acc5}:
//
//   4 XDisplayTryEnableHdrMode(XDisplayHdrModePreference, XDisplayHdrModeInfo*)
//     -> XDisplayHdrModeResult (documented: 0 unknown, 1 enabled, 2 disabled;
//     observed: titles compare the result with 1)
//
// The graphics library presents standard-dynamic-range images, so HDR output
// is reported disabled, which is what the platform reports for a display
// without HDR. The documented info struct (three float luminance values) is
// zeroed.
#include <windows.h>

#include <cstring>

#include "core/libs/support.h"
#include "core/libs/xgameruntime/provider.h"
#include "core/libs/xgameruntime/services.h"

namespace holy::xgr {
namespace {

constexpr uint32_t kHdrDisabled = 2;

struct HdrModeInfo {
  float min_tone_map_luminance;
  float max_tone_map_luminance;
  float max_full_frame_tone_map_luminance;
};

uint32_t STDMETHODCALLTYPE try_enable_hdr_mode(Object*, uint32_t preference, HdrModeInfo* info) {
  if (info) std::memset(info, 0, sizeof(*info));
  LIB_LOG(holy::lib::kDebug, "xdisplay", "HDR mode requested (preference %u): disabled", preference);
  return kHdrDisabled;
}

}  // namespace

void register_display() {
  // {35f07670-706e-4bfb-9476-090798c5ebf3}
  constexpr GUID kDisplay = {0x35f07670, 0x706e, 0x4bfb, {0x94, 0x76, 0x09, 0x07, 0x98, 0xc5, 0xeb, 0xf3}};
  Interface& display = define_interface(kDisplay, "display");
  set_method(display, 4, "XDisplayTryEnableHdrMode", &try_enable_hdr_mode);
}

}  // namespace holy::xgr
