#include "core/PadMap.h"

#include <cmath>

#include "core/Util.h"

namespace pifx {

static const std::map<std::string, int>& pal() {
    static const std::map<std::string, int> p = {
        {"off", 0},   {"grey", 1},  {"white", 3}, {"red", 5},     {"orange", 9},  {"yellow", 13},
        {"lime", 17}, {"green", 21}, {"mint", 25}, {"cyan", 37},  {"sky", 41},    {"blue", 45},
        {"violet", 49}, {"purple", 53}, {"pink", 57}, {"rose", 61}};
    return p;
}

int palette(const std::string& color) {
    auto it = pal().find(color);
    return it == pal().end() ? 1 : it->second;
}

int dim(const std::string& color) {
    int p = palette(color);
    return p >= 4 ? p + 2 : p;
}

std::string fxColorName(const std::string& fx) {
    static const std::map<std::string, std::string> m = {
        {"filter", "cyan"}, {"eq", "orange"}, {"drive", "red"},    {"tremolo", "violet"},
        {"delay", "blue"},  {"crush", "pink"}, {"stutter", "yellow"}, {"reverb", "green"}};
    auto it = m.find(fx);
    return it == m.end() ? "white" : it->second;
}

unsigned paletteRgb(int idx) {
    if (idx <= 0) return 0x202020;
    if (idx == 1) return 0x606060;
    if (idx == 2) return 0xa0a0a0;
    if (idx == 3) return 0xffffff;
    static const unsigned hues[15] = {0xff2020, 0xff8a1c, 0xffe020, 0xa8ff20, 0x20ff40, 0x20ffa0, 0x20ffd0, 0x20e8ff,
                                      0x20c8ff, 0x2090ff, 0x2040ff, 0x7a30ff, 0xc030ff, 0xff30c0, 0xff3070};
    int h = std::min(14, (idx - 4) / 4), v = (idx - 4) % 4;
    unsigned c = hues[h];
    float r = (float)((c >> 16) & 255), g = (float)((c >> 8) & 255), b = (float)(c & 255);
    float k = v == 0 ? 1.0f : v == 1 ? 1.0f : v == 2 ? 0.38f : 0.18f;
    if (v == 0) { r = r * 0.6f + 102; g = g * 0.6f + 102; b = b * 0.6f + 102; }   // light = pastel
    auto ch = [&](float x) { return (unsigned)std::min(255.0f, x * k); };
    return ch(r) << 16 | ch(g) << 8 | ch(b);
}

std::string padKey(int x, int y) { return std::to_string(x) + "," + std::to_string(y); }

json defaultPadMap() {
    json m = json::object();
    static const char* fxOrder[] = {"filter", "eq", "drive", "tremolo", "delay", "crush", "stutter", "reverb"};
    for (int x = 0; x < 8; x++) m[padKey(x, 7)] = {{"action", "toggle"}, {"fx", fxOrder[x]}, {"label", fxOrder[x]}};
    json holds = json::array({
        {{"action", "hold"}, {"fx", "stutter"}, {"label", "stutter"}},
        {{"action", "hold_param"}, {"fx", "filter"}, {"param", "cutoff"}, {"value", 250}, {"label", "LP 250"},
         {"also", {{"mode", "lowpass"}, {"q", 1.2}}}},
        {{"action", "hold_param"}, {"fx", "filter"}, {"param", "cutoff"}, {"value", 2500}, {"label", "HP 2.5k"},
         {"also", {{"mode", "highpass"}, {"q", 1.0}}}},
        {{"action", "hold_param"}, {"fx", "delay"}, {"param", "feedback"}, {"value", 0.9}, {"label", "dly throw"},
         {"also", {{"mix", 0.8}}}},
        {{"action", "hold_param"}, {"fx", "reverb"}, {"param", "size"}, {"value", 1.0}, {"label", "rvb freeze"},
         {"also", {{"mix", 0.7}}}},
        {{"action", "hold"}, {"fx", "crush"}, {"label", "crush"}},
        {{"action", "hold_param"}, {"fx", "tremolo"}, {"param", "rate"}, {"value", 14}, {"label", "trem fast"},
         {"also", {{"depth", 1.0}, {"shape", "square"}}}},
        {{"action", "kill"}, {"label", "kill"}}});
    for (int x = 0; x < 8; x++) m[padKey(x, 6)] = holds[x];
    const std::pair<const char*, int> tones[] = {{"sine", 440}, {"sine", 1000}, {"sine", 100}, {"square", 220},
                                                 {"saw", 110},  {"white", 0},   {"pink", 0},   {"sweep", 0}};
    for (int x = 0; x < 8; x++) {
        std::string lbl = tones[x].second ? format("%s %d", tones[x].first, tones[x].second) : tones[x].first;
        m[padKey(x, 5)] = {{"action", "tone"}, {"wave", tones[x].first}, {"freq", tones[x].second}, {"label", lbl}};
    }
    json shapes = json::array({{{"shape", "circle"}}, {{"shape", "lissajous"}, {"a", 1}, {"b", 2}},
                               {{"shape", "lissajous"}, {"a", 2}, {"b", 3}}, {{"shape", "lissajous"}, {"a", 3}, {"b", 4}},
                               {{"shape", "rose"}, {"a", 3}}, {{"shape", "rose"}, {"a", 5}},
                               {{"shape", "star"}, {"a", 5}}, {{"shape", "square"}}});
    for (int x = 0; x < 8; x++) {
        json s = shapes[x];
        std::string lbl = s["shape"].get<std::string>();
        if (s.contains("b")) lbl += format(" %d:%d", s["a"].get<int>(), s["b"].get<int>());
        else if (s.contains("a")) lbl += format(" %d", s["a"].get<int>());
        s["action"] = "shape";
        s["freq"] = 100;
        s["label"] = lbl;
        m[padKey(x, 4)] = s;
    }
    for (int x = 0; x < 8; x++) m[padKey(x, 3)] = {{"action", "preset"}, {"slot", x + 1}, {"label", format("preset %d", x + 1)}};
    const std::pair<double, const char*> divs[] = {{1.0 / 32, "1/32"}, {1.0 / 16, "1/16"}, {1.0 / 12, "1/8T"}, {1.0 / 8, "1/8"},
                                                   {1.0 / 6, "1/4T"},  {1.0 / 4, "1/4"},   {3.0 / 8, "3/8"},   {1.0 / 2, "1/2"}};
    for (int x = 0; x < 8; x++) m[padKey(x, 2)] = {{"action", "delay_div"}, {"div", divs[x].first}, {"label", divs[x].second}};
    const int cut[] = {80, 160, 320, 640, 1250, 2500, 5000, 10000};
    for (int x = 0; x < 8; x++)
        m[padKey(x, 1)] = {{"action", "param"}, {"fx", "filter"}, {"param", "cutoff"}, {"value", cut[x]},
                           {"label", cut[x] < 1000 ? format("%d Hz", cut[x]) : format("%g kHz", cut[x] / 1000.0)}};
    const int vols[] = {-40, -30, -24, -18, -12, -9, -6, 0};
    for (int x = 0; x < 8; x++) m[padKey(x, 0)] = {{"action", "volume"}, {"db", vols[x]}, {"label", format("%d dB", vols[x])}};
    json right = json::array({{{"action", "panic"}, {"label", "panic"}},
                              {{"action", "file_prev"}, {"label", "prev file"}},
                              {{"action", "file_next"}, {"label", "next file"}},
                              {{"action", "source"}, {"kind", "capture"}, {"label", "src: input"}},
                              {{"action", "source"}, {"kind", "file"}, {"label", "src: file"}},
                              {{"action", "source"}, {"kind", "tone"}, {"label", "src: tone"}},
                              {{"action", "bypass_all"}, {"label", "bypass"}},
                              {{"action", "mute"}, {"label", "mute"}}});
    for (int y = 0; y < 8; y++) m[padKey(8, y)] = right[y];
    json top = json::array({{{"action", "volume_step"}, {"delta", 3}, {"label", "vol +"}},
                            {{"action", "volume_step"}, {"delta", -3}, {"label", "vol -"}},
                            {{"action", "delay_step"}, {"factor", 0.8}, {"label", "dly -"}},
                            {{"action", "delay_step"}, {"factor", 1.25}, {"label", "dly +"}},
                            {{"action", "tap"}, {"label", "tap"}},
                            {{"action", "cycle"}, {"fx", "filter"}, {"param", "mode"}, {"label", "filt mode"}},
                            {{"action", "cycle"}, {"fx", "drive"}, {"param", "mode"}, {"label", "drive mode"}},
                            {{"action", "all_off"}, {"label", "all off"}}});
    for (int x = 0; x < 8; x++) m[padKey(x, 8)] = top[x];
    return m;
}

json loadPadMap(const std::string& path) {
    std::string text;
    if (readFile(path, text)) {
        json j = json::parse(text, nullptr, false);
        if (j.is_object() && !j.empty()) return j;
    }
    json m = defaultPadMap();
    makeDirs(parentDir(path));
    writeFile(path, m.dump(1));
    return m;
}

int padColor(const json& a, int x, int y, const PadContext& ctx) {
    const std::string act = a.value("action", "");
    const std::string fx = a.value("fx", "");
    const std::string col = fxColorName(fx);
    auto it = ctx.held.find({x, y});
    const bool held = it != ctx.held.end() && it->second;
    auto num = [&](const char* k, double d) { return a.contains(k) && a[k].is_number() ? a[k].get<double>() : d; };
    if (act == "toggle") {
        auto f = ctx.fxOn.find(fx);
        return f != ctx.fxOn.end() && f->second ? palette(col) : dim(col);
    }
    if (act == "hold" || act == "hold_param") return held ? palette("white") : dim(col);
    if (act == "param") return held ? palette("cyan") : palette("grey");
    if (act == "tone") {
        double f = num("freq", 0);
        bool on = ctx.sourceKind == "tone" && ctx.toneMode == 0 && ctx.wave == a.value("wave", "") &&
                  (f == 0 || std::fabs(ctx.freq - f) < 0.5);
        return on ? palette("sky") : dim("sky");
    }
    if (act == "shape") {
        bool on = ctx.sourceKind == "tone" && ctx.toneMode == 1 && ctx.shape == a.value("shape", "") &&
                  ctx.a == (int)num("a", ctx.a) && ctx.b == (int)num("b", ctx.b);
        return on ? palette("purple") : dim("purple");
    }
    if (act == "preset") return ctx.preset == (int)num("slot", -1) ? palette("yellow") : dim("yellow");
    if (act == "delay_div") return held ? palette("blue") : palette("grey");
    if (act == "volume") return ctx.volumeDb >= num("db", 0) - 0.01 ? palette("green") : dim("green");
    if (act == "volume_step") return palette("white");
    if (act == "mute") return ctx.muted ? palette("red") : dim("red");
    if (act == "kill") return held ? palette("red") : dim("red");
    if (act == "bypass_all") return ctx.bypassAll ? palette("orange") : dim("orange");
    if (act == "all_off" || act == "panic") return dim("red");
    if (act == "source") return ctx.sourceKind == a.value("kind", "") ? palette("lime") : dim("lime");
    if (act == "file_next" || act == "file_prev") return dim("lime");
    if (act == "tap") return ctx.tapFlash ? palette("white") : palette("grey");
    if (act == "view") return ctx.view == a.value("view", "") ? palette("mint") : dim("mint");
    return palette("grey");
}

}  // namespace pifx
