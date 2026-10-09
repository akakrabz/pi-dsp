#include "core/Rig.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "core/Util.h"

namespace pifx {

double nowSeconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// A small demo loop so the file source has something to play on a fresh install.
static void writeDemoWav(const std::string& path, int sr, double seconds) {
    const int n = (int)(sr * seconds);
    std::vector<int16_t> s((size_t)n);
    for (int i = 0; i < n; i++) {
        double t = (double)i / sr;
        double beat = std::sin(2 * M_PI * 2 * t) > 0.8 ? 1.0 : 0.0;
        double v = std::sin(2 * M_PI * 220 * t) * 0.3 + std::sin(2 * M_PI * 330 * t) * 0.2 * beat;
        s[i] = (int16_t)(v * 0.6 * 32767);
    }
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f); u32(36 + (uint32_t)n * 2); std::fwrite("WAVEfmt ", 1, 8, f);
    u32(16); u16(1); u16(1); u32((uint32_t)sr); u32((uint32_t)sr * 2); u16(2); u16(16);
    std::fwrite("data", 1, 4, f); u32((uint32_t)n * 2);
    std::fwrite(s.data(), 2, (size_t)n, f);
    std::fclose(f);
}

static std::string safeName(const std::string& n) {
    std::string out;
    for (char c : n)
        if (std::isalnum((unsigned char)c) || c == '-' || c == '_' || c == ' ') out += c;
    out = trim(out);
    if (out.size() > 40) out.resize(40);
    return out.empty() ? "preset" : out;
}

// ---------------------------------------------------------------- lifecycle
Rig::Rig(Options o) : opt_(std::move(o)), hijack_(joinPath(opt_.dataDir, "hijack.json")) {
    makeDirs(joinPath(opt_.dataDir, "presets"));
    makeDirs(opt_.mediaDir);
    if (mediaFiles().empty()) writeDemoWav(joinPath(opt_.mediaDir, "demo-tone-loop.wav"), 48000, 4.0);

    std::string text;
    if (readFile(joinPath(opt_.dataDir, "settings.json"), text)) settings_ = json::parse(text, nullptr, false);
    if (!settings_.is_object()) settings_ = json::object();

    engine_ = std::make_unique<Engine>(opt_.sampleRate, 2048);
    io_ = std::make_unique<AudioIO>(*engine_, opt_.block);
    hat_ = detectHat();
    mixer_ = mixerFor(hat_);
    padMap_ = loadPadMap(joinPath(opt_.dataDir, "padmap.json"));
    for (const auto& d : effectDescs()) {
        FxState s;
        for (const auto& p : d.params) s.values.push_back(p.def);
        fx_.push_back(s);
    }
    tapBuf_.resize(4096 * kTapChannels);
}

Rig::~Rig() { shutdown(); }

void Rig::start() {
    if (started_) return;
    started_ = true;
    std::string msg = Hijack::repair(joinPath(opt_.dataDir, "hijack.json"));
    if (!msg.empty()) notify(msg);

    if (opt_.audio) {
        std::string b = opt_.backend != "auto" ? opt_.backend : settings_.value("backend", std::string("auto"));
        std::string err;
        if (!io_->init(backendFromName(b), &err)) notify(err);
    }
    setMasterDb(settings_.value("master_db", -6.0f));
    outGainDb_ = settings_.value("output_gain_db", 0.0f);

    // startup source
    std::string src = opt_.source.empty() ? "tone" : opt_.source;
    if (src == "shape") { ToneParams p; p.mode = 1; p.shape = 1; p.a = 3; p.b = 2; p.freq = 100; setTone(p); }
    else if (src == "sweep") { ToneParams p; p.wave = 6; p.sweepSec = 4; p.sweepLo = 40; p.sweepHi = 16000; setTone(p); }
    else if (src == "noise") { ToneParams p; p.wave = 5; setTone(p); }
    else if (startsWith(src, "file")) {
        auto files = mediaFiles();
        std::string name = src.size() > 5 ? src.substr(5) : (files.empty() ? "" : files[0]);
        if (!name.empty()) playFile(name);
        else setTone(tone_);
    } else if (startsWith(src, "capture")) {
        std::string err;
        if (!useCapture(src.size() > 8 ? src.substr(8) : opt_.input, &err)) { notify(err); setTone(tone_); }
    } else if (src == "silence") useSilence();
    else setTone(tone_);

    chooseInitialRouting();
    if (!opt_.preset.empty() && !loadPreset(opt_.preset)) notify("preset not found: " + opt_.preset);

    if (opt_.launchpad) {
        lp_ = std::make_unique<LaunchpadManager>(opt_.midiPort);
        lp_->start();
    }
}

