// Launchpad pad map: which pad does what, and what colour it shows.
//
// Coordinates: x 0-7 left->right, y 0-7 bottom->top for the 8x8 grid; x == 8 is the
// right-hand column of round buttons, y == 8 the top row. The same map drives the
// physical Launchpad and the virtual one in the UI. Stored as data/padmap.json
// (same format as the Python version, so an edited map keeps working). Actions:
//
//   toggle      {"fx": id}                               effect on/off
//   hold        {"fx": id}                               effect on while held
//   hold_param  {"fx": id, "param": p, "value": v, "also": {p: v}}   params while held
//   param       {"fx": id, "param": p, "value": v}       set a parameter
//   tone        {"wave": w, "freq": f}                   tone generator preset
//   shape       {"shape": s, "a": n, "b": n, "freq": f}  X-Y scope shape
//   preset      {"slot": 1-8}                            recall preset slot
//   volume      {"db": v}            volume_step {"delta": dB}       DAC volume
//   mute {}     kill {} (mute while held)     bypass_all {}     all_off {}
//   tap {}      delay_div {"div": 0.25}       delay_step {"factor": 1.25}
//   source      {"kind": tone|file|capture}   file_next {}  file_prev {}
//   cycle       {"fx": id, "param": p}        panic {}
//   view        {"view": wave|heat|xy|spectrum|spectrogram}       scope view
//   timebase    {"factor": 2.0}                                    window x factor
#pragma once
#include <map>
#include <string>
#include <utility>

#include "nlohmann/json.hpp"

namespace pifx {

using json = nlohmann::json;

// Launchpad RGB palette indices (MK2/MK3/X/Pro). Hues come in [light, full, dim, dimmer].
int palette(const std::string& color);
int dim(const std::string& color);
std::string fxColorName(const std::string& fxId);
// Approximate sRGB of a palette index (for the on-screen pad grid).
unsigned paletteRgb(int index);

std::string padKey(int x, int y);
json defaultPadMap();
json loadPadMap(const std::string& path);   // writes defaults on first run

struct PadContext {
    std::map<std::string, bool> fxOn;
    std::map<std::pair<int, int>, bool> held;
    std::string sourceKind;                  // tone | file | capture | silence
    int toneMode = 0;                        // 0 tone, 1 shape
    std::string wave, shape;
    float freq = 0;
    int a = 1, b = 1;
    int preset = 0;                          // current slot, 0 = none
    double volumeDb = -1e9;
    bool muted = false, bypassAll = false, tapFlash = false;
    std::string view;
};
int padColor(const json& action, int x, int y, const PadContext& ctx);

}  // namespace pifx
