#include "core/Dsp.h"

#include <algorithm>
#include <cstring>

namespace pifx {

static constexpr double kPi = 3.14159265358979323846;

// ---------------------------------------------------------------- params
float ParamSpec::clamp(float v) const {
    if (isEnum()) {
        if (std::isnan(v)) return 0;
        int i = (int)v;
        return (float)std::max(0, std::min((int)choices.size() - 1, i));
    }
    if (std::isnan(v)) v = def;
    v = std::max(min, std::min(max, v));
    if (step > 0) v = std::round(v / step) * step;
    return v;
}

int ParamSpec::choiceIndex(const std::string& name) const {
    for (size_t i = 0; i < choices.size(); i++)
        if (choices[i] == name) return (int)i;
    return -1;
}

int EffectDesc::paramIndex(const std::string& pid) const {
    for (size_t i = 0; i < params.size(); i++)
        if (params[i].id == pid) return (int)i;
    return -1;
}

static ParamSpec P(const char* id, const char* label, float mn, float mx, float def, const char* unit = "",
                   bool log = false, float smoothMs = 20, float step = 0) {
    ParamSpec p;
    p.id = id; p.label = label; p.min = mn; p.max = mx; p.def = def; p.unit = unit;
    p.log = log; p.smoothMs = smoothMs; p.step = step;
    return p;
}
static ParamSpec E(const char* id, const char* label, std::vector<std::string> choices, int def = 0) {
    ParamSpec p;
    p.id = id; p.label = label; p.min = 0; p.max = (float)choices.size() - 1; p.def = (float)def;
    p.choices = std::move(choices); p.smoothMs = 0;
    return p;
}

const std::vector<EffectDesc>& effectDescs() {
    static const std::vector<EffectDesc> d = {
        {"filter", "Filter", "#4fd1c5", false, {
            E("mode", "Mode", {"lowpass", "highpass", "bandpass"}),
            P("cutoff", "Cutoff", 20, 20000, 1200, "Hz", true, 30),
            P("q", "Resonance", 0.3f, 10, 0.9f, "Q", true, 30),
            P("drive", "Drive", 0, 18, 0, "dB")}},
        {"eq", "EQ", "#f6ad55", false, {
            P("low", "Low", -15, 15, 0, "dB"),
            P("mid", "Mid", -15, 15, 0, "dB"),
            P("mid_freq", "Mid freq", 200, 8000, 1000, "Hz", true, 40),
            P("high", "High", -15, 15, 0, "dB")}},
        {"drive", "Drive", "#fc8181", false, {
            E("mode", "Mode", {"soft", "hard", "asym", "fold"}),
            P("drive", "Drive", 0, 40, 12, "dB"),
            P("tone", "Tone", 500, 12000, 4000, "Hz", true, 30),
            P("level", "Level", -24, 6, -6, "dB")}},
        {"tremolo", "Tremolo", "#b794f4", false, {
            P("rate", "Rate", 0.1f, 25, 4.5f, "Hz", true, 60),
            P("depth", "Depth", 0, 1, 0.7f),
            E("shape", "Shape", {"sine", "triangle", "square"}),
            P("spread", "Stereo", 0, 180, 0, "deg", false, 60)}},
        {"delay", "Delay", "#63b3ed", true, {
            P("time", "Time", 10, 2000, 380, "ms", true, 120),
            P("feedback", "Feedback", 0, 0.98f, 0.45f),
            P("mix", "Mix", 0, 1, 0.35f),
            P("damp", "Damping", 500, 16000, 5000, "Hz", true, 60),
            E("pingpong", "Ping-pong", {"off", "on"})}},
        {"crush", "Bitcrush", "#f687b3", false, {
            P("bits", "Bits", 2, 16, 8, "bit", false, 0, 1),
            P("downsample", "Downsample", 1, 40, 4, "x", false, 0, 1),
            P("mix", "Mix", 0, 1, 1)}},
        {"stutter", "Stutter", "#f6e05e", false, {
            P("size", "Slice", 15, 1000, 125, "ms", true, 0),
            P("decay", "Decay", 0, 1, 0, "", false, 0)}},
        {"reverb", "Reverb", "#68d391", true, {
            P("size", "Size", 0, 1, 0.6f),
            P("damp", "Damping", 0, 1, 0.4f),
            P("mix", "Mix", 0, 1, 0.3f),
            P("predelay", "Pre-delay", 0, 120, 10, "ms", false, 0)}},
    };
    return d;
}

int effectIndex(const std::string& id) {
    const auto& d = effectDescs();
    for (size_t i = 0; i < d.size(); i++)
        if (d[i].id == id) return (int)i;
    return -1;
}

// ---------------------------------------------------------------- smoother
void Smoother::set(float v) {
    target_ = v;
    float tau = ms_ * 1e-3f * (float)sr_;
    rate_ = tau > 0 ? (target_ - cur_) / tau : 0.0f;
    if (tau <= 0) cur_ = target_;
}

float Smoother::advance(int n) {
    if (cur_ == target_ || n <= 0) return cur_;
    float end = cur_ + rate_ * (float)n;
    if ((rate_ > 0 && end >= target_) || (rate_ < 0 && end <= target_) || rate_ == 0) end = target_;
    cur_ = end;
    return cur_;
}

// ---------------------------------------------------------------- biquad
void Biquad::design(Kind kind, double sr, double f0, double q, double gainDb) {
    f0 = std::max(10.0, std::min(f0, sr * 0.49));
    double w0 = 2 * kPi * f0 / sr, cw = std::cos(w0), sw = std::sin(w0);
    double alpha = sw / (2 * std::max(q, 0.05));
    double A = std::pow(10.0, gainDb / 40.0);
    double b0, b1, b2, a0, a1, a2;
    switch (kind) {
    case Lowpass: b0 = (1 - cw) / 2; b1 = 1 - cw; b2 = (1 - cw) / 2; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha; break;
    case Highpass: b0 = (1 + cw) / 2; b1 = -(1 + cw); b2 = (1 + cw) / 2; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha; break;
    case Bandpass: b0 = alpha; b1 = 0; b2 = -alpha; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha; break;
    case Notch: b0 = 1; b1 = -2 * cw; b2 = 1; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha; break;
    case Peak: b0 = 1 + alpha * A; b1 = -2 * cw; b2 = 1 - alpha * A; a0 = 1 + alpha / A; a1 = -2 * cw; a2 = 1 - alpha / A; break;
    case LowShelf: {
        double sa = 2 * std::sqrt(A) * alpha;
        b0 = A * ((A + 1) - (A - 1) * cw + sa); b1 = 2 * A * ((A - 1) - (A + 1) * cw); b2 = A * ((A + 1) - (A - 1) * cw - sa);
        a0 = (A + 1) + (A - 1) * cw + sa; a1 = -2 * ((A - 1) + (A + 1) * cw); a2 = (A + 1) + (A - 1) * cw - sa;
        break;
    }
    case HighShelf: default: {
        double sa = 2 * std::sqrt(A) * alpha;
        b0 = A * ((A + 1) + (A - 1) * cw + sa); b1 = -2 * A * ((A - 1) + (A + 1) * cw); b2 = A * ((A + 1) + (A - 1) * cw - sa);
        a0 = (A + 1) - (A - 1) * cw + sa; a1 = 2 * ((A - 1) - (A + 1) * cw); a2 = (A + 1) - (A - 1) * cw - sa;
        break;
    }
    }
    b0_ = b0 / a0; b1_ = b1 / a0; b2_ = b2 / a0; a1_ = a1 / a0; a2_ = a2 / a0;
}

double Biquad::magnitude(double f, double sr) const {
    double w = 2 * kPi * f / sr;
    double cr = std::cos(w), ci = -std::sin(w), c2r = std::cos(2 * w), c2i = -std::sin(2 * w);
    double nr = b0_ + b1_ * cr + b2_ * c2r, ni = b1_ * ci + b2_ * c2i;
    double dr = 1 + a1_ * cr + a2_ * c2r, di = a1_ * ci + a2_ * c2i;
    return std::sqrt((nr * nr + ni * ni) / (dr * dr + di * di));
}

// ---------------------------------------------------------------- base effect
void Effect::prepare(int sr, int maxBlock) {
    sr_ = sr;
    maxBlock_ = maxBlock;
    values_.resize(desc_.params.size());
    sm_.resize(desc_.params.size());
    for (size_t i = 0; i < desc_.params.size(); i++) {
        const auto& p = desc_.params[i];
        values_[i] = p.def;
        sm_[i].init(p.def, sr, p.isEnum() ? 0.0f : p.smoothMs);
    }
    enable_.init(0.0f, sr, 15.0f);
    dry_.assign((size_t)maxBlock * 2, 0.0f);
    wet_.assign((size_t)maxBlock * 2, 0.0f);
    setup();
}

void Effect::setParam(int idx, float v) {
    if (idx < 0 || idx >= (int)values_.size()) return;
    v = desc_.params[idx].clamp(v);
    values_[idx] = v;
    sm_[idx].set(v);
    onParam(idx, v);
}

void Effect::resetParams() {
    for (size_t i = 0; i < desc_.params.size(); i++) setParam((int)i, desc_.params[i].def);
}

void Effect::setEnabled(bool on) {
    if (on && !enabled_ && !desc_.tail) reset();
    enabled_ = on;
    enable_.set(on ? 1.0f : 0.0f);
    if (on) tailQuiet_ = false;
}

void Effect::run(float* x, int n) {
    if (desc_.tail) {
        if (!enabled_ && tailQuiet_ && enable_.settled()) return;
        float* in = dry_.data();
        for (int i = 0; i < n; i++) {
            float m = enable_.next();
            in[2 * i] = x[2 * i] * m;
            in[2 * i + 1] = x[2 * i + 1] * m;
        }
        processWet(in, wet_.data(), n);
        float peak = 0;
        for (int i = 0; i < 2 * n; i++) {
            x[i] += wet_[i];
            peak = std::max(peak, std::fabs(wet_[i]));
        }
        if (!enabled_ && enable_.settled()) tailQuiet_ = peak < 1e-4f;
        return;
    }
    if (!enabled_ && enable_.settled()) return;
    std::memcpy(dry_.data(), x, sizeof(float) * 2 * n);
    process(x, n);
    for (int i = 0; i < n; i++) {
        float m = enable_.next();
        x[2 * i] = dry_[2 * i] * (1 - m) + x[2 * i] * m;
        x[2 * i + 1] = dry_[2 * i + 1] * (1 - m) + x[2 * i + 1] * m;
    }
}

// ---------------------------------------------------------------- effects
namespace {
constexpr int kSub = 32;   // coefficient update interval (samples)

class Filter : public Effect {
public:
    using Effect::Effect;
    enum { MODE, CUTOFF, Q, DRIVE };
    Biquad bq;
    void reset() override { bq.reset(); }
    void process(float* x, int n) override {
        static const Biquad::Kind kinds[] = {Biquad::Lowpass, Biquad::Highpass, Biquad::Bandpass};
        for (int off = 0; off < n; off += kSub) {
            int c = std::min(kSub, n - off);
            bq.design(kinds[choice(MODE)], sr_, advance(CUTOFF, c), advance(Q, c));
            for (int i = off; i < off + c; i++) {
                float g = dbToLin(next(DRIVE));
                x[2 * i] = bq.tick(0, x[2 * i] * g);
                x[2 * i + 1] = bq.tick(1, x[2 * i + 1] * g);
            }
        }
    }
};

class EQ : public Effect {
public:
    using Effect::Effect;
    enum { LOW, MID, MIDF, HIGH };
    Biquad lo, mid, hi;
    void reset() override { lo.reset(); mid.reset(); hi.reset(); }
    void process(float* x, int n) override {
        for (int off = 0; off < n; off += kSub) {
            int c = std::min(kSub, n - off);
            lo.design(Biquad::LowShelf, sr_, 200.0, 0.7071, advance(LOW, c));
            mid.design(Biquad::Peak, sr_, advance(MIDF, c), 1.0, advance(MID, c));
            hi.design(Biquad::HighShelf, sr_, 4000.0, 0.7071, advance(HIGH, c));
            for (int i = off; i < off + c; i++)
                for (int ch = 0; ch < 2; ch++)
                    x[2 * i + ch] = hi.tick(ch, mid.tick(ch, lo.tick(ch, x[2 * i + ch])));
        }
    }
};

// soft: tanh (rounded), hard: clip (flat tops), asym: offset tanh (even harmonics), fold: wavefolder
class Drive : public Effect {
public:
    using Effect::Effect;
    enum { MODE, DRIVE, TONE, LEVEL };
    Biquad tone, dc;
    void setup() override { dc.design(Biquad::Highpass, sr_, 15.0); }
    void reset() override { tone.reset(); dc.reset(); }
    void process(float* x, int n) override {
        const int mode = choice(MODE);
        const float t035 = std::tanh(0.35f);
        for (int off = 0; off < n; off += kSub) {
            int c = std::min(kSub, n - off);
            tone.design(Biquad::Lowpass, sr_, advance(TONE, c));
            for (int i = off; i < off + c; i++) {
                float g = dbToLin(next(DRIVE)), lvl = dbToLin(next(LEVEL));
                for (int ch = 0; ch < 2; ch++) {
                    float p = x[2 * i + ch] * g, y;
                    switch (mode) {
                    case 0: y = std::tanh(p); break;
                    case 1: y = std::max(-0.8f, std::min(0.8f, p)) * 1.25f; break;
                    case 2: y = std::tanh(p + 0.35f) - t035; break;
                    default: y = std::sin(p * 1.5708f); break;
                    }
                    x[2 * i + ch] = tone.tick(ch, dc.tick(ch, y)) * lvl;
                }
            }
        }
    }
};

class Tremolo : public Effect {
public:
    using Effect::Effect;
    enum { RATE, DEPTH, SHAPE, SPREAD };
    double phase = 0;
    void reset() override { phase = 0; }
    static float lfo(int shape, double ph) {
        ph -= std::floor(ph);
        if (shape == 0) return (float)(0.5 - 0.5 * std::cos(2 * kPi * ph));
        if (shape == 1) return (float)(1 - std::fabs(2 * ph - 1));
        return ph < 0.5 ? 1.0f : 0.0f;
    }
    void process(float* x, int n) override {
        const int shape = choice(SHAPE);
        for (int i = 0; i < n; i++) {
            phase += next(RATE) / sr_;
            phase -= std::floor(phase);
            double spread = next(SPREAD) / 360.0;
            float depth = next(DEPTH);
            x[2 * i] *= 1 - depth * (1 - lfo(shape, phase));
            x[2 * i + 1] *= 1 - depth * (1 - lfo(shape, phase + spread));
        }
    }
};

// Stereo delay with fractional (tape-style) time changes and damping in the loop.
class Delay : public Effect {
public:
    using Effect::Effect;
    enum { TIME, FEEDBACK, MIX, DAMP, PINGPONG };
    std::vector<float> buf;
    int L = 0, w = 0;
    Biquad lp;
    void setup() override {
        L = (int)(sr_ * 2.0) + maxBlock_ * 2 + 8;
        buf.assign((size_t)L * 2, 0.0f);
        w = 0;
    }
    void reset() override { std::fill(buf.begin(), buf.end(), 0.0f); lp.reset(); }
    void processWet(const float* in, float* wet, int n) override {
        const bool pp = choice(PINGPONG) == 1;
        for (int off = 0; off < n; off += kSub) {
            int c = std::min(kSub, n - off);
            lp.design(Biquad::Lowpass, sr_, advance(DAMP, c));
            for (int i = off; i < off + c; i++) {
                double d = next(TIME) * (sr_ / 1000.0);
                d = std::max(2.0, std::min(d, (double)L - 4));
                double pos = w - d;
                double fl = std::floor(pos);
                int i0 = (int)fl;
                float frac = (float)(pos - fl);
                int a = ((i0 % L) + L) % L, b = (a + 1) % L;
                float dl = buf[2 * a] * (1 - frac) + buf[2 * b] * frac;
                float dr = buf[2 * a + 1] * (1 - frac) + buf[2 * b + 1] * frac;
                float fb = next(FEEDBACK);
                float fl0 = lp.tick(0, dl) * fb, fr0 = lp.tick(1, dr) * fb;
                if (pp) std::swap(fl0, fr0);
                buf[2 * w] = in[2 * i] + fl0;
                buf[2 * w + 1] = in[2 * i + 1] + fr0;
                w = (w + 1) % L;
                float mix = next(MIX);
                wet[2 * i] = dl * mix;
                wet[2 * i + 1] = dr * mix;
            }
        }
    }
};

class Crush : public Effect {
public:
    using Effect::Effect;
    enum { BITS, DOWN, MIX };
    float hold[2] = {0, 0};
    int count = 0;
    void reset() override { hold[0] = hold[1] = 0; count = 0; }
    void process(float* x, int n) override {
        const int k = std::max(1, (int)values_[DOWN]);
        const float step = std::pow(2.0f, (float)((int)values_[BITS] - 1));
        for (int i = 0; i < n; i++) {
            if (count % k == 0) { hold[0] = x[2 * i]; hold[1] = x[2 * i + 1]; }
            count = (count + 1) % k;
            float m = next(MIX);
            for (int ch = 0; ch < 2; ch++) {
                float y = std::round(hold[ch] * step) / step;
                x[2 * i + ch] = x[2 * i + ch] * (1 - m) + y * m;
            }
        }
    }
};

// Beat repeat: when engaged, loops the last `size` ms of audio.
class Stutter : public Effect {
public:
    using Effect::Effect;
    enum { SIZE, DECAY };
    std::vector<float> hist, loop;
    int hw = 0, loopLen = 0, lp = 0;
    float gain = 1;
    void setup() override {
        hist.assign((size_t)sr_ * 2 * 2, 0.0f);
        loop.assign((size_t)sr_ * 2, 0.0f);   // up to 1 s
    }
    void setEnabled(bool on) override {
        if (on && !enabled_) {
            int size = std::min((int)(values_[SIZE] * sr_ / 1000.0f), sr_);
            int H = (int)hist.size() / 2;
            for (int i = 0; i < size; i++) {
                int j = ((hw - size + i) % H + H) % H;
                loop[2 * i] = hist[2 * j];
                loop[2 * i + 1] = hist[2 * j + 1];
            }
            int f = std::min(64, size / 4);
            for (int i = 0; i < f; i++) {
                float r = f > 1 ? (float)i / (float)(f - 1) : 1.0f;
                for (int ch = 0; ch < 2; ch++) {
                    loop[2 * i + ch] *= r;
                    loop[2 * (size - 1 - i) + ch] *= r;
                }
            }
            loopLen = size;
            lp = 0;
            gain = 1;
        }
        Effect::setEnabled(on);
    }
    void run(float* x, int n) override {
        int H = (int)hist.size() / 2;        // always record: the next grab is fresh material
        for (int i = 0; i < n; i++) {
            hist[2 * hw] = x[2 * i];
            hist[2 * hw + 1] = x[2 * i + 1];
            hw = (hw + 1) % H;
        }
        Effect::run(x, n);
    }
    void process(float* x, int n) override {
        if (loopLen <= 0) return;
        for (int i = 0; i < n; i++) {
            x[2 * i] = loop[2 * lp] * gain;
            x[2 * i + 1] = loop[2 * lp + 1] * gain;
            if (++lp >= loopLen) {
                lp = 0;
                if (values_[DECAY] > 0) gain *= 1 - 0.5f * values_[DECAY];
            }
        }
    }
};

// Freeverb: 8 damped combs in parallel into 4 allpasses, per channel.
class Reverb : public Effect {
public:
    using Effect::Effect;
    enum { SIZE, DAMP, MIX, PREDELAY };
    struct Line { std::vector<float> b; int i = 0; float lp = 0; };
    Line combs[16], aps[8];
    std::vector<float> pre;
    int preW = 0;
    void setup() override {
        static const int C[8] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
        static const int A[4] = {556, 441, 341, 225};
        const double scale = sr_ / 44100.0;
        for (int s = 0; s < 2; s++) {
            for (int k = 0; k < 8; k++) combs[s * 8 + k].b.assign((size_t)(C[k] * scale) + s * 23, 0.0f);
            for (int k = 0; k < 4; k++) aps[s * 4 + k].b.assign((size_t)(A[k] * scale) + s * 23, 0.0f);
        }
        pre.assign((size_t)(sr_ * 0.13) * 2, 0.0f);
    }
    void reset() override {
        for (auto& c : combs) { std::fill(c.b.begin(), c.b.end(), 0.0f); c.lp = 0; }
        for (auto& a : aps) std::fill(a.b.begin(), a.b.end(), 0.0f);
        std::fill(pre.begin(), pre.end(), 0.0f);
    }
    void processWet(const float* in, float* wet, int n) override {
        const float fb = 0.7f + 0.28f * values_[SIZE];
        const float damp = 0.1f + 0.85f * values_[DAMP];
        const int P = (int)pre.size() / 2;
        const int pd = std::min(P - 1, (int)(values_[PREDELAY] * sr_ / 1000.0f));
        for (int i = 0; i < n; i++) {
            pre[2 * preW] = in[2 * i];
            pre[2 * preW + 1] = in[2 * i + 1];
            int r = ((preW - pd) % P + P) % P;
            preW = (preW + 1) % P;
            float mono = (pre[2 * r] + pre[2 * r + 1]) * 0.015f;
            float mix = next(MIX);
            for (int s = 0; s < 2; s++) {
                float sig = 0;
                for (int k = 0; k < 8; k++) {
                    Line& c = combs[s * 8 + k];
                    float y = c.b[c.i];
                    c.lp = y * (1 - damp) + c.lp * damp;
                    c.b[c.i] = mono + c.lp * fb;
                    if (++c.i >= (int)c.b.size()) c.i = 0;
                    sig += y;
                }
                for (int k = 0; k < 4; k++) {
                    Line& a = aps[s * 4 + k];
                    float bo = a.b[a.i];
                    a.b[a.i] = sig + 0.5f * bo;
                    if (++a.i >= (int)a.b.size()) a.i = 0;
                    sig = bo - sig;
                }
                wet[2 * i + s] = sig * mix;
            }
        }
    }
};
}  // namespace

std::unique_ptr<Effect> makeEffect(const std::string& id) {
    const int i = effectIndex(id);
    if (i < 0) return nullptr;
    const EffectDesc& d = effectDescs()[i];
    if (id == "filter") return std::make_unique<Filter>(d);
    if (id == "eq") return std::make_unique<EQ>(d);
    if (id == "drive") return std::make_unique<Drive>(d);
    if (id == "tremolo") return std::make_unique<Tremolo>(d);
    if (id == "delay") return std::make_unique<Delay>(d);
    if (id == "crush") return std::make_unique<Crush>(d);
    if (id == "stutter") return std::make_unique<Stutter>(d);
    if (id == "reverb") return std::make_unique<Reverb>(d);
    return nullptr;
}

// ---------------------------------------------------------------- limiter
void Limiter::prepare(int sr, float ceiling, float releaseMs) {
    ceiling_ = ceiling;
    gain_ = 1.0f;
    release_ = std::exp(-1.0f / (releaseMs * 1e-3f * (float)sr));
    reductionDb_ = 0;
}

void Limiter::process(float* x, int n) {
    if (n <= 0) return;
    float peak = 0;
    for (int i = 0; i < 2 * n; i++) peak = std::max(peak, std::fabs(x[i]));
    const float target = peak <= ceiling_ ? 1.0f : ceiling_ / peak;
    float end;
    if (target < gain_) end = target;                    // instant attack
    else end = std::min(target, 1.0f - (1.0f - gain_) * std::pow(release_, (float)n));
    for (int i = 0; i < n; i++) {
        float g = gain_ + (end - gain_) * (float)i / (float)n;
        x[2 * i] = std::max(-1.0f, std::min(1.0f, x[2 * i] * g));
        x[2 * i + 1] = std::max(-1.0f, std::min(1.0f, x[2 * i + 1] * g));
    }
    gain_ = end;
    reductionDb_ = 20.0f * std::log10(std::max(end, 1e-6f));
}

// ---------------------------------------------------------------- chain
Chain::Chain(int sr, int maxBlock) : sr_(sr), maxBlock_(maxBlock) {
    for (const auto& d : effectDescs()) {
        auto fx = makeEffect(d.id);
        fx->prepare(sr, maxBlock);
        fx_.push_back(std::move(fx));
    }
    master_.init(dbToLin(masterDb_), sr, 20.0f);
    bypass_.init(1.0f, sr, 15.0f);
    limiter_.prepare(sr);
    dry_.assign((size_t)maxBlock * 2, 0.0f);
}

Effect* Chain::byId(const std::string& id) {
    for (auto& f : fx_)
        if (f->id() == id) return f.get();
    return nullptr;
}

void Chain::setMasterDb(float db) {
    masterDb_ = std::max(-60.0f, std::min(12.0f, db));
    master_.set(masterDb_ > -60.0f ? dbToLin(masterDb_) : 0.0f);
}

void Chain::setBypassAll(bool on) {
    bypassAll_ = on;
    bypass_.set(on ? 0.0f : 1.0f);
}

void Chain::panic() {
    for (auto& f : fx_) {
        f->setEnabled(false);
        f->resetParams();
    }
    setBypassAll(false);
}

void Chain::process(float* x, int n) {
    for (int off = 0; off < n; off += maxBlock_) processChunk(x + 2 * off, std::min(maxBlock_, n - off));
}

void Chain::processChunk(float* x, int n) {
    if (!(bypassAll_ && bypass_.settled())) {
        const bool ramp = !bypass_.settled() || bypassAll_;
        if (ramp) std::memcpy(dry_.data(), x, sizeof(float) * 2 * n);
        for (auto& f : fx_) f->run(x, n);
        if (ramp) {
            for (int i = 0; i < n; i++) {
                float m = bypass_.next();
                x[2 * i] = dry_[2 * i] * (1 - m) + x[2 * i] * m;
                x[2 * i + 1] = dry_[2 * i + 1] * (1 - m) + x[2 * i + 1] * m;
            }
        }
    }
    for (int i = 0; i < n; i++) {
        float g = master_.next();
        x[2 * i] *= g;
        x[2 * i + 1] *= g;
    }
    limiter_.process(x, n);
}

}  // namespace pifx