void Rig::chooseInitialRouting() {
    std::vector<std::string> keys;
    if (!opt_.audio) {
        io_->setOutputs({}, nullptr);
        return;
    }
    if (opt_.outputsGiven) {
        for (const auto& k : opt_.outputs)
            if (k != "none") keys.push_back(k);
    } else if (settings_.contains("outputs") && settings_["outputs"].is_array()) {
        for (const auto& k : settings_["outputs"])
            if (k.is_string() && k.get<std::string>() != "none" && io_->findPlayback(k.get<std::string>()))
                keys.push_back(k.get<std::string>());
        if (keys.empty() && !settings_["outputs"].empty() && settings_["outputs"][0] == "none") {
            io_->setOutputs({}, nullptr);
            return;
        }
    }
    if (keys.empty() && !opt_.outputsGiven) {
        // The DAC HAT if we can see it, else the system default.
        if (hat_.detected && hat_.card) {
            for (const auto& d : io_->playback())
                if (contains(d.name, hat_.card->id) || contains(d.key, hat_.card->id) || contains(d.name, hat_.card->name)) {
                    keys.push_back(d.key);
                    break;
                }
        }
        if (keys.empty())
            for (const auto& d : io_->playback())
                if (d.isDefault) { keys.push_back(d.key); break; }
        if (keys.empty() && !io_->playback().empty()) keys.push_back(io_->playback()[0].key);
    }
    std::string err;
    io_->setOutputs(keys, &err);
    if (!err.empty()) notify(err);
    setOutputGainAll(outGainDb_);
}

void Rig::shutdown() {
    if (!started_) return;
    started_ = false;
    if (lp_) lp_->stop();
    if (hijack_.active()) stopHijack();
    if (sourceKind() == SourceKind::Capture) engine_->retire(engine_->source());
    io_->shutdown();
    engine_->flushOffline();
    saveSettings();
}

void Rig::update() {
    // scope feed
    FrameRing& tap = engine_->tap();
    for (int guard = 0; guard < 64; guard++) {
        size_t n = tap.read(tapBuf_.data(), 4096);
        if (n == 0) break;
        history_.append(tapBuf_.data(), n);
    }
    engine_->collect();

    if (loading_.valid() && loading_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        auto clip = loading_.get();
        if (clip) {
            engine_->setSource(new FileSource(clip));
            auto files = mediaFiles();
            auto it = std::find(files.begin(), files.end(), clip->name);
            if (it != files.end()) fileIndex_ = (int)(it - files.begin());
            preset_ = 0;
        } else {
            notify(loadErr_ && !loadErr_->empty() ? *loadErr_ : "could not load " + loadingName_);
        }
        loadingName_.clear();
    }

    std::string m = io_->poll();
    if (!m.empty()) notify(m);

    if (lp_) {
        PadEvent e;
        while (lp_->popEvent(e)) padEvent(e.x, e.y, e.pressed);
        lp_->setColors(padColors());
    }
    const double now = nowSeconds();
    while (!messages_.empty() && now - messages_.front().second > 8.0) messages_.pop_front();
}

void Rig::notify(const std::string& msg) {
    if (msg.empty()) return;
    std::fprintf(stderr, "pifx: %s\n", msg.c_str());
    messages_.push_back({msg, nowSeconds()});
    while (messages_.size() > 6) messages_.pop_front();
}

void Rig::saveSettings() {
    settings_["master_db"] = masterDb_;
    settings_["output_gain_db"] = outGainDb_;
    writeFile(joinPath(opt_.dataDir, "settings.json"), settings_.dump(1));
}

// ---------------------------------------------------------------- effects
void Rig::postFx(int fx, int param, float v) {
    Command c;
    c.type = Cmd::FxParam;
    c.fx = fx;
    c.param = param;
    c.value = v;
    engine_->post(c);
}

void Rig::setParam(int fx, int param, float value) {
    if (fx < 0 || fx >= (int)fx_.size()) return;
    const auto& spec = effectDescs()[fx].params;
    if (param < 0 || param >= (int)spec.size()) return;
    float v = spec[param].clamp(value);
    fx_[fx].values[param] = v;
    preset_ = 0;
    postFx(fx, param, v);
}

