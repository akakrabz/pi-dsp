#include "core/Sources.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "core/Dsp.h"
#include "miniaudio.h"

namespace pifx {

static constexpr double kTwoPi = 6.28318530717958647692;

const std::vector<std::string> kWaves = {"sine", "square", "triangle", "saw", "white", "pink", "sweep"};
const std::vector<std::string> kShapes = {"circle", "lissajous", "rose", "figure8", "line", "square", "star", "spiral"};

const char* sourceKindName(SourceKind k) {
    switch (k) {
    case SourceKind::Tone: return "tone";
    case SourceKind::File: return "file";
    case SourceKind::Capture: return "capture";
    default: return "silence";
    }
}

void SilenceSource::pull(float* out, int n) { std::memset(out, 0, sizeof(float) * 2 * n); }

// ---------------------------------------------------------------- tone
void ToneSource::setParams(const ToneParams& p) {
    bool restartSweep = p.wave != p_.wave && p.wave == 6;   // switching to "sweep"
    p_ = p;
    p_.wave = std::clamp(p_.wave, 0, (int)kWaves.size() - 1);
    p_.shape = std::clamp(p_.shape, 0, (int)kShapes.size() - 1);
    p_.a = std::clamp(p_.a, 1, 16);
    p_.b = std::clamp(p_.b, 1, 16);
    if (restartSweep) sweepT_ = 0;
}

float ToneSource::noise() {
    // xorshift64* -> two uniforms -> Box-Muller (one value used)
    auto u = [this]() {
        rng_ ^= rng_ >> 12; rng_ ^= rng_ << 25; rng_ ^= rng_ >> 27;
        return ((rng_ * 0x2545F4914F6CDD1Dull) >> 11) * (1.0 / 9007199254740992.0);
    };
    double a = std::max(u(), 1e-12), b = u();
    return (float)(std::sqrt(-2.0 * std::log(a)) * std::cos(kTwoPi * b));
}

void ToneSource::pull(float* out, int n) {
    const float amp = dbToLin(p_.level);
    if (p_.mode == 1) { shape(out, n, amp); return; }
    enum { SINE, SQUARE, TRIANGLE, SAW, WHITE, PINK, SWEEP };
    const int w = p_.wave;
    const double f = p_.freq;
    if (w == WHITE) {
        for (int i = 0; i < n; i++) out[2 * i] = out[2 * i + 1] = noise() * amp * 0.3f;
        return;
    }
    if (w == PINK) {
        // Paul Kellet's economy filter on white noise (3rd order IIR).
        static const double B[4] = {0.049922035, -0.095993537, 0.050612699, -0.004408786};
        static const double A[4] = {1.0, -2.494956002, 2.017265875, -0.522189400};
        for (int i = 0; i < n; i++) {
            double x = noise();
            double y = B[0] * x + pz_[0];
            pz_[0] = B[1] * x - A[1] * y + pz_[1];
            pz_[1] = B[2] * x - A[2] * y + pz_[2];
            pz_[2] = B[3] * x - A[3] * y;
            out[2 * i] = out[2 * i + 1] = (float)(y * amp * 2.0);
        }
        return;
    }
    if (w == SWEEP) {
        const double T = std::max(0.5f, p_.sweepSec), lo = std::max(1.0f, p_.sweepLo), hi = std::max(p_.sweepLo + 1, p_.sweepHi);
        for (int i = 0; i < n; i++) {
            double inst = lo * std::pow(hi / lo, sweepT_ / T);
            phase_ += inst / sr_;
            phase_ -= std::floor(phase_);
            sweepT_ += 1.0 / sr_;
            if (sweepT_ >= T) sweepT_ -= T;
            out[2 * i] = out[2 * i + 1] = (float)(amp * std::sin(kTwoPi * phase_));
        }
        return;
    }
    const double inc = f / sr_;
    for (int i = 0; i < n; i++) {
        double ph = phase_;
        phase_ += inc;
        phase_ -= std::floor(phase_);
        float s;
        if (w == SQUARE) s = ph < 0.5 ? 1.0f : -1.0f;
        else if (w == TRIANGLE) s = (float)(1 - 4 * std::fabs(ph - 0.5));
        else if (w == SAW) s = (float)(2 * ph - 1);
        else s = (float)std::sin(kTwoPi * ph);
        out[2 * i] = out[2 * i + 1] = s * amp;
    }
}

void ToneSource::shape(float* out, int n, float amp) {
    const double inc = p_.freq / sr_;
    const int a = p_.a, b = p_.b;
    const double phi = p_.phaseDeg * kTwoPi / 360.0;
    enum { CIRCLE, LISSAJOUS, ROSE, FIGURE8, LINE, SQUARE, STAR, SPIRAL };
    const int s = p_.shape;
    for (int i = 0; i < n; i++) {
        double u = phase_;
        phase_ += inc;
        phase_ -= std::floor(phase_);
        double th = kTwoPi * u, x, y;
        if (s == CIRCLE) { x = std::cos(th); y = std::sin(th); }
        else if (s == LISSAJOUS) { x = std::sin(a * th); y = std::sin(b * th + phi); }
        else if (s == ROSE) { double r = std::cos(a * th); x = r * std::cos(th); y = r * std::sin(th); }
        else if (s == FIGURE8) { x = std::sin(th); y = std::sin(2 * th) * 0.5; }
        else if (s == LINE) { x = y = std::sin(th); }
        else if (s == SQUARE) {
            // trace the perimeter at constant speed
            double v = u * 4.0;
            int seg = (int)std::floor(v) % 4;
            double t = v - std::floor(v);
            switch (seg) {
            case 0: x = -1 + 2 * t; y = -1; break;
            case 1: x = 1; y = -1 + 2 * t; break;
            case 2: x = 1 - 2 * t; y = 1; break;
            default: x = -1; y = 1 - 2 * t; break;
            }
        } else if (s == STAR) {
            int k = std::max(3, a);
            double r = 0.55 + 0.45 * std::cos(k * th);
            x = r * std::cos(th); y = r * std::sin(th);
        } else {  // spiral: radius grows over one trace
            double r = u;
            x = r * std::cos(a * th); y = r * std::sin(a * th);
        }
        out[2 * i] = (float)(x * amp);
        out[2 * i + 1] = (float)(y * amp);
    }
}

// ---------------------------------------------------------------- files
bool isAudioFile(const std::string& name) {
    auto dot = name.rfind('.');
    if (dot == std::string::npos) return false;
    std::string ext = name.substr(dot + 1);
    for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
    return ext == "wav" || ext == "flac" || ext == "mp3";
}

std::shared_ptr<AudioClip> loadClip(const std::string& path, int sampleRate, std::string* err) {
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 2, (ma_uint32)sampleRate);
    ma_decoder dec;
    ma_result r = ma_decoder_init_file(path.c_str(), &cfg, &dec);
    if (r != MA_SUCCESS) {
        if (err) *err = std::string("cannot open ") + path + ": " + ma_result_description(r);
        return nullptr;
    }
    auto clip = std::make_shared<AudioClip>();
    clip->path = path;
    auto slash = path.find_last_of('/');
    clip->name = slash == std::string::npos ? path : path.substr(slash + 1);
    clip->sampleRate = sampleRate;
    ma_uint64 total = 0;
    if (ma_decoder_get_length_in_pcm_frames(&dec, &total) == MA_SUCCESS && total > 0)
        clip->data.reserve((size_t)total * 2);
    std::vector<float> chunk(4096 * 2);
    for (;;) {
        ma_uint64 got = 0;
        r = ma_decoder_read_pcm_frames(&dec, chunk.data(), 4096, &got);
        clip->data.insert(clip->data.end(), chunk.begin(), chunk.begin() + (size_t)got * 2);
        if (got < 4096 || r != MA_SUCCESS) break;
        if (clip->data.size() > (size_t)sampleRate * 2 * 60 * 30) break;   // 30 min cap
    }
    ma_decoder_uninit(&dec);
    if (clip->data.empty()) {
        if (err) *err = "no audio in " + path;
        return nullptr;
    }
    return clip;
}

