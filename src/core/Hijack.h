// "Hijack" the Pi's audio on a PipeWire/PulseAudio desktop.
//
//   tap     : record "Monitor of <output>" - non-invasive, the audio still plays
//             where it was going; pifx only listens (no pactl needed).
//   hijack  : create a virtual sink "pifx-hijack", make it the default and move every
//             playing stream onto it. pifx records its monitor, runs it through the
//             effects and plays it on whatever outputs you pick (DAC, HDMI, USB...).
//             stop() puts the previous default back and moves the streams home.
//
// Uses `pactl` (Debian: pulseaudio-utils), which works against pipewire-pulse too.
// A small state file lets `pifx unhijack` repair things after a crash.
#pragma once
#include <string>
#include <vector>

namespace pifx {

struct SinkInput {
    int index = -1;
    int sink = -1;
    std::string app;
};

class Hijack {
public:
    static constexpr const char* kSink = "pifx_hijack";
    static constexpr const char* kMonitor = "pifx_hijack.monitor";

    explicit Hijack(std::string stateFile = "") : stateFile_(std::move(stateFile)) {}
    ~Hijack() { stop(); }

    static bool available(std::string* why = nullptr);
    bool active() const { return module_ >= 0; }
    bool start(std::string* err);
    void stop();
    const std::string& previousSink() const { return prevSink_; }
    int movedStreams() const { return moved_; }

    // Restore a hijack left behind by a crashed pifx. Returns a message ("" if nothing to do).
    static std::string repair(const std::string& stateFile);

    // Exposed for tests.
    static std::vector<SinkInput> parseSinkInputsJson(const std::string& json);
    static std::vector<SinkInput> parseSinkInputsText(const std::string& text);

private:
    static std::string pactl(const std::string& args, int* status = nullptr);
    static std::string defaultSink();
    static int sinkIndex(const std::string& name);
    static std::vector<SinkInput> sinkInputs();

    std::string stateFile_;
    int module_ = -1;
    std::string prevSink_;
    int moved_ = 0;
};

}  // namespace pifx
