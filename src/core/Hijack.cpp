#include "core/Hijack.h"

#include <cstdio>

#include "core/Util.h"
#include "nlohmann/json.hpp"

namespace pifx {

using json = nlohmann::json;

std::string Hijack::pactl(const std::string& args, int* status) { return runCapture("pactl " + args, status); }

bool Hijack::isVirtualSink(const std::string& s) {
    for (const char* v : {kPersistentSink, kSink}) {
        const std::string n = v;
        if (s == n || s == n + ".monitor" || startsWith(s, n + ".")) return true;
    }
    return s == "pi-dsp" || s == "pifx-hijack";   // descriptions, as shown in device lists
}

bool Hijack::available(std::string* why) {
    if (!which("pactl")) {
        if (why) *why = "pactl not found (sudo apt install pulseaudio-utils)";
        return false;
    }
    int st = 0;
    pactl("info", &st);
    if (st != 0) {
        if (why) *why = "no PipeWire/PulseAudio server for this user";
        return false;
    }
    return true;
}

std::string Hijack::defaultSink() {
    int st = 0;
    std::string s = trim(pactl("get-default-sink", &st));
    if (st == 0 && !s.empty() && s.find('\n') == std::string::npos) return s;
    for (const auto& line : splitLines(pactl("info")))
        if (startsWith(line, "Default Sink:")) return trim(line.substr(13));
    return "";
}

int Hijack::sinkIndex(const std::string& name) {
    for (const auto& line : splitLines(pactl("list short sinks"))) {
        auto cols = split(line, '\t');
        if (cols.size() >= 2 && cols[1] == name) return std::atoi(cols[0].c_str());
    }
    return -1;
}

bool Hijack::persistentSinkExists() { return which("pactl") && sinkIndex(kPersistentSink) >= 0; }

std::string Hijack::firstRealSink() {
    for (const auto& line : splitLines(pactl("list short sinks"))) {
        auto cols = split(line, '\t');
        if (cols.size() >= 2 && !isVirtualSink(cols[1])) return cols[1];
    }
    return "";
}

std::vector<SinkInput> Hijack::parseSinkInputsJson(const std::string& text) {
    std::vector<SinkInput> out;
    json j = json::parse(text, nullptr, false);
    if (!j.is_array()) return out;
    for (const auto& e : j) {
        SinkInput si;
        si.index = e.value("index", -1);
        si.sink = e.value("sink", -1);
        if (e.contains("properties") && e["properties"].is_object())
            si.app = e["properties"].value("application.name", "");
        if (si.index >= 0) out.push_back(si);
    }
    return out;
}

std::vector<SinkInput> Hijack::parseSinkInputsText(const std::string& text) {
    // `pactl list sink-inputs` (older pactl without -f json)
    std::vector<SinkInput> out;
    for (const auto& raw : splitLines(text)) {
        std::string line = trim(raw);
        if (startsWith(line, "Sink Input #")) {
            SinkInput si;
            si.index = std::atoi(line.c_str() + 12);
            out.push_back(si);
        } else if (!out.empty() && startsWith(line, "Sink:")) {
            out.back().sink = std::atoi(trim(line.substr(5)).c_str());
        } else if (!out.empty() && startsWith(line, "application.name = ")) {
            std::string v = trim(line.substr(19));
            if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
            out.back().app = v;
        }
    }
    return out;
}

std::vector<SinkInput> Hijack::sinkInputs() {
    int st = 0;
    std::string j = pactl("-f json list sink-inputs", &st);
    if (st == 0 && !trim(j).empty() && trim(j)[0] == '[') return parseSinkInputsJson(j);
    return parseSinkInputsText(pactl("list sink-inputs"));
}

void Hijack::writeState() const {
    if (stateFile_.empty()) return;
    json st = {{"module", module_}, {"sink", sink_}, {"previous_sink", prevSink_}};
    writeFile(stateFile_, st.dump(1));
}

bool Hijack::start(std::string* err, const std::string& fallbackSink) {
    if (active_) return true;
    std::string why;
    if (!available(&why)) {
        if (err) *err = why;
        return false;
    }
    // Where audio goes back to on stop(): the current default, unless that already is
    // one of ours (remembered by WirePlumber from an earlier run).
    prevSink_ = defaultSink();
    if (isVirtualSink(prevSink_)) {
        prevSink_ = !fallbackSink.empty() && !isVirtualSink(fallbackSink) ? fallbackSink : firstRealSink();
    }
    module_ = -1;
    if (sinkIndex(kPersistentSink) >= 0) {
        sink_ = kPersistentSink;                      // the permanent dummy output from PipeWire's config
    } else {
        int st = 0;
        std::string out = pactl(std::string("load-module module-null-sink sink_name=") + kSink +
                                    " sink_properties=device.description=pifx-hijack rate=48000 channels=2",
                                &st);
        if (st != 0) {
            if (err) *err = "could not create the hijack sink: " + trim(out);
            return false;
        }
        module_ = std::atoi(trim(out).c_str());
        sink_ = kSink;
    }
    pactl("set-default-sink " + shellQuote(sink_));
    const int target = sinkIndex(sink_);
    moved_ = 0;
    for (const auto& si : sinkInputs()) {
        if (si.app == "pifx" || si.sink == target) continue;   // never move our own playback
        int s = 0;
        pactl(format("move-sink-input %d ", si.index) + shellQuote(sink_), &s);
        if (s == 0) moved_++;
    }
    active_ = true;
    writeState();
    return true;
}

void Hijack::stop() {
    if (!active_) return;
    const int ours = sinkIndex(sink_);
    if (!prevSink_.empty()) {
        pactl("set-default-sink " + shellQuote(prevSink_));
        for (const auto& si : sinkInputs())
            if (si.sink == ours && si.app != "pifx") pactl(format("move-sink-input %d ", si.index) + shellQuote(prevSink_));
    }
    if (module_ >= 0) pactl(format("unload-module %d", module_));   // the permanent pi_dsp sink stays
    active_ = false;
    module_ = -1;
    moved_ = 0;
    if (!stateFile_.empty()) std::remove(stateFile_.c_str());
}

std::string Hijack::repair(const std::string& stateFile) {
    std::string text;
    if (!readFile(stateFile, text)) return "";
    json j = json::parse(text, nullptr, false);
    std::remove(stateFile.c_str());
    if (!j.is_object() || !which("pactl")) return "";
    Hijack h;
    h.module_ = j.value("module", -1);
    h.sink_ = j.value("sink", std::string(h.module_ >= 0 ? kSink : kPersistentSink));
    h.prevSink_ = j.value("previous_sink", "");
    h.active_ = true;
    h.stop();
    return "restored audio routing left behind by a previous pifx run (default output: " +
           (h.prevSink_.empty() ? std::string("unchanged") : h.prevSink_) + ")";
}

}  // namespace pifx