void FileSource::pull(float* out, int n) {
    const int64_t len = (int64_t)clip_->frames();
    int64_t seek = seekTo.exchange(-1);
    int64_t p = pos.load(std::memory_order_relaxed);
    if (seek >= 0) p = std::min(seek, std::max<int64_t>(0, len - 1));
    if (!playing.load() || len == 0) {
        std::memset(out, 0, sizeof(float) * 2 * n);
        pos.store(p);
        return;
    }
    const float g = dbToLin(gainDb.load());
    const float* d = clip_->data.data();
    const bool lp = loop.load();
    for (int i = 0; i < n; i++) {
        if (p >= len) {
            if (lp) p = 0;
            else {
                std::memset(out + 2 * i, 0, sizeof(float) * 2 * (n - i));
                playing.store(false);
                p = 0;
                break;
            }
        }
        out[2 * i] = d[2 * p] * g;
        out[2 * i + 1] = d[2 * p + 1] * g;
        p++;
    }
    pos.store(p);
}

// ---------------------------------------------------------------- capture
CaptureSource::CaptureSource(int sr, int block) : sr_(sr), block_(block) {
    ring_.reset((size_t)std::max(block * 16, 4096), 2);
    conv_.resize(8192 * 2);
}

CaptureSource::~CaptureSource() { close(); }

