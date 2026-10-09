// Novation Launchpad support: detection, programmer mode, pad input, LED feedback.
//
// Supported:  Launchpad Mini MK3, X, Pro MK3 (programmer mode) - MK2 (session layout)
//             - Pro 2015 (programmer layout) - S / Mini MK1 / original (XY layout,
//             red/green LEDs only).
// I/O goes through ALSA rawmidi, loaded with dlopen("libasound.so.2") at runtime, so
// the binary builds without ALSA headers and runs fine on machines without ALSA.
// The layout logic works on plain byte vectors and is unit-tested with a fake port.
#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace pifx {

using Bytes = std::vector<uint8_t>;

struct PadEvent {
    int x = 0, y = 0;
    bool pressed = false;
};

struct LpModel {
    std::string family;   // mk3 | mk2 | pro1 | legacy
    int deviceId = -1;    // sysex device id, -1 = none
    bool operator==(const LpModel& o) const { return family == o.family && deviceId == o.deviceId; }
};
std::optional<LpModel> identifyLaunchpad(const std::string& portName);
// Chooses the MIDI (not DAW) port of the first Launchpad. "" if none.
std::string pickLaunchpadPort(const std::vector<std::string>& names);
int legacyColor(int paletteIndex);

class LpLayout {
public:
    LpLayout(std::string family, int deviceId) : family_(std::move(family)), dev_(deviceId) {}
    const std::string& family() const { return family_; }
    std::vector<Bytes> enterMessages() const;
    std::vector<Bytes> exitMessages() const;
    std::optional<PadEvent> decode(const Bytes& msg) const;
    Bytes led(int x, int y, int color) const;    // empty = no such LED

private:
    std::string family_;
    int dev_;
};

// Splits a raw MIDI byte stream into messages (running status, sysex, realtime).
class MidiParser {
public:
    void feed(const uint8_t* data, size_t n, const std::function<void(const Bytes&)>& cb);

private:
    Bytes msg_;
    uint8_t running_ = 0;
    bool sysex_ = false;
};

// One connected Launchpad. `send` writes raw bytes to the device.
class LaunchpadDevice {
public:
    LaunchpadDevice(const std::string& name, std::function<void(const Bytes&)> send,
                    std::function<void(const PadEvent&)> onPad);
    const std::string& name() const { return name_; }
    std::string model() const;
    void onMidi(const Bytes& msg);
    // 9x9 grid, index y*9+x; the (8,8) corner is unused.
    void setLeds(const std::array<int, 81>& colors);
    void clear();
    void close();

private:
    std::string name_;
    LpLayout layout_;
    std::function<void(const Bytes&)> send_;
    std::function<void(const PadEvent&)> onPad_;
    std::array<int, 81> leds_;
};

// ALSA rawmidi through dlopen.
struct MidiPort {
    std::string name;     // "card name:subdevice name"
    std::string hw;       // "hw:1,0,1"
    int card = 0, device = 0, sub = 0, subCount = 1;
};
class RawMidi {
public:
    static bool available(std::string* why = nullptr);
    static std::vector<MidiPort> list();               // input-capable ports
    ~RawMidi() { close(); }
    bool open(const std::string& hw, std::string* err);
    void close();
    bool isOpen() const { return in_ != nullptr; }
    // Waits up to timeoutMs. Returns bytes read, 0 on timeout, < 0 on error (unplugged).
    int read(uint8_t* buf, size_t n, int timeoutMs);
    bool write(const Bytes& b);

private:
    void* in_ = nullptr;
    void* out_ = nullptr;
};

// Background thread: finds a Launchpad, keeps it connected across hot-plugs, queues pad
// events for the UI thread and pushes LED colours back to the device.
class LaunchpadManager {
public:
    struct Status {
        bool available = false, connected = false;
        std::string name, model, error;
    };
    explicit LaunchpadManager(std::string portOverride = "") : override_(std::move(portOverride)) {}
    ~LaunchpadManager() { stop(); }
    void start();
    void stop();
    bool popEvent(PadEvent& e);
    void setColors(const std::array<int, 81>& c);
    Status status() const;

private:
    void run();
    std::string override_;
    std::thread th_;
    std::atomic<bool> stop_{false};
    mutable std::mutex mu_;
    std::vector<PadEvent> events_;
    std::array<int, 81> colors_{};
    bool dirty_ = true;
    Status status_;
};

}  // namespace pifx