bool Rig::setParam(const std::string& fx, const std::string& param, const json& value) {
    int f = effectIndex(fx);
    if (f < 0) return false;
    int p = effectDescs()[f].paramIndex(param);
    if (p < 0) return false;
    const ParamSpec& s = effectDescs()[f].params[p];
    float v;
    if (value.is_string()) {
        int i = s.choiceIndex(value.get<std::string>());
        if (i < 0) return false;
        v = (float)i;
    } else if (value.is_number()) {
        v = value.get<float>();
    } else if (value.is_boolean()) {
        v = value.get<bool>() ? 1.0f : 0.0f;
    } else {
        return false;
    }
    setParam(f, p, v);
    return true;
}

void Rig::setEnabled(int fx, bool on) {
    if (fx < 0 || fx >= (int)fx_.size()) return;
    fx_[fx].enabled = on;
    Command c;
    c.type = Cmd::FxEnable;
    c.fx = fx;
    c.flag = on;
    engine_->post(c);
}

void Rig::setBypassAll(bool on) {
    bypassAll_ = on;
    Command c;
    c.type = Cmd::BypassAll;
    c.flag = on;
    engine_->post(c);
}

void Rig::allOff() {
    for (int i = 0; i < (int)fx_.size(); i++) setEnabled(i, false);
}

void Rig::panic() {
    for (size_t i = 0; i < fx_.size(); i++) {
        fx_[i].enabled = false;
        const auto& spec = effectDescs()[i].params;
        for (size_t p = 0; p < spec.size(); p++) fx_[i].values[p] = spec[p].def;
    }
    bypassAll_ = false;
    held_.clear();
    preset_ = 0;
    Command c;
    c.type = Cmd::Panic;
    engine_->post(c);
}

void Rig::cycle(const std::string& fx, const std::string& param) {
    int f = effectIndex(fx);
    if (f < 0) return;
    int p = effectDescs()[f].paramIndex(param);
    if (p < 0 || !effectDescs()[f].params[p].isEnum()) return;
    int n = (int)effectDescs()[f].params[p].choices.size();
    setParam(f, p, (float)(((int)fx_[f].values[p] + 1) % n));
}

void Rig::setMasterDb(float db) {
    masterDb_ = std::max(-60.0f, std::min(12.0f, db));
    Command c;
    c.type = Cmd::MasterDb;
    c.value = masterDb_;
    engine_->post(c);
}

// ---------------------------------------------------------------- tempo
void Rig::tap() {
    const double now = nowSeconds();
    std::vector<double> keep;
    for (double t : taps_)
        if (now - t >= 0 && now - t < 2.5) keep.push_back(t);
    keep.push_back(now);
    taps_ = keep;
    double sum = 0;
    int n = 0;
    for (size_t i = 1; i < taps_.size(); i++) {
        double g = taps_[i] - taps_[i - 1];
        if (g > 0.05) { sum += g; n++; }
    }
    if (n) tempo_ = (float)std::max(30.0, std::min(300.0, 60.0 / (sum / n)));
}

void Rig::setTempo(float bpm) { tempo_ = std::max(30.0f, std::min(300.0f, bpm)); }

bool Rig::tapFlash() const { return !taps_.empty() && nowSeconds() - taps_.back() < 0.15; }

void Rig::delayDiv(double div) {
    const int f = effectIndex("delay");
    const int p = effectDescs()[f].paramIndex("time");
    setParam(f, p, (float)(60000.0 / tempo_ * 4 * div));
    if (!fx_[f].enabled) setEnabled(f, true);
}

// ---------------------------------------------------------------- sources
SourceKind Rig::sourceKind() const { return engine_->source()->kind(); }

void Rig::setTone(const ToneParams& p) {
    tone_ = p;
    if (sourceKind() != SourceKind::Tone) {
        auto* t = new ToneSource(engine_->sampleRate());
        t->setParams(p);
        engine_->setSource(t);
    } else {
        Command c;
        c.type = Cmd::ToneParams;
        c.tone = p;
        engine_->post(c);
    }
    preset_ = 0;
}

void Rig::useSilence() { engine_->setSource(new SilenceSource()); }

std::vector<std::string> Rig::mediaFiles() const {
    std::vector<std::string> out;
    for (const auto& n : listDir(opt_.mediaDir))
        if (isAudioFile(n)) out.push_back(n);
    return out;
}

