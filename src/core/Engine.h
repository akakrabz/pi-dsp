// The audio graph:   source -> effects chain -> master -> limiter -> outputs
//                       \-> tap (pre-FX)        \-> tap (post-FX)       (for the scope)
//
// render() runs on whichever thread clocks the engine (the first output device's
// callback, or a timer thread when nothing is playing). The UI never touches the
// chain or the active source directly: it posts Commands, and retired sources come
// back through a garbage queue so the UI thread frees them (never the audio thread).
#pragma once
#include <atomic>
#include <cstdint>
#include <vector>

#include "core/Dsp.h"
#include "core/RingBuffer.h"
#include "core/Sources.h"

namespace pifx {

enum class Cmd : uint8_t { SetSource, ToneParams, FxParam, FxEnable, BypassAll, MasterDb, Panic };

struct Command {
    Cmd type = Cmd::Panic;
    int fx = 0, param = 0;
    float value = 0;
    bool flag = false;
    Source* src = nullptr;
    ToneParams tone;
};

// Tap channels, interleaved: pre-FX L/R, post-FX L/R.
enum TapChannel { kInL = 0, kInR = 1, kOutL = 2, kOutR = 3, kTapChannels = 4 };
const char* tapChannelName(int ch);

class Engine {
public:
    explicit Engine(int sampleRate = 48000, int maxBlock = 2048);
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    int sampleRate() const { return sr_; }

    // ---- UI thread
    bool post(const Command& c);
    void setSource(Source* s);                 // takes ownership
    Source* source() const { return uiSource_; }
    void collect();                            // free retired sources
    // Swap in silence and block until `old` is retired and freed (closes capture devices
    // before their audio context goes away). Returns false on timeout.
    bool retire(Source* old, int timeoutMs = 500);
    void flushOffline();                       // apply queued commands now (only while no clock runs)
    FrameRing& tap() { return tap_; }          // consumer: the UI

    // ---- clock thread
    void render(float* out, int frames);

    // ---- stats (any thread)
    std::atomic<float> load{0}, limiterDb{0};
    std::atomic<float> outPeak[2], outRms[2], inPeak[2];
    std::atomic<uint64_t> frames{0};
    std::atomic<uint32_t> tapDrops{0}, errors{0};
    std::atomic<bool> clocked{false};          // set by AudioIO while a clock calls render()

    // Direct access for tests and offline rendering (no clock running).
    Chain& chainUnsafe() { return chain_; }

private:
    void applyCommands();
    void renderChunk(float* out, int n);

    int sr_, maxBlock_;
    Chain chain_;
    Source* active_ = nullptr;     // audio side
    Source* uiSource_ = nullptr;   // UI side: last source handed over
    SpscQueue<Command, 1024> cmds_;
    SpscQueue<Source*, 64> garbage_;
    FrameRing tap_;
    std::vector<float> in_, tapBuf_;
    float peakHold_[2] = {0, 0};
};

}  // namespace pifx
