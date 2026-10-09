#include "core/Launchpad.h"

#include <dlfcn.h>
#include <poll.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <sstream>

#include "core/Util.h"

namespace pifx {

static const Bytes kSysexHead = {0xF0, 0x00, 0x20, 0x29, 0x02};

// ---------------------------------------------------------------- identification
std::optional<LpModel> identifyLaunchpad(const std::string& portName) {
    std::string n = toLower(portName);
    if (n.find("launchpad") == std::string::npos && n.find("lp") == std::string::npos) return std::nullopt;
    static const struct { const char* sub; const char* fam; int dev; } models[] = {
        {"mini mk3", "mk3", 0x0D}, {"minimk3", "mk3", 0x0D}, {"launchpad x", "mk3", 0x0C}, {"lpx", "mk3", 0x0C},
        {"pro mk3", "mk3", 0x0E},  {"lpprmk3", "mk3", 0x0E}, {"lpprom", "mk3", 0x0E},      {"mk2", "mk2", 0x18},
        {"launchpad pro", "pro1", 0x10}, {"launchpad s", "legacy", -1}, {"launchpad mini", "legacy", -1},
        {"launchpad", "legacy", -1}};
    for (const auto& m : models)
        if (n.find(m.sub) != std::string::npos) return LpModel{m.fam, m.dev};
    return std::nullopt;
}

static std::vector<std::string> words(const std::string& s) {
    std::string t = toLower(s);
    std::replace(t.begin(), t.end(), ':', ' ');
    std::istringstream in(t);
    std::vector<std::string> w;
    std::string x;
    while (in >> x) w.push_back(x);
    return w;
}
static bool isDaw(const std::string& n) {
    for (const auto& w : words(n))
        if (w == "daw" || w == "da" || w == "live" || w == "d") return true;
    return toLower(n).find("live port") != std::string::npos;
}
static bool isMidi(const std::string& n) {
    for (const auto& w : words(n))
        if (w == "midi" || w == "mi" || w == "m") return true;
    return false;
}

std::string pickLaunchpadPort(const std::vector<std::string>& names) {
    std::vector<std::string> c;
    for (const auto& n : names)
        if (identifyLaunchpad(n)) c.push_back(n);
    if (c.empty()) return "";
    for (const auto& n : c)
        if (isMidi(n) && !isDaw(n)) return n;
    for (const auto& n : c)
        if (!isDaw(n)) return n;
    return c.back();
}

int legacyColor(int pal) {
    if (pal == 0) return 12;
    int r, g;
    if (pal >= 1 && pal <= 3) {
        r = g = pal == 1 ? 1 : 3;
    } else {
        int hue = (pal - 4) / 4;
        int bright = ((pal - 4) % 4 == 0 || (pal - 4) % 4 == 1) ? 3 : 1;
        if (hue == 0 || hue == 14 || hue == 13) { r = bright; g = 0; }
        else if (hue == 1 || hue == 2) { r = bright; g = hue == 2 ? bright : std::max(1, bright - 1); }
        else if (hue >= 3 && hue <= 5) { r = 0; g = bright; }
        else { r = g = std::max(1, bright - 1); }
    }
    return 16 * g + r + 12;
}

// ---------------------------------------------------------------- layout
std::vector<Bytes> LpLayout::enterMessages() const {
    auto sx = [&](std::initializer_list<uint8_t> tail) {
        Bytes b = kSysexHead;
        b.insert(b.end(), tail);
        return b;
    };
    if (family_ == "mk3") return {sx({(uint8_t)dev_, 0x0E, 0x01, 0xF7})};
    if (family_ == "mk2") return {sx({0x18, 0x22, 0x00, 0xF7})};
    if (family_ == "pro1") return {sx({0x10, 0x2C, 0x03, 0xF7})};
    return {{0xB0, 0x00, 0x00}, {0xB0, 0x00, 0x01}};
}

std::vector<Bytes> LpLayout::exitMessages() const {
    auto sx = [&](std::initializer_list<uint8_t> tail) {
        Bytes b = kSysexHead;
        b.insert(b.end(), tail);
        return b;
    };
    if (family_ == "mk3") return {sx({(uint8_t)dev_, 0x0E, 0x00, 0xF7})};
    if (family_ == "mk2") return {sx({0x18, 0x0E, 0x00, 0xF7})};
    if (family_ == "pro1") return {sx({0x10, 0x2C, 0x00, 0xF7})};
    return {{0xB0, 0x00, 0x00}};
}

std::optional<PadEvent> LpLayout::decode(const Bytes& m) const {
    if (m.size() < 3) return std::nullopt;
    const int status = m[0] & 0xF0, d1 = m[1], d2 = m[2];
    if (family_ == "legacy") {
        if (status == 0x90 || status == 0x80) {
            int row = d1 / 16, col = d1 % 16;
            if (row > 7 || col > 8) return std::nullopt;
            return PadEvent{col, 7 - row, status == 0x90 && d2 > 0};
        }
        if (status == 0xB0 && d1 >= 104 && d1 <= 111) return PadEvent{d1 - 104, 8, d2 > 0};
        return std::nullopt;
    }
    if (status == 0x90 || status == 0x80) {
        int tens = d1 / 10, units = d1 % 10;
        if (tens >= 1 && tens <= 8 && units >= 1 && units <= 9) return PadEvent{units - 1, tens - 1, status == 0x90 && d2 > 0};
        return std::nullopt;
    }
    if (status == 0xB0) {
        if (d1 >= 91 && d1 <= 98) return PadEvent{d1 - 91, 8, d2 > 0};
        if (d1 >= 104 && d1 <= 111) return PadEvent{d1 - 104, 8, d2 > 0};
        int tens = d1 / 10, units = d1 % 10;
        if (units == 9 && tens >= 1 && tens <= 8) return PadEvent{8, tens - 1, d2 > 0};
    }
    return std::nullopt;
}

Bytes LpLayout::led(int x, int y, int color) const {
    const uint8_t c = (uint8_t)std::clamp(color, 0, 127);
    if (family_ == "legacy") {
        uint8_t vel = (uint8_t)legacyColor(color);
        if (y == 8) return x <= 7 ? Bytes{0xB0, (uint8_t)(104 + x), vel} : Bytes{};
        if (x > 8 || y > 7) return {};
        return {0x90, (uint8_t)(16 * (7 - y) + x), vel};
    }
    if (y == 8) {
        if (x > 7) return {};
        return {0xB0, (uint8_t)(family_ == "mk2" ? 104 + x : 91 + x), c};
    }
    if (x == 8) {
        uint8_t note = (uint8_t)(10 * (y + 1) + 9);
        return family_ == "mk2" ? Bytes{0x90, note, c} : Bytes{0xB0, note, c};
    }
    return {0x90, (uint8_t)(10 * (y + 1) + (x + 1)), c};
}

// ---------------------------------------------------------------- parser
void MidiParser::feed(const uint8_t* data, size_t n, const std::function<void(const Bytes&)>& cb) {
    for (size_t i = 0; i < n; i++) {
        uint8_t b = data[i];
        if (b >= 0xF8) continue;                     // realtime: ignore
        if (sysex_) {
            msg_.push_back(b);
            if (b == 0xF7) { cb(msg_); msg_.clear(); sysex_ = false; }
            continue;
        }
        if (b == 0xF0) { msg_ = {b}; sysex_ = true; continue; }
        if (b & 0x80) {
            running_ = b >= 0xF0 ? 0 : b;
            msg_ = {b};
        } else {
            if (msg_.empty()) {
                if (!running_) continue;
                msg_ = {running_};
            }
            msg_.push_back(b);
        }
        const int st = msg_[0] & 0xF0;
        const size_t need = (st == 0xC0 || st == 0xD0) ? 2 : (msg_[0] >= 0xF0 ? 1 : 3);
        if (msg_.size() >= need && msg_[0] < 0xF0) {
            cb(msg_);
            msg_.clear();
        } else if (msg_[0] >= 0xF0 && msg_[0] != 0xF0) {
            msg_.clear();                            // system common: drop
        }
    }
}

// ---------------------------------------------------------------- device
LaunchpadDevice::LaunchpadDevice(const std::string& name, std::function<void(const Bytes&)> send,
                                 std::function<void(const PadEvent&)> onPad)
    : name_(name),
      layout_([&] {
          auto m = identifyLaunchpad(name);
          return m ? LpLayout(m->family, m->deviceId) : LpLayout("mk3", 0x0D);
      }()),
      send_(std::move(send)),
      onPad_(std::move(onPad)) {
    leds_.fill(-1);
    for (const auto& m : layout_.enterMessages()) send_(m);
    clear();
}

std::string LaunchpadDevice::model() const {
    const auto& f = layout_.family();
    if (f == "mk3") return "Launchpad MK3 family";
    if (f == "mk2") return "Launchpad MK2";
    if (f == "pro1") return "Launchpad Pro (2015)";
    return "Launchpad S / Mini";
}

void LaunchpadDevice::onMidi(const Bytes& msg) {
    if (auto e = layout_.decode(msg)) onPad_(*e);
}

void LaunchpadDevice::setLeds(const std::array<int, 81>& colors) {
    for (int y = 0; y < 9; y++)
        for (int x = 0; x < 9; x++) {
            if (x == 8 && y == 8) continue;
            int i = y * 9 + x;
            if (leds_[i] == colors[i]) continue;
            Bytes m = layout_.led(x, y, colors[i]);
            if (!m.empty()) {
                send_(m);
                leds_[i] = colors[i];
            }
        }
}

void LaunchpadDevice::clear() {
    for (int y = 0; y < 9; y++)
        for (int x = 0; x < 9; x++) {
            if (x == 8 && y == 8) continue;
            Bytes m = layout_.led(x, y, 0);
            if (!m.empty()) send_(m);
            leds_[y * 9 + x] = 0;
        }
}

void LaunchpadDevice::close() {
    clear();
    for (const auto& m : layout_.exitMessages()) send_(m);
}

// ---------------------------------------------------------------- ALSA rawmidi (dlopen)
namespace {
struct Alsa {
    void* lib = nullptr;
    int (*card_next)(int*) = nullptr;
    int (*card_get_name)(int, char**) = nullptr;
    int (*ctl_open)(void**, const char*, int) = nullptr;
    int (*ctl_close)(void*) = nullptr;
    int (*ctl_rawmidi_next_device)(void*, int*) = nullptr;
    int (*ctl_rawmidi_info)(void*, void*) = nullptr;
    int (*info_malloc)(void**) = nullptr;
    void (*info_free)(void*) = nullptr;
    void (*info_set_device)(void*, unsigned) = nullptr;
    void (*info_set_subdevice)(void*, unsigned) = nullptr;
    void (*info_set_stream)(void*, int) = nullptr;
    unsigned (*info_get_subdevices_count)(const void*) = nullptr;
    const char* (*info_get_subdevice_name)(const void*) = nullptr;
    const char* (*info_get_name)(const void*) = nullptr;
    int (*rm_open)(void**, void**, const char*, int) = nullptr;
    int (*rm_close)(void*) = nullptr;
    long (*rm_read)(void*, void*, size_t) = nullptr;
    long (*rm_write)(void*, const void*, size_t) = nullptr;
    int (*rm_poll_count)(void*) = nullptr;
    int (*rm_poll_desc)(void*, struct pollfd*, unsigned) = nullptr;
    std::string error;