void Rig::playFile(const std::string& pathOrName) {
    std::string path = pathOrName.find('/') == std::string::npos ? joinPath(opt_.mediaDir, pathOrName) : pathOrName;
    if (loading_.valid()) loading_.wait();
    loadingName_ = pathOrName.substr(pathOrName.find_last_of('/') == std::string::npos ? 0 : pathOrName.find_last_of('/') + 1);
    loadErr_ = std::make_shared<std::string>();
    auto err = loadErr_;
    const int sr = engine_->sampleRate();
    loading_ = std::async(std::launch::async, [path, sr, err] { return loadClip(path, sr, err.get()); });
}

void Rig::stepFile(int delta) {
    auto files = mediaFiles();
    if (files.empty()) {
        notify("no audio files in " + opt_.mediaDir);
        return;
    }
    fileIndex_ = ((fileIndex_ + delta) % (int)files.size() + (int)files.size()) % (int)files.size();
    playFile(files[fileIndex_]);
}

FileSource* Rig::file() {
    Source* s = engine_->source();
    return s->kind() == SourceKind::File ? static_cast<FileSource*>(s) : nullptr;
}

CaptureSource* Rig::capture() {
    Source* s = engine_->source();
    return s->kind() == SourceKind::Capture ? static_cast<CaptureSource*>(s) : nullptr;
}

bool Rig::useCapture(const std::string& key, std::string* err) {
    auto src = io_->openCapture(key, err);
    if (!src) return false;
    captureKey_ = key;
    engine_->setSource(src.release());
    if (key != Hijack::kMonitor) {
        settings_["input"] = key;
        saveSettings();
    }
    return true;
}

// ---------------------------------------------------------------- hijack / routing
bool Rig::startHijack(std::string* err) {
    if (!io_->isPulse()) {
        if (err) *err = "hijack needs the PipeWire/PulseAudio backend (Routing > Backend)";
        return false;
    }
    if (!hijack_.start(err)) return false;
    io_->refresh();
    if (!useCapture(Hijack::kMonitor, err)) {
        hijack_.stop();
        io_->refresh();
        return false;
    }
    notify(format("hijacked system audio: %d stream(s) moved; new streams follow automatically", hijack_.movedStreams()));
    if (io_->outputKeys().empty()) notify("no output selected - pick one under Routing to hear it");
    return true;
}

void Rig::stopHijack() {
    if (capture() && captureKey_ == Hijack::kMonitor)
        engine_->retire(engine_->source());   // close our capture stream before the sink disappears
    hijack_.stop();
    if (io_) io_->refresh();
    setTone(tone_);
}

bool Rig::setOutputs(const std::vector<std::string>& keys, std::string* err) {
    std::vector<std::string> k;
    for (const auto& s : keys) {
        if (s == Hijack::kSink) {
            notify("skipped pifx-hijack as an output (it would feed back into itself)");
            continue;
        }
        k.push_back(s);
    }
    bool ok = io_->setOutputs(k, err);
    settings_["outputs"] = k.empty() ? json::array({"none"}) : json(k);
    setOutputGainAll(outGainDb_);
    if (outMuted_)
        for (size_t i = 0; i < k.size(); i++) io_->setOutputMute(i, true);
    saveSettings();
    return ok;
}

bool Rig::setBackend(const std::string& name, std::string* err) {
    if (hijack_.active()) stopHijack();
    const bool hadCapture = sourceKind() == SourceKind::Capture;
    if (hadCapture) engine_->retire(engine_->source());   // capture devices belong to the old context
    auto prev = io_->outputKeys();
    io_->shutdown();
    engine_->flushOffline();
    bool ok = io_->init(backendFromName(name), err);
    settings_["backend"] = name;
    std::vector<std::string> keys;
    for (const auto& k : prev)
        if (io_->findPlayback(k)) keys.push_back(k);
    if (keys.empty())
        for (const auto& d : io_->playback())
            if (d.isDefault) { keys.push_back(d.key); break; }
    std::string e2;
    io_->setOutputs(keys, &e2);
    if (!e2.empty()) notify(e2);
    setOutputGainAll(outGainDb_);
    if (hadCapture) setTone(tone_);
    saveSettings();
    return ok;
}

void Rig::setOutputGainAll(float db) {
    outGainDb_ = std::max(-60.0f, std::min(0.0f, db));
    auto n = io_->outputKeys().size();
    for (size_t i = 0; i < n; i++) io_->setOutputGain(i, outGainDb_);
}

