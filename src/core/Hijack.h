// Route the Pi's system audio through pifx (PipeWire / PulseAudio).
//
//   tap     : record "Monitor of <output>": non-invasive, the audio still plays where
//             it was going; pifx only listens (no pactl needed).
//   hijack  : make a virtual output the system default and move every playing stream
//             onto it. pifx records its monitor, runs it through the effects and plays
//             it on the outputs you pick (DAC, HDMI, USB...). stop() puts the previous
//             default back and moves the streams home.
//
// The virtual output is either
//   - "pi_dsp" (shown as "pi-dsp"): a permanent dummy output created by PipeWire from
//     packaging/pipewire/20-pi-dsp-sink.conf (install.sh --system-audio). Always there,
//     visible in the desktop's sound menu, survives pifx restarts. Preferred.
//   - "pifx_hijack": created on the fly with module-null-sink when "pi_dsp" is missing,
//     and removed again by stop().
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
    static constexpr const char* kPersistentSink = "pi_dsp";
    static constexpr const char* kSink = "pifx_hijack";
    static bool isVirtualSink(const std::string& keyOrName);   // never use these as an output

    explicit Hijack(std::string stateFile = "") : stateFile_(std::move(stateFile)) {}
    ~Hijack() { stop(); }

    static bool available(std::string* why = nullptr);
    static bool persistentSinkExists();
    bool active() const { return active_; }
    // fallbackSink: where audio goes on stop() if the current default already is a
    // virtual sink (e.g. remembered from an earlier run). Usually pifx's own output.
    bool start(std::string* err, const std::string& fallbackSink = "");
    void stop();
    const std::string& sink() const { return sink_; }
    std::string monitor() const { return sink_.empty() ? std::string(kPersistentSink) + ".monitor" : sink_ + ".monitor"; }
    bool permanent() const { return active_ && module_ < 0; }
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
    static std::string firstRealSink();
    static std::vector<SinkInput> sinkInputs();
    void writeState() const;

    std::string stateFile_;
    bool active_ = false;
    int module_ = -1;          // >= 0 when we loaded module-null-sink ourselves
    std::string sink_, prevSink_;
    int moved_ = 0;
};

}  // namespace pifx