    bool load() {
        if (lib) return true;
        if (!error.empty()) return false;
        lib = dlopen("libasound.so.2", RTLD_NOW | RTLD_LOCAL);
        if (!lib) { error = "libasound.so.2 not found (ALSA)"; return false; }
        bool ok = true;
        auto sym = [&](auto& fn, const char* name) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(lib, name));
            ok &= fn != nullptr;
        };
        sym(card_next, "snd_card_next");
        sym(card_get_name, "snd_card_get_name");
        sym(ctl_open, "snd_ctl_open");
        sym(ctl_close, "snd_ctl_close");
        sym(ctl_rawmidi_next_device, "snd_ctl_rawmidi_next_device");
        sym(ctl_rawmidi_info, "snd_ctl_rawmidi_info");
        sym(info_malloc, "snd_rawmidi_info_malloc");
        sym(info_free, "snd_rawmidi_info_free");
        sym(info_set_device, "snd_rawmidi_info_set_device");
        sym(info_set_subdevice, "snd_rawmidi_info_set_subdevice");
        sym(info_set_stream, "snd_rawmidi_info_set_stream");
        sym(info_get_subdevices_count, "snd_rawmidi_info_get_subdevices_count");
        sym(info_get_subdevice_name, "snd_rawmidi_info_get_subdevice_name");
        sym(info_get_name, "snd_rawmidi_info_get_name");
        sym(rm_open, "snd_rawmidi_open");
        sym(rm_close, "snd_rawmidi_close");
        sym(rm_read, "snd_rawmidi_read");
        sym(rm_write, "snd_rawmidi_write");
        sym(rm_poll_count, "snd_rawmidi_poll_descriptors_count");
        sym(rm_poll_desc, "snd_rawmidi_poll_descriptors");
        if (!ok) {
            error = "libasound.so.2 is missing rawmidi symbols";
            dlclose(lib);
            lib = nullptr;
        }
        return ok;
    }
};
Alsa& alsa() {
    static Alsa a;
    return a;
}
constexpr int kStreamInput = 1;      // SND_RAWMIDI_STREAM_INPUT
constexpr int kNonblock = 0x0002;    // SND_RAWMIDI_NONBLOCK
}  // namespace

