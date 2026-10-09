// Audio I/O, interface-agnostic: pick a backend (PipeWire/PulseAudio, ALSA, JACK),
// list every playback and capture device it offers, play the engine on any set of
// outputs at once, and capture from any input - including "Monitor of <output>",
// which is how pifx taps audio already playing on the Pi.
//
// Routing model: the first selected output is the clock and calls Engine::render();
// the others ("followers") are fed through small ring buffers that absorb clock
// drift. With no output selected a timer thread clocks the engine (scope only).
#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/Engine.h"
#include "core/RingBuffer.h"

struct ma_context;
struct ma_device;

namespace pifx {

enum class Backend { Auto, Pulse, Alsa, Jack };
const char* backendName(Backend b);
Backend backendFromName(const std::string& s);

struct DeviceInfo {
    std::string name;          // human readable
    std::string key;           // stable id: PulseAudio sink/source name, ALSA "hw:2,0", ...
    bool isDefault = false;
    bool monitor = false;      // capture device that records an output
    std::vector<unsigned char> id;   // raw ma_device_id
};

struct OutputView {
    std::string key, name;
    bool clock = false;        // this device drives the engine
    bool ok = false;           // running
    float gainDb = 0;
    bool mute = false;
    float peak = 0;
    uint32_t underruns = 0;
    std::string error;
};

class AudioIO {
public:
    AudioIO(Engine& engine, int blockFrames = 256);
    ~AudioIO();

    // (Re)open the backend. Stops all outputs; call setOutputs() again afterwards.
    bool init(Backend b, std::string* err);
    void shutdown();
    Backend requested() const { return requested_; }
    std::string activeBackend() const;              // e.g. "PulseAudio"
    bool isPulse() const;

    void refresh();                                  // re-enumerate devices
    const std::vector<DeviceInfo>& playback() const { return playback_; }
    const std::vector<DeviceInfo>& capture() const { return capture_; }
    const DeviceInfo* findPlayback(const std::string& keyOrName) const;
    const DeviceInfo* findCapture(const std::string& keyOrName) const;

    // First key = clock. Empty list = timer clock (no sound, scope keeps running).
    // Unknown keys are skipped and reported in *err.
    bool setOutputs(const std::vector<std::string>& keys, std::string* err);
    std::vector<std::string> outputKeys() const;
    std::vector<OutputView> outputs() const;
    void setOutputGain(size_t i, float db);
    void setOutputMute(size_t i, bool mute);
    std::string clockName() const;
    bool timerClock() const { return timer_.joinable(); }
    int blockFrames() const { return block_; }

    // UI thread, every frame: notices unplugged outputs and re-routes.
    // Returns a message when something changed.
    std::string poll();

    ma_context* context() { return ctx_; }
    // Opens a capture source on a device (nullptr key = system default input).
    std::unique_ptr<CaptureSource> openCapture(const std::string& key, std::string* err);

    // Used by the device callbacks.
    struct Out;
    void clockCallback(Out* o, float* out, int frames);

private:
    void stopAll();
    void startTimer();
    void stopTimer();

    Engine& engine_;
    int block_;
    Backend requested_ = Backend::Auto;
    ma_context* ctx_ = nullptr;
    std::vector<DeviceInfo> playback_, capture_;
    std::vector<std::unique_ptr<Out>> outs_;
    std::thread timer_;
    std::atomic<bool> timerStop_{false};
    mutable std::mutex mu_;   // guards outs_ against poll()/outputs() from the UI
};

}  // namespace pifx