// ---------------------------------------------------------------- DAC / output level
double Rig::volumeDb() const {
    if (mixer_->ok()) {
        auto v = mixer_->volumeDb();
        return v ? *v : -1e9;
    }
    return outGainDb_;
}

void Rig::setVolumeDb(double db) {
    if (mixer_->ok()) {
        if (!mixer_->setVolumeDb(db)) notify("mixer: " + mixer_->error());
    } else {
        setOutputGainAll((float)db);
    }
}

void Rig::stepVolume(double delta) {
    double cur = volumeDb();
    if (!std::isfinite(cur) || cur < -100) cur = -40;
    setVolumeDb(std::max(-60.0, cur + delta));
}

bool Rig::muted() const {
    if (mixer_->ok()) {
        auto m = mixer_->muted();
        return m && *m;
    }
    return outMuted_;
}

void Rig::setMute(bool on) {
    if (mixer_->ok()) {
        mixer_->setMute(on);
        return;
    }
    outMuted_ = on;
    auto n = io_->outputKeys().size();
    for (size_t i = 0; i < n; i++) io_->setOutputMute(i, on);
}

void Rig::rescanHat() {
    hat_ = detectHat();
    mixer_ = mixerFor(hat_);
}

// ---------------------------------------------------------------- presets
std::string Rig::presetPath(const std::string& name) const {
    return joinPath(joinPath(opt_.dataDir, "presets"), safeName(name) + ".json");
}

std::vector<std::string> Rig::presets() const {
    std::vector<std::string> out;
    for (const auto& n : listDir(joinPath(opt_.dataDir, "presets")))
        if (endsWith(n, ".json")) out.push_back(n.substr(0, n.size() - 5));
    return out;
}

json Rig::stateJson() const {
    json fx = json::array();
    const auto& d = effectDescs();
    for (size_t i = 0; i < d.size(); i++) {
        json params = json::object();
        for (size_t p = 0; p < d[i].params.size(); p++) {
            if (d[i].params[p].isEnum()) params[d[i].params[p].id] = (int)fx_[i].values[p];
            else params[d[i].params[p].id] = fx_[i].values[p];
        }
        fx.push_back({{"id", d[i].id}, {"enabled", fx_[i].enabled}, {"params", params}});
    }
    json src;
    const SourceKind k = sourceKind();
    if (k == SourceKind::File) {
        auto* f = const_cast<Rig*>(this)->file();
        src = {{"kind", "file"}, {"file", f->clip().name}, {"gain", f->gainDb.load()}};
    } else {
        src = {{"kind", k == SourceKind::Tone ? "tone" : sourceKindName(k)},
               {"mode", tone_.mode == 1 ? "shape" : "tone"},
               {"wave", kWaves[tone_.wave]},
               {"freq", tone_.freq},
               {"level", tone_.level},
               {"shape", kShapes[tone_.shape]},
               {"a", tone_.a},
               {"b", tone_.b},
               {"phase", tone_.phaseDeg},
               {"sweep_seconds", tone_.sweepSec},
               {"sweep_lo", tone_.sweepLo},
               {"sweep_hi", tone_.sweepHi}};
    }
    return {{"chain", {{"master_db", masterDb_}, {"bypass_all", bypassAll_}, {"fx", fx}}},
            {"source", src},
            {"tempo_bpm", tempo_}};
}

static int indexOf(const std::vector<std::string>& v, const std::string& s, int def) {
    for (size_t i = 0; i < v.size(); i++)
        if (v[i] == s) return (int)i;
    return def;
}