bool RawMidi::available(std::string* why) {
    bool ok = alsa().load();
    if (!ok && why) *why = alsa().error;
    return ok;
}

std::vector<MidiPort> RawMidi::list() {
    std::vector<MidiPort> out;
    Alsa& a = alsa();
    if (!a.load()) return out;
    void* info = nullptr;
    if (a.info_malloc(&info) < 0) return out;
    int card = -1;
    while (a.card_next(&card) >= 0 && card >= 0) {
        void* ctl = nullptr;
        if (a.ctl_open(&ctl, format("hw:%d", card).c_str(), 0) < 0) continue;
        char* cname = nullptr;
        a.card_get_name(card, &cname);
        std::string cardName = cname ? cname : format("card %d", card);
        std::free(cname);
        int dev = -1;
        while (a.ctl_rawmidi_next_device(ctl, &dev) >= 0 && dev >= 0) {
            a.info_set_device(info, (unsigned)dev);
            a.info_set_stream(info, kStreamInput);
            a.info_set_subdevice(info, 0);
            if (a.ctl_rawmidi_info(ctl, info) < 0) continue;
            unsigned subs = a.info_get_subdevices_count(info);
            for (unsigned s = 0; s < subs; s++) {
                a.info_set_subdevice(info, s);
                if (a.ctl_rawmidi_info(ctl, info) < 0) continue;
                const char* sn = a.info_get_subdevice_name(info);
                std::string sub = sn && *sn ? sn : (a.info_get_name(info) ? a.info_get_name(info) : "MIDI");
                MidiPort p;
                p.name = cardName + ":" + sub;
                p.hw = format("hw:%d,%d,%u", card, dev, s);
                p.card = card;
                p.device = dev;
                p.sub = (int)s;
                p.subCount = (int)subs;
                out.push_back(p);
            }
        }
        a.ctl_close(ctl);
    }
    a.info_free(info);
    return out;
}