static void captureCallback(ma_device* dev, void* out, const void* in, ma_uint32 frames) {
    auto* self = (CaptureSource*)dev->pUserData;
    if (self && in) self->feed((const float*)in, (int)frames, (int)dev->capture.channels);
}

bool CaptureSource::open(ma_context* ctx, const void* deviceId, const std::string& name, std::string* err) {
    close();
    dev_ = new ma_device;
    ma_device_config cfg = ma_device_config_init(ma_device_type_capture);
    cfg.capture.pDeviceID = (const ma_device_id*)deviceId;
    cfg.capture.format = ma_format_f32;
    cfg.capture.channels = 2;
    cfg.sampleRate = (ma_uint32)sr_;
    cfg.periodSizeInFrames = (ma_uint32)block_;
    cfg.performanceProfile = ma_performance_profile_low_latency;
    cfg.dataCallback = captureCallback;
    cfg.pUserData = this;
    ma_result r = ma_device_init(ctx, &cfg, dev_);
    if (r != MA_SUCCESS) {
        delete dev_;
        dev_ = nullptr;
        if (err) *err = std::string("capture open failed: ") + ma_result_description(r);
        return false;
    }
    r = ma_device_start(dev_);
    if (r != MA_SUCCESS) {
        ma_device_uninit(dev_);
        delete dev_;
        dev_ = nullptr;
        if (err) *err = std::string("capture start failed: ") + ma_result_description(r);
        return false;
    }
    name_ = name;
    return true;
}

void CaptureSource::close() {
    if (dev_) {
        ma_device_uninit(dev_);
        delete dev_;
        dev_ = nullptr;
    }
}

void CaptureSource::feed(const float* in, int frames, int channels) {
    float pk = 0;
    while (frames > 0) {
        int n = std::min(frames, (int)conv_.size() / 2);
        for (int i = 0; i < n; i++) {
            float l = in[i * channels], r = channels > 1 ? in[i * channels + 1] : l;
            conv_[2 * i] = l;
            conv_[2 * i + 1] = r;
            pk = std::max(pk, std::max(std::fabs(l), std::fabs(r)));
        }
        size_t w = ring_.write(conv_.data(), (size_t)n);
        if ((int)w < n) overruns.fetch_add(1);
        in += (size_t)n * channels;
        frames -= n;
    }
    peak.store(std::max(pk, peak.load() * 0.9f));
}

void CaptureSource::pull(float* out, int n) {
    size_t have = ring_.readable();
    if (have < (size_t)n) {
        underruns.fetch_add(1);
        std::memset(out, 0, sizeof(float) * 2 * n);
        return;
    }
    // keep latency bounded: far behind -> skip ahead to two blocks of slack
    if (have > (size_t)(6 * std::max(block_, n))) {
        ring_.skip(have - (size_t)(2 * std::max(block_, n)));
        overruns.fetch_add(1);
    }
    ring_.read(out, (size_t)n);
    const float g = dbToLin(gainDb.load());
    if (g != 1.0f)
        for (int i = 0; i < 2 * n; i++) out[i] *= g;
}

}  // namespace pifx