void Rig::applyStateJson(const json& j) {
    if (j.contains("chain") && j["chain"].is_object()) {
        const json& c = j["chain"];
        if (c.contains("master_db") && c["master_db"].is_number()) setMasterDb(c["master_db"].get<float>());
        if (c.contains("bypass_all") && c["bypass_all"].is_boolean()) setBypassAll(c["bypass_all"].get<bool>());
        if (c.contains("fx") && c["fx"].is_array()) {
            for (const auto& f : c["fx"]) {
                int i = effectIndex(f.value("id", ""));
                if (i < 0) continue;
                if (f.contains("params") && f["params"].is_object())
                    for (auto it = f["params"].begin(); it != f["params"].end(); ++it) setParam(f["id"].get<std::string>(), it.key(), it.value());
                if (f.contains("enabled") && f["enabled"].is_boolean()) setEnabled(i, f["enabled"].get<bool>());
            }
        }
    }
    if (j.contains("source") && j["source"].is_object()) {
        const json& s = j["source"];
        const std::string kind = s.value("kind", "");
        if (kind == "tone") {
            ToneParams p = tone_;
            p.mode = s.value("mode", std::string("tone")) == "shape" ? 1 : 0;
            if (s.contains("wave")) p.wave = indexOf(kWaves, s.value("wave", "sine"), p.wave);
            if (s.contains("shape")) p.shape = indexOf(kShapes, s.value("shape", "circle"), p.shape);
            p.freq = s.value("freq", p.freq);
            p.level = s.value("level", p.level);
            p.a = s.value("a", p.a);
            p.b = s.value("b", p.b);
            p.phaseDeg = s.value("phase", p.phaseDeg);
            p.sweepSec = s.value("sweep_seconds", p.sweepSec);
            p.sweepLo = s.value("sweep_lo", p.sweepLo);
            p.sweepHi = s.value("sweep_hi", p.sweepHi);
            setTone(p);
        } else if (kind == "file" && s.contains("file") && s["file"].is_string()) {
            playFile(s["file"].get<std::string>());
        }
    }
    if (j.contains("tempo_bpm") && j["tempo_bpm"].is_number()) setTempo(j["tempo_bpm"].get<float>());
}

bool Rig::savePreset(const std::string& name) {
    json data = stateJson();
    data["saved"] = (double)std::time(nullptr);
    bool ok = writeFile(presetPath(name), data.dump(1));
    std::string n = safeName(name);
    if (ok && startsWith(n, "slot")) preset_ = std::atoi(n.c_str() + 4);
    return ok;
}

bool Rig::loadPreset(const std::string& name) {
    std::string text;
    if (!readFile(presetPath(name), text)) return false;
    json j = json::parse(text, nullptr, false);
    if (!j.is_object()) return false;
    applyStateJson(j);
    std::string n = safeName(name);
    preset_ = startsWith(n, "slot") ? std::atoi(n.c_str() + 4) : 0;
    return true;
}

void Rig::deletePreset(const std::string& name) { std::remove(presetPath(name).c_str()); }

// ---------------------------------------------------------------- pads
std::string Rig::padLabel(int x, int y) const {
    auto it = padMap_.find(padKey(x, y));
    if (it == padMap_.end() || !it->is_object()) return "";
    return it->value("label", it->value("action", ""));
}

std::array<int, 81> Rig::padColors() const {
    PadContext ctx;
    const auto& d = effectDescs();
    for (size_t i = 0; i < d.size(); i++) ctx.fxOn[d[i].id] = fx_[i].enabled;
    for (const auto& h : held_) ctx.held[h.first] = true;
    ctx.sourceKind = sourceKindName(sourceKind());
    ctx.toneMode = tone_.mode;
    ctx.wave = kWaves[tone_.wave];
    ctx.shape = kShapes[tone_.shape];
    ctx.freq = tone_.freq;
    ctx.a = tone_.a;
    ctx.b = tone_.b;
    ctx.preset = preset_;
    ctx.volumeDb = volumeDb();
    ctx.muted = muted();
    ctx.bypassAll = bypassAll_;
    ctx.tapFlash = tapFlash();
    ctx.view = view;
    std::array<int, 81> out{};
    for (int y = 0; y < 9; y++)
        for (int x = 0; x < 9; x++) {
            if (x == 8 && y == 8) continue;
            auto it = padMap_.find(padKey(x, y));
            out[y * 9 + x] = (it == padMap_.end() || !it->is_object()) ? 0 : padColor(*it, x, y, ctx);
        }
    return out;
}

void Rig::padEvent(int x, int y, bool pressed) {
    auto it = padMap_.find(padKey(x, y));
    if (it == padMap_.end() || !it->is_object()) return;
    try {
        if (pressed) pressPad(x, y, *it);
        else releasePad(x, y, *it);
    } catch (const std::exception& e) {
        notify(format("pad %d,%d: %s", x, y, e.what()));
    }
}

