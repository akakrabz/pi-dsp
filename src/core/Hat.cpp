#include "core/Hat.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <regex>
#include <set>

#include "core/Util.h"

namespace pifx {

const char* const kRecommendedOverlay = "allo-boss-dac-pcm512x-audio";
const char* const kFallbackOverlay = "hifiberry-dacplus";

static const char* kOverlays[] = {
    "allo-boss-dac-pcm512x-audio", "hifiberry-dacplus", "hifiberry-dacplus-std", "hifiberry-dacplus-pro",
    "hifiberry-dac", "allo-piano-dac-pcm512x-audio", "allo-piano-dac-plus-pcm512x-audio", "iqaudio-dacplus",
    "iqaudio-dac", "justboom-dac", "rpi-dac"};
static const char* kCardHints[] = {"bossdac", "boss", "hifiberry", "pcm512", "pcm5122", "iqaudio",
                                   "justboom", "pianodac", "allo", "rpi-dac", "rpidac"};
static const char* kModules[] = {"snd_soc_pcm512x", "snd_soc_pcm512x_i2c", "snd_soc_allo_boss_dac",
                                 "snd_soc_hifiberry_dacplus", "snd_soc_rpi_simple_soundcard"};

static std::string readSys(const std::string& abs) {
    std::string s;
    if (!readFile(sysPath(abs), s)) return "";
    s.erase(std::remove(s.begin(), s.end(), '\0'), s.end());
    return trim(s);
}

std::string HatStatus::summary() const {
    if (detected && card) return format("%s (card %d, hw:%d)", card->name.c_str(), card->index, card->index);
    return "no PCM5122 DAC detected";
}

void parseConfigTxt(const std::string& text, std::vector<std::string>& overlays, std::optional<bool>& audio) {
    static const std::regex ov(R"(^dtoverlay\s*=\s*(.+)$)"), dp(R"(^dtparam\s*=\s*(.+)$)");
    for (const auto& raw : splitLines(text)) {
        std::string line = trim(raw.substr(0, raw.find('#')));
        if (line.empty()) continue;
        std::smatch m;
        if (std::regex_match(line, m, ov)) {
            overlays.push_back(trim(m[1]));
        } else if (std::regex_match(line, m, dp)) {
            for (const auto& part : split(m[1], ',')) {
                auto eq = part.find('=');
                std::string k = trim(part.substr(0, eq)), v = eq == std::string::npos ? "" : toLower(trim(part.substr(eq + 1)));
                if (k == "audio") audio = v.empty() || v == "on" || v == "1" || v == "true" || v == "yes";
            }
        }
    }
}

std::vector<SoundCard> parseAsoundCards(const std::string& text) {
    static const std::regex head(R"(^\s*(\d+)\s+\[(\S+)\s*\]:\s+(\S+)\s+-\s+(.*)$)");
    std::vector<SoundCard> cards;
    auto lines = splitLines(text);
    for (size_t i = 0; i < lines.size(); i++) {
        std::smatch m;
        if (!std::regex_match(lines[i], m, head)) continue;
        SoundCard c;
        c.index = std::atoi(m[1].str().c_str());
        c.id = m[2];
        c.driver = m[3];
        c.name = i + 1 < lines.size() ? trim(lines[i + 1]) : trim(m[4]);
        std::string blob = toLower(c.id + " " + c.driver + " " + c.name);
        for (const char* h : kCardHints) c.pcm512x |= blob.find(h) != std::string::npos;
        cards.push_back(c);
        i++;
    }
    return cards;
}

static bool processRunning(const std::string& name) {
    for (const auto& pid : listDir(sysPath("/proc"))) {
        if (pid.empty() || !std::isdigit((unsigned char)pid[0])) continue;
        if (readSys("/proc/" + pid + "/comm") == name) return true;
    }
    return false;
}

HatStatus detectHat() {
    HatStatus st;
    st.piModel = readSys("/proc/device-tree/model");
    st.isPi5 = st.piModel.find("Raspberry Pi 5") != std::string::npos;
    for (const char* k : {"vendor", "product", "product_id", "product_ver", "uuid"}) {
        std::string v = readSys(std::string("/proc/device-tree/hat/") + k);
        if (!v.empty()) st.eeprom.push_back({k, v});
    }
    for (const char* cand : {"/boot/firmware/config.txt", "/boot/config.txt"})
        if (fileExists(sysPath(cand))) { st.configPath = cand; break; }
    if (!st.configPath.empty()) {
        std::string text;
        readFile(sysPath(st.configPath), text);
        parseConfigTxt(text, st.overlays, st.onboardAudio);
    }
    for (const auto& ov : st.overlays) {
        std::string base = trim(ov.substr(0, ov.find(',')));
        if (std::find_if(std::begin(kOverlays), std::end(kOverlays), [&](const char* o) { return base == o; }) !=
            std::end(kOverlays)) {
            st.audioOverlay = ov;
            break;
        }
    }
    std::set<std::string> mods;
    for (const auto& line : splitLines(readSys("/proc/modules"))) {
        std::string n = trim(line.substr(0, line.find(' ')));
        for (const char* m : kModules)
            if (n == m) mods.insert(n);
    }
    st.modules.assign(mods.begin(), mods.end());
    for (const auto& dev : listDir(sysPath("/sys/bus/i2c/devices"))) {
        std::string name = readSys("/sys/bus/i2c/devices/" + dev + "/name");
        if (toLower(name).find("pcm512") != std::string::npos) {
            st.i2cCodec = dev + ": " + name;
            break;
        }
    }
    st.cards = parseAsoundCards(readSys("/proc/asound/cards"));
    for (auto& c : st.cards) {
        std::string hp = readSys(format("/proc/asound/card%d/pcm0p/sub0/hw_params", c.index));
        if (!hp.empty() && hp != "closed") {
            std::string flat;
            for (const auto& l : splitLines(hp)) flat += (flat.empty() ? "" : " ") + trim(l);
            c.hwParams = flat;
        }
    }
    st.pipewire = processRunning("pipewire");
    st.pulseaudio = processRunning("pulseaudio");
    for (const auto& c : st.cards)
        if (c.pcm512x) { st.card = c; st.detected = true; break; }

    const std::string cfg = st.configPath.empty() ? "/boot/firmware/config.txt" : st.configPath;
    if (!st.detected) {
        if (st.piModel.empty()) {
            st.issues.push_back("Not running on a Raspberry Pi (no /proc/device-tree/model). Any other sound "
                                "device works as an output.");
        } else if (st.audioOverlay.empty()) {
            st.issues.push_back("No PCM5122 sound card and no DAC overlay configured.");
            st.recommendations.push_back(format("Add this line to %s and reboot:\n    dtoverlay=%s\n  (If the card still "
                                                "does not appear, try dtoverlay=%s instead.)",
                                                cfg.c_str(), kRecommendedOverlay, kFallbackOverlay));
        } else {
            st.issues.push_back("Overlay '" + st.audioOverlay + "' is configured but no PCM5122 card appeared.");
            if (st.i2cCodec.empty())
                st.recommendations.push_back("The codec was not found on I2C. Check that the HAT is seated on all 40 "
                                             "pins, then run\n    sudo i2cdetect -y 1\n  and look for a device at 0x4d (or 0x4c).");
            st.recommendations.push_back(std::string("Try the other overlay: dtoverlay=") +
                                         (startsWith(st.audioOverlay, "allo") ? kFallbackOverlay : kRecommendedOverlay));
            if (st.isPi5)
                st.recommendations.push_back("On a Pi 5, if the card appears but stays silent or playback stalls, "
                                             "append ',slave' to the overlay line so the Pi generates the I2S clocks.");
        }
    } else if (st.pipewire || st.pulseaudio) {
        st.recommendations.push_back("PipeWire/PulseAudio is running: use the PipeWire/PulseAudio backend (default) "
                                     "and pick the DAC as an output, or stop the desktop audio server and use ALSA "
                                     "for the lowest latency.");
    }
    if (st.detected && st.audioOverlay.empty() && st.eeprom.empty())
        st.recommendations.push_back("The card is up but no DAC overlay is in config.txt, so it is probably loaded "
                                     "from the HAT EEPROM. That is fine.");
    return st;
}

std::string formatReport(const HatStatus& st) {
    std::string out;
    auto line = [&](const std::string& s) { out += s + "\n"; };
    line("Board        : " + (st.piModel.empty() ? std::string("not a Raspberry Pi") : st.piModel));
    if (!st.eeprom.empty()) {
        std::string e;
        for (const auto& kv : st.eeprom) e += (e.empty() ? "" : ", ") + kv.first + "=" + kv.second;
        line("HAT EEPROM   : " + e);
    } else {
        line("HAT EEPROM   : none read (overlay must come from config.txt)");
    }
    line("config.txt   : " + (st.configPath.empty() ? std::string("not found") : st.configPath));
    line("DAC overlay  : " + (st.audioOverlay.empty() ? std::string("none") : st.audioOverlay));
    if (!st.overlays.empty()) {
        std::string o;
        for (const auto& v : st.overlays) o += (o.empty() ? "" : ", ") + v;
        line("all overlays : " + o);
    }
    line(std::string("onboard audio: ") + (st.onboardAudio ? (*st.onboardAudio ? "on" : "off") : "unset"));
    line("I2C codec    : " + (st.i2cCodec.empty() ? std::string("not found") : st.i2cCodec));
    std::string mods;
    for (const auto& m : st.modules) mods += (mods.empty() ? "" : ", ") + m;
    line("modules      : " + (mods.empty() ? std::string("none") : mods));
    std::string srv = std::string(st.pipewire ? "pipewire " : "") + (st.pulseaudio ? "pulseaudio" : "");
    line("audio server : " + (trim(srv).empty() ? std::string("none (plain ALSA)") : trim(srv)));
    line("sound cards  :");
    for (const auto& c : st.cards)
        line(format("    hw:%d  %-16s %s%s%s", c.index, c.id.c_str(), c.name.c_str(),
                    c.hwParams.empty() ? "" : ("  [" + c.hwParams + "]").c_str(), c.pcm512x ? "  <-- PCM5122 DAC" : ""));
    if (st.cards.empty()) line("    (none)");
    line(std::string("RESULT       : ") + (st.detected ? "OK - " : "NOT FOUND - ") + st.summary());
    for (const auto& i : st.issues) line("  ! " + i);
    for (const auto& r : st.recommendations) line("  > " + r);
    return out;
}

// ---------------------------------------------------------------- amixer
std::optional<double> MixerControl::rawToDb(int raw) const {
    if (!dbMin || !dbStep) return std::nullopt;
    if (dbMuteAtMin && raw == vmin) return -std::numeric_limits<double>::infinity();
    return *dbMin + (raw - vmin) * *dbStep;
}

int MixerControl::dbToRaw(double db) const {
    if (!dbMin || !dbStep || *dbStep == 0) return vmin;
    int raw = (int)std::lround(vmin + (db - *dbMin) / *dbStep);
    return std::max(vmin, std::min(vmax, raw));
}

std::vector<int> MixerControl::ints() const {
    std::vector<int> v;
    for (const auto& s : values) {
        char* e = nullptr;
        long x = std::strtol(s.c_str(), &e, 10);
        if (e && *e == 0 && !s.empty()) v.push_back((int)x);
    }
    return v;
}

std::vector<MixerControl> parseAmixerContents(const std::string& text) {
    static const std::regex ctl(R"(^numid=(\d+),iface=\w+,name='([^']*)')");
    static const std::regex type(R"(type=(\w+),access=[\w-]+,values=(\d+)(?:,min=(-?\d+),max=(-?\d+))?)");
    static const std::regex vals(R"(^\s*:\s*values=(.*)$)");
    static const std::regex db(R"(dBscale-min=(-?[\d.]+)dB,step=([\d.]+)dB(?:,mute=(\d))?)");
    static const std::regex item(R"(^\s*;\s*Item #(\d+) '(.*)'$)");
    std::vector<MixerControl> out;
    for (const auto& line : splitLines(text)) {
        std::smatch m;
        std::string t = trim(line);
        if (std::regex_search(t, m, ctl) && m.position(0) == 0) {
            MixerControl c;
            c.numid = std::atoi(m[1].str().c_str());
            c.name = m[2];
            c.kind = "?";
            out.push_back(c);
            continue;
        }
        if (out.empty()) continue;
        MixerControl& c = out.back();
        if (c.kind == "?" && std::regex_search(line, m, type)) {
            c.kind = m[1];
            if (m[3].matched) { c.vmin = std::atoi(m[3].str().c_str()); c.vmax = std::atoi(m[4].str().c_str()); }
            continue;
        }
        if (std::regex_match(line, m, item)) { c.items.push_back(m[2]); continue; }
        if (std::regex_match(line, m, vals)) {
            c.values.clear();
            for (auto v : split(m[1], ',')) {
                v = trim(v);
                if (v.empty()) continue;
                if (c.kind == "ENUMERATED" && !v.empty() && std::all_of(v.begin(), v.end(), ::isdigit) &&
                    std::atoi(v.c_str()) < (int)c.items.size())
                    v = c.items[std::atoi(v.c_str())];
                c.values.push_back(v);
            }
            continue;
        }
        if (std::regex_search(line, m, db)) {
            c.dbMin = std::atof(m[1].str().c_str());
            c.dbStep = std::atof(m[2].str().c_str());
            c.dbMuteAtMin = m[3].matched && m[3] == "1";
        }
    }
    return out;
}

AmixerMixer::AmixerMixer(int card) : card_(card) {
    const char* env = std::getenv("PIFX_AMIXER");
    amixer_ = env && *env ? env : "amixer";
    refresh();
}

std::string AmixerMixer::run(const std::string& args, int* status) const {
    return runCapture(shellQuote(amixer_) + " -c " + std::to_string(card_) + " " + args, status);
}

void AmixerMixer::refresh() {
    if (amixer_ == "amixer" && !which("amixer")) {
        error_ = "amixer not installed (sudo apt install alsa-utils)";
        controls_.clear();
        return;
    }
    int st = 0;
    std::string out = run("contents", &st);
    if (st != 0) {
        error_ = trim(out).empty() ? "amixer failed" : trim(out);
        controls_.clear();
        return;
    }
    controls_ = parseAmixerContents(out);
    error_.clear();
}

bool AmixerMixer::ok() const { return error_.empty() && get(kVolume) != nullptr; }

const MixerControl* AmixerMixer::get(const std::string& name) const {
    for (const auto& c : controls_)
        if (c.name == name) return &c;
    return nullptr;
}

std::vector<std::string> AmixerMixer::controlNames() const {
    std::vector<std::string> v;
    for (const auto& c : controls_) v.push_back(c.name);
    std::sort(v.begin(), v.end());
    return v;
}

bool AmixerMixer::setRaw(const std::string& name, const std::string& value) {
    const MixerControl* c = get(name);
    if (!c) return false;
    std::string v = value;
    if (c->values.size() > 1 && v.find(',') == std::string::npos) {   // same value on every channel
        std::string all;
        for (size_t i = 0; i < c->values.size(); i++) all += (i ? "," : "") + v;
        v = all;
    }
    int st = 0;
    std::string out = run(format("cset numid=%d ", c->numid) + shellQuote(v), &st);
    if (st != 0) error_ = trim(out);
    refresh();
    return st == 0;
}

std::optional<double> AmixerMixer::volumeDb() const {
    const MixerControl* c = get(kVolume);
    if (!c) return std::nullopt;
    auto v = c->ints();
    if (v.empty()) return std::nullopt;
    return c->rawToDb(*std::max_element(v.begin(), v.end()));
}

std::optional<double> AmixerMixer::volumeDbMin() const {
    const MixerControl* c = get(kVolume);
    return c ? c->dbMin : std::nullopt;
}

bool AmixerMixer::setVolumeDb(double db) {
    const MixerControl* c = get(kVolume);
    if (!c) return false;
    auto top = c->rawToDb(c->vmax);
    db = std::min(db, std::min(kMaxDb, top ? *top : kMaxDb));
    return setRaw(kVolume, std::to_string(c->dbToRaw(db)));
}

std::optional<bool> AmixerMixer::muted() const {
    const MixerControl* c = get(kMute);
    if (!c || c->values.empty()) return std::nullopt;
    for (const auto& v : c->values)
        if (v != "on") return true;
    return false;
}

bool AmixerMixer::setMute(bool m) { return setRaw(kMute, m ? "off" : "on"); }

std::optional<double> AmixerMixer::analogDb() const {
    const MixerControl* c = get(kAnalog);
    if (!c) return std::nullopt;
    auto v = c->ints();
    if (v.empty()) return std::nullopt;
    return c->rawToDb(*std::min_element(v.begin(), v.end()));
}

bool AmixerMixer::setAnalogDb(double db) { return setRaw(kAnalog, db >= -3 ? "1" : "0"); }

std::vector<std::string> AmixerMixer::dspPrograms() const {
    const MixerControl* c = get(kDsp);
    return c ? c->items : std::vector<std::string>{};
}

std::string AmixerMixer::dspProgram() const {
    const MixerControl* c = get(kDsp);
    return c && !c->values.empty() ? c->values[0] : "";
}

bool AmixerMixer::setDspProgram(int index) { return setRaw(kDsp, std::to_string(index)); }

std::unique_ptr<Mixer> mixerFor(const HatStatus& st) {
    if (st.detected && st.card) return std::make_unique<AmixerMixer>(st.card->index);
    return std::make_unique<NullMixer>();
}

}  // namespace pifx
