// What feeds the engine: tones / sweeps / noise, X-Y scope shapes, audio files and
// live capture (a USB interface, a mic, or the monitor of another output: that is how
// pifx "hijacks" audio already playing on the Pi).
//
// A Source is pulled from the audio thread only. The UI talks to it through engine
// commands (see Engine.h) and reads back progress through atomics.
#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/RingBuffer.h"

struct ma_context;
struct ma_device;

namespace pifx {

enum class SourceKind { Silence, Tone, File, Capture };
const char* sourceKindName(SourceKind k);

extern const std::vector<std::string> kWaves;   // sine square triangle saw white pink sweep
extern const std::vector<std::string> kShapes;  // circle lissajous rose figure8 line square star spiral

struct ToneParams {
    int mode = 0;            // 0 = tone (L = R), 1 = X-Y shape (L = x, R = y)
    int wave = 0;            // index into kWaves
    float freq = 440;        // Hz; for shapes: traces per second
    float level = -12;       // dBFS peak
    int shape = 0;           // index into kShapes
    int a = 3, b = 2;        // lissajous ratio / rose petals / star points
    float phaseDeg = 90;     // lissajous phase offset
    float sweepSec = 10, sweepLo = 20, sweepHi = 20000;
};

class Source {
public:
    virtual ~Source() = default;
    virtual SourceKind kind() const = 0;
    // Fill n interleaved stereo frames. Audio thread only.
    virtual void pull(float* out, int n) = 0;
};

class SilenceSource : public Source {
public:
    SourceKind kind() const override { return SourceKind::Silence; }
    void pull(float* out, int n) override;
};

class ToneSource : public Source {
public:
    explicit ToneSource(int sr) : sr_(sr) {}
    SourceKind kind() const override { return SourceKind::Tone; }
    void pull(float* out, int n) override;
    void setParams(const ToneParams& p);      // audio thread (via command)
    const ToneParams& params() const { return p_; }

private:
    void shape(float* out, int n, float amp);
    float noise();
    int sr_;
    ToneParams p_;
    double phase_ = 0, sweepT_ = 0;
    uint64_t rng_ = 0x9E3779B97F4A7C15ull;
    double pz_[3] = {0, 0, 0};                 // pink filter state
};

// Decoded audio, at the engine rate, interleaved stereo. Shared with the UI (overview).
struct AudioClip {
    std::string path, name;
    int sampleRate = 48000;
    std::vector<float> data;
    size_t frames() const { return data.size() / 2; }
    double seconds() const { return (double)frames() / sampleRate; }
};
// WAV / FLAC / MP3, any rate or channel count. Returns nullptr and sets err on failure.
std::shared_ptr<AudioClip> loadClip(const std::string& path, int sampleRate, std::string* err);
bool isAudioFile(const std::string& name);

class FileSource : public Source {
public:
    explicit FileSource(std::shared_ptr<const AudioClip> clip) : clip_(std::move(clip)) {}
    SourceKind kind() const override { return SourceKind::File; }
    void pull(float* out, int n) override;

    const AudioClip& clip() const { return *clip_; }
    std::shared_ptr<const AudioClip> clipPtr() const { return clip_; }
    // Shared state: written by the UI, read by the audio thread (and vice versa for pos).
    std::atomic<bool> playing{true};
    std::atomic<bool> loop{true};
    std::atomic<float> gainDb{0.0f};
    std::atomic<int64_t> seekTo{-1};          // UI requests a jump (frame index)
    std::atomic<int64_t> pos{0};              // current frame, for the UI

private:
    std::shared_ptr<const AudioClip> clip_;
};

// Live input through a lock-free ring. Input and output run on different clocks, so
// the fill level drifts: starve -> silence, too full -> skip ahead. Fine for scoping
// and jamming; for sample-exact work use the same card for in and out.
class CaptureSource : public Source {
public:
    CaptureSource(int sr, int block);
    ~CaptureSource() override;
    SourceKind kind() const override { return SourceKind::Capture; }
    void pull(float* out, int n) override;

    // Opens and starts the capture device. UI thread.
    bool open(ma_context* ctx, const void* deviceId /* ma_device_id* or null */, const std::string& name,
              std::string* err);
    void close();
    const std::string& deviceName() const { return name_; }

    std::atomic<float> gainDb{0.0f};
    std::atomic<uint32_t> overruns{0}, underruns{0};
    std::atomic<float> peak{0.0f};            // input level, for the UI meter

    void feed(const float* in, int frames, int channels);   // capture callback

private:
    int sr_, block_;
    FrameRing ring_;
    std::vector<float> conv_;
    ma_device* dev_ = nullptr;
    std::string name_;
};

}  // namespace pifx