void Rig::pressPad(int x, int y, const json& a) {
    const std::string act = a.value("action", "");
    const std::string fx = a.value("fx", "");
    const int f = effectIndex(fx);
    const auto key = std::make_pair(x, y);
    if (act == "toggle" && f >= 0) setEnabled(f, !fx_[f].enabled);
    else if (act == "hold" && f >= 0) {
        held_[key] = {{"fx", fx}, {"was_enabled", fx_[f].enabled}};
        setEnabled(f, true);
    } else if (act == "hold_param" && f >= 0) {
        const std::string param = a.value("param", "");
        json saved = json::object();
        auto save = [&](const std::string& p) {
            int pi = effectDescs()[f].paramIndex(p);
            if (pi >= 0) saved[p] = fx_[f].values[pi];
        };
        save(param);
        if (a.contains("also") && a["also"].is_object())
            for (auto it = a["also"].begin(); it != a["also"].end(); ++it) save(it.key());
        held_[key] = {{"fx", fx}, {"was_enabled", fx_[f].enabled}, {"params", saved}};
        if (a.contains("also") && a["also"].is_object())
            for (auto it = a["also"].begin(); it != a["also"].end(); ++it) setParam(fx, it.key(), it.value());
        if (a.contains("value")) setParam(fx, param, a["value"]);
        setEnabled(f, true);
    } else if (act == "param" && f >= 0) {
        held_[key] = json::object();
        if (a.contains("value")) setParam(fx, a.value("param", ""), a["value"]);
    } else if (act == "tone") {
        ToneParams p = tone_;
        p.mode = 0;
        p.wave = indexOf(kWaves, a.value("wave", "sine"), 0);
        if (a.contains("freq") && a["freq"].is_number() && a["freq"].get<double>() > 0) p.freq = a["freq"].get<float>();
        setTone(p);
    } else if (act == "shape") {
        ToneParams p = tone_;
        p.mode = 1;
        p.shape = indexOf(kShapes, a.value("shape", "circle"), 0);
        if (a.contains("a")) p.a = a["a"].get<int>();
        if (a.contains("b")) p.b = a["b"].get<int>();
        if (a.contains("freq")) p.freq = a["freq"].get<float>();
        setTone(p);
    } else if (act == "preset") {
        loadPreset(format("slot%d", a.value("slot", 1)));
    } else if (act == "volume") {
        setVolumeDb(a.value("db", -12.0));
    } else if (act == "volume_step") {
        stepVolume(a.value("delta", 3.0));
    } else if (act == "mute") {
        setMute(!muted());
    } else if (act == "kill") {
        held_[key] = {{"was_muted", muted()}};
        setMute(true);
    } else if (act == "bypass_all") {
        setBypassAll(!bypassAll_);
    } else if (act == "all_off") {
        allOff();
    } else if (act == "tap") {
        tap();
    } else if (act == "delay_div") {
        held_[key] = json::object();
        delayDiv(a.value("div", 0.25));
    } else if (act == "delay_step") {
        int d = effectIndex("delay"), p = effectDescs()[d].paramIndex("time");
        setParam(d, p, fx_[d].values[p] * a.value("factor", 1.25f));
    } else if (act == "source") {
        const std::string kind = a.value("kind", "");
        if (kind == "tone") setTone(tone_);
        else if (kind == "file") stepFile(0);
        else if (kind == "capture") {
            std::string err;
            if (!useCapture(settings_.value("input", std::string()), &err)) notify(err);
        }
    } else if (act == "file_next") {
        stepFile(+1);
    } else if (act == "file_prev") {
        stepFile(-1);
    } else if (act == "cycle") {
        cycle(fx, a.value("param", ""));
    } else if (act == "panic") {
        panic();
    } else if (act == "view") {
        view = a.value("view", view);
    } else if (act == "timebase") {
        timebaseFactor *= a.value("factor", 2.0);
    }
}

void Rig::releasePad(int x, int y, const json& a) {
    auto it = held_.find({x, y});
    if (it == held_.end()) return;
    json info = it->second;
    held_.erase(it);
    const std::string act = a.value("action", "");
    if (act == "hold") {
        setEnabled(effectIndex(info.value("fx", "")), info.value("was_enabled", false));
    } else if (act == "hold_param") {
        const std::string fx = info.value("fx", "");
        if (info.contains("params"))
            for (auto p = info["params"].begin(); p != info["params"].end(); ++p) setParam(fx, p.key(), p.value());
        setEnabled(effectIndex(fx), info.value("was_enabled", false));
    } else if (act == "kill") {
        setMute(info.value("was_muted", false));
    }
}

}  // namespace pifx