bool RawMidi::open(const std::string& hw, std::string* err) {
    close();
    Alsa& a = alsa();
    if (!a.load()) {
        if (err) *err = a.error;
        return false;
    }
    int r = a.rm_open(&in_, &out_, hw.c_str(), kNonblock);
    if (r < 0) {
        in_ = out_ = nullptr;
        if (err) *err = format("cannot open %s (error %d)", hw.c_str(), r);
        return false;
    }
    return true;
}

void RawMidi::close() {
    Alsa& a = alsa();
    if (in_) a.rm_close(in_);
    if (out_) a.rm_close(out_);
    in_ = out_ = nullptr;
}

int RawMidi::read(uint8_t* buf, size_t n, int timeoutMs) {
    if (!in_) return -1;
    Alsa& a = alsa();
    struct pollfd fds[4];
    int cnt = std::min(4, a.rm_poll_count(in_));
    if (cnt <= 0) return -1;
    a.rm_poll_desc(in_, fds, (unsigned)cnt);
    int pr = ::poll(fds, (nfds_t)cnt, timeoutMs);
    if (pr < 0) return -1;
    if (pr == 0) return 0;
    for (int i = 0; i < cnt; i++)
        if (fds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) return -1;
    long got = a.rm_read(in_, buf, n);
    if (got == -11 /* EAGAIN */) return 0;
    return (int)got;
}

bool RawMidi::write(const Bytes& b) {
    if (!out_ || b.empty()) return false;
    long w = alsa().rm_write(out_, b.data(), b.size());
    return w == (long)b.size();
}

