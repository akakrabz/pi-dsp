// Stereo effects chain: Filter, EQ, Drive, Tremolo, Delay, Bitcrush, Stutter, Reverb,
// then master gain and a peak limiter.
//
// Every effect works in place on interleaved stereo float frames, keeps its own
// state between calls and smooths parameter changes so knob moves never click.
// Nothing here touches hardware or allocates after prepare(), so the whole chain
// runs inside the audio callback and is unit-testable.
//
// Adding an effect:
//   1. add an EffectDesc (id, name, colour, params) in effectDescs() in Dsp.cpp
//   2. subclass Effect, implement process() (or processWet() for effects whose
//      tail should keep ringing after bypass), and construct it in makeEffect()
// The UI, presets and the Launchpad pick it up from the descriptor.
#pragma once
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pifx {

inline float dbToLin(float db) { return std::pow(10.0f, db / 20.0f); }
inline float linToDb(float lin) { return 20.0f * std::log10(std::max(lin, 1e-9f)); }

struct ParamSpec {
    std::string id, label;
    float min = 0, max = 1, def = 0;
    std::string unit;
    bool log = false;                    // UI slider mapping
    float step = 0;                      // 0 = continuous
    std::vector<std::string> choices;    // non-empty = enum; value is the index
    float smoothMs = 20;                 // 0 = jump immediately

    bool isEnum() const { return !choices.empty(); }
    float clamp(float v) const;
    int choiceIndex(const std::string& name) const;   // -1 if unknown
};

struct EffectDesc {
    std::string id, name, color;         // colour is "#rrggbb" (UI accent)
    bool tail = false;                   // keeps ringing after bypass
    std::vector<ParamSpec> params;
    int paramIndex(const std::string& pid) const;
};

// The chain order, also the order of effect pads on the Launchpad.
const std::vector<EffectDesc>& effectDescs();
int effectIndex(const std::string& id);

// Linear ramp toward a target over `ms` milliseconds, whatever the block size.
class Smoother {
public:
    void init(float v, int sr, float ms) { sr_ = sr; ms_ = ms; cur_ = target_ = v; rate_ = 0; }
    void set(float v);
    void snap(float v) { cur_ = target_ = v; rate_ = 0; }
    bool settled() const { return cur_ == target_; }
    float value() const { return cur_; }
    float target() const { return target_; }
    float next() {
        if (cur_ == target_) return cur_;
        cur_ += rate_;
        if ((rate_ > 0 && cur_ >= target_) || (rate_ < 0 && cur_ <= target_) || rate_ == 0) cur_ = target_;
        return cur_;
    }
    float advance(int n);   // n samples at once, returns the end value

private:
    int sr_ = 48000;
    float ms_ = 20, cur_ = 0, target_ = 0, rate_ = 0;
};

// One stereo biquad section (RBJ cookbook), transposed direct form II.
class Biquad {
public:
    enum Kind { Lowpass, Highpass, Bandpass, Notch, Peak, LowShelf, HighShelf };
    void design(Kind kind, double sr, double f0, double q = 0.7071, double gainDb = 0.0);
    void reset() { z1_[0] = z1_[1] = z2_[0] = z2_[1] = 0; }
    inline float tick(int ch, float x) {
        double y = b0_ * x + z1_[ch];
        z1_[ch] = b1_ * x - a1_ * y + z2_[ch];
        z2_[ch] = b2_ * x - a2_ * y;
        return (float)y;
    }
    // Magnitude response at f (for tests / UI curves).
    double magnitude(double f, double sr) const;

private:
    double b0_ = 1, b1_ = 0, b2_ = 0, a1_ = 0, a2_ = 0;
    double z1_[2] = {0, 0}, z2_[2] = {0, 0};
};

class Effect {
public:
    explicit Effect(const EffectDesc& d) : desc_(d) {}
    virtual ~Effect() = default;

    void prepare(int sr, int maxBlock);
    const EffectDesc& desc() const { return desc_; }
    const std::string& id() const { return desc_.id; }

    void setParam(int idx, float v);
    float param(int idx) const { return values_[idx]; }
    int choice(int idx) const { return (int)values_[idx]; }
    void resetParams();                         // all params to their defaults

    virtual void setEnabled(bool on);
    bool enabled() const { return enabled_; }
    bool tailQuiet() const { return tailQuiet_; }

    // In place on n interleaved stereo frames (n <= maxBlock).
    virtual void run(float* x, int n);

protected:
    virtual void setup() {}
    virtual void reset() {}
    virtual void onParam(int idx, float v) {}
    virtual void process(float* x, int n) {}                     // in place: replace with fully wet
    virtual void processWet(const float* in, float* wet, int n) {} // tail effects: wet only

    float next(int idx) { return sm_[idx].next(); }
    float advance(int idx, int n) { return sm_[idx].advance(n); }

    const EffectDesc& desc_;
    int sr_ = 48000, maxBlock_ = 1024;
    std::vector<float> values_;
    std::vector<Smoother> sm_;
    Smoother enable_;
    bool enabled_ = false, tailQuiet_ = true;
    std::vector<float> dry_, wet_;
};

std::unique_ptr<Effect> makeEffect(const std::string& id);

// Peak limiter + hard ceiling. Always last, protects the speakers.
class Limiter {
public:
    void prepare(int sr, float ceiling = 0.98f, float releaseMs = 250.0f);
    void process(float* x, int n);
    float reductionDb() const { return reductionDb_; }

private:
    float ceiling_ = 0.98f, gain_ = 1.0f, release_ = 0.9999f, reductionDb_ = 0;
};

// Ordered effects + software master gain + limiter.
class Chain {
public:
    Chain(int sr = 48000, int maxBlock = 1024);
    int size() const { return (int)fx_.size(); }
    Effect& fx(int i) { return *fx_[i]; }
    Effect* byId(const std::string& id);

    void setMasterDb(float db);
    float masterDb() const { return masterDb_; }
    void setBypassAll(bool on);
    bool bypassAll() const { return bypassAll_; }
    void panic();                                // everything off, params to defaults

    void process(float* x, int n);               // any n; chunks internally
    float limiterDb() const { return limiter_.reductionDb(); }

private:
    void processChunk(float* x, int n);
    int sr_, maxBlock_;
    std::vector<std::unique_ptr<Effect>> fx_;
    Smoother master_, bypass_;
    float masterDb_ = -6.0f;
    bool bypassAll_ = false;
    Limiter limiter_;
    std::vector<float> dry_;
};

}  // namespace pifx