// ---------------------------------------------------------------- manager
void LaunchpadManager::start() {
    std::string why;
    {
        std::lock_guard<std::mutex> lk(mu_);
        status_.available = RawMidi::available(&why);
        status_.error = why;
    }
    if (!status_.available) return;
    stop_.store(false);
    th_ = std::thread([this] { run(); });
}

void LaunchpadManager::stop() {
    stop_.store(true);
    if (th_.joinable()) th_.join();
}

bool LaunchpadManager::popEvent(PadEvent& e) {
    std::lock_guard<std::mutex> lk(mu_);
    if (events_.empty()) return false;
    e = events_.front();
    events_.erase(events_.begin());
    return true;
}

void LaunchpadManager::setColors(const std::array<int, 81>& c) {
    std::lock_guard<std::mutex> lk(mu_);
    if (c != colors_) {
        colors_ = c;
        dirty_ = true;
    }
}

LaunchpadManager::Status LaunchpadManager::status() const {
    std::lock_guard<std::mutex> lk(mu_);
    return status_;
}

static std::optional<MidiPort> choosePort(const std::vector<MidiPort>& ports, const std::string& override_) {
    if (!override_.empty()) {
        for (const auto& p : ports)
            if (p.hw == override_ || contains(p.name, override_)) return p;
        return std::nullopt;
    }
    std::vector<std::string> names;
    for (const auto& p : ports) names.push_back(p.name);
    std::string pick = pickLaunchpadPort(names);
    if (pick.empty()) return std::nullopt;
    const MidiPort* chosen = nullptr;
    for (const auto& p : ports)
        if (p.name == pick) { chosen = &p; break; }
    // Port names that do not say DAW/MIDI ("... MIDI 1", "... MIDI 2"): on MK3-family
    // devices the second port is the one that carries programmer-mode pads.
    auto m = identifyLaunchpad(pick);
    if (chosen && m && m->family == "mk3" && chosen->subCount > 1 && !isDaw(pick) && chosen->sub == 0) {
        bool namesSaySo = false;
        for (const auto& p : ports)
            if (p.card == chosen->card && p.device == chosen->device && isDaw(p.name)) namesSaySo = true;
        if (!namesSaySo)
            for (const auto& p : ports)
                if (p.card == chosen->card && p.device == chosen->device && p.sub == 1) return p;
    }
    return chosen ? std::optional<MidiPort>(*chosen) : std::nullopt;
}

void LaunchpadManager::run() {
    RawMidi midi;
    MidiParser parser;
    std::unique_ptr<LaunchpadDevice> dev;
    std::string hw;
    auto lastScan = std::chrono::steady_clock::now() - std::chrono::seconds(10);
    uint8_t buf[256];
    while (!stop_.load()) {
        if (!dev) {
            auto now = std::chrono::steady_clock::now();
            if (now - lastScan < std::chrono::seconds(2)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            lastScan = now;
            auto port = choosePort(RawMidi::list(), override_);
            if (!port) continue;
            std::string err;
            if (!midi.open(port->hw, &err)) {
                std::lock_guard<std::mutex> lk(mu_);
                status_.error = err;
                continue;
            }
            hw = port->hw;
            dev = std::make_unique<LaunchpadDevice>(
                port->name, [&midi](const Bytes& b) { midi.write(b); },
                [this](const PadEvent& e) {
                    std::lock_guard<std::mutex> lk(mu_);
                    if (events_.size() < 256) events_.push_back(e);
                });
            std::lock_guard<std::mutex> lk(mu_);
            status_.connected = true;
            status_.name = port->name + " (" + port->hw + ")";
            status_.model = dev->model();
            status_.error.clear();
            dirty_ = true;
        }
        int n = midi.read(buf, sizeof buf, 15);
        if (n < 0) {
            dev.reset();
            midi.close();
            std::lock_guard<std::mutex> lk(mu_);
            status_.connected = false;
            status_.error = "Launchpad disconnected";
            continue;
        }
        if (n > 0) parser.feed(buf, (size_t)n, [&](const Bytes& m) { dev->onMidi(m); });
        std::array<int, 81> want{};
        bool push = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (dirty_) {
                want = colors_;
                dirty_ = false;
                push = true;
            }
        }
        if (push) dev->setLeds(want);
    }
    if (dev) dev->close();
    midi.close();
}

}  // namespace pifx
