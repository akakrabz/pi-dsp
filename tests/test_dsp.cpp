// Port of the Python DSP tests: same signals, same expectations.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <random>
#include <set>

#include "check.h"
#include "core/Dsp.h"

using namespace pifx;

static constexpr int SR = 48000, N = 256;

static std::vector<float> sine(double f, int blocks = 40, float amp = 0.5f) {
    std::vector<float> x((size_t)blocks * N * 2);
    for (int i = 0; i < blocks * N; i++) x[2 * i] = x[2 * i + 1] = amp * (float)std::sin(2 * M_PI * f * i / SR);
    return x;
}
static std::vector<float> runBlocks(Effect& fx, std::vector<float> x) {
    const int frames = (int)x.size() / 2;
    for (int i = 0; i < frames; i += N) fx.run(&x[2 * i], std::min(N, frames - i));
    return x;
}
static double rmsDb(const std::vector<float>& x, size_t from = 0) {
    double s = 0;
    size_t n = 0;
    for (size_t i = from * 2; i < x.size(); i += 2) { s += x[i] * x[i]; n++; }
    return 20 * std::log10(std::max(std::sqrt(s / std::max<size_t>(n, 1)), 1e-12));
}
static std::unique_ptr<Effect> make(const char* id) {
    auto fx = makeEffect(id);
    fx->prepare(SR, 1024);
    return fx;
}
static void set(Effect& fx, const char* p, float v) { fx.setParam(fx.desc().paramIndex(p), v); }

TEST(dsp_every_effect_runs_clean) {
    std::mt19937 rng(1);
    for (const auto& d : effectDescs()) {
        auto fx = make(d.id.c_str());
        fx->setEnabled(true);
        auto y = runBlocks(*fx, sine(440, 30));
        CHECK(std::all_of(y.begin(), y.end(), [](float v) { return std::isfinite(v); }));
        for (size_t p = 0; p < d.params.size(); p++) {
            std::uniform_real_distribution<float> u(d.params[p].min, d.params[p].max);
            fx->setParam((int)p, u(rng));
        }
        y = runBlocks(*fx, sine(440, 30));
        CHECK(std::all_of(y.begin(), y.end(), [](float v) { return std::isfinite(v); }));
    }
}

TEST(dsp_bypassed_effect_is_transparent) {
    for (const auto& d : effectDescs()) {
        auto fx = make(d.id.c_str());
        auto x = sine(1000, 10);
        CHECK(runBlocks(*fx, x) == x);
    }
}

TEST(dsp_lowpass_attenuates_highs) {
    auto fx = make("filter");
    set(*fx, "mode", 0);
    set(*fx, "cutoff", 500);
    fx->setEnabled(true);
    auto hi = runBlocks(*fx, sine(10000));
    auto fx2 = make("filter");
    set(*fx2, "mode", 0);
    set(*fx2, "cutoff", 500);
    fx2->setEnabled(true);
    auto lo = runBlocks(*fx2, sine(100));
    CHECK(rmsDb(hi, N * 10) < rmsDb(sine(10000)) - 30);
    CHECK(std::fabs(rmsDb(lo, N * 10) - rmsDb(sine(100))) < 1.0);
}

TEST(dsp_highpass_attenuates_lows) {
    auto fx = make("filter");
    set(*fx, "mode", 1);
    set(*fx, "cutoff", 2000);
    fx->setEnabled(true);
    auto lo = runBlocks(*fx, sine(100));
    CHECK(rmsDb(lo, N * 10) < rmsDb(sine(100)) - 30);
}

TEST(dsp_eq_low_boost) {
    auto fx = make("eq");
    set(*fx, "low", 12);
    fx->setEnabled(true);
    auto y = runBlocks(*fx, sine(60));
    double gain = rmsDb(y, N * 10) - rmsDb(sine(60));
    CHECK(gain > 10 && gain < 13);
}

TEST(dsp_drive_adds_harmonics_and_stays_bounded) {
    auto fx = make("drive");
    set(*fx, "drive", 30);
    set(*fx, "tone", 12000);
    set(*fx, "level", 0);
    fx->setEnabled(true);
    auto y = runBlocks(*fx, sine(200, 40, 0.8f));
    float peak = 0;
    for (float v : y) peak = std::max(peak, std::fabs(v));
    CHECK(peak <= 1.5f);
    // third harmonic via a single-bin DFT
    auto bin = [&](double f) {
        double re = 0, im = 0;
        for (int i = N * 10; i < (int)y.size() / 2; i++) {
            re += y[2 * i] * std::cos(2 * M_PI * f * i / SR);
            im += y[2 * i] * std::sin(2 * M_PI * f * i / SR);
        }
        return std::hypot(re, im);
    };
    CHECK(bin(600) > bin(200) * 0.05);
}

TEST(dsp_delay_echo_at_right_time) {
    auto fx = make("delay");
    set(*fx, "time", 100);
    set(*fx, "feedback", 0);
    set(*fx, "mix", 1.0f);
    set(*fx, "damp", 16000);
    fx->setEnabled(true);
    runBlocks(*fx, std::vector<float>((size_t)N * 80 * 2, 0.0f));
    std::vector<float> x((size_t)(SR / 2 / N * N) * 2, 0.0f);
    x[2 * 1000] = x[2 * 1000 + 1] = 1.0f;
    auto y = runBlocks(*fx, x);
    int peak = 1500;
    for (int i = 1500; i < (int)y.size() / 2; i++)
        if (std::fabs(y[2 * i]) > std::fabs(y[2 * peak])) peak = i;
    CHECK(std::abs(peak - (1000 + SR / 10)) <= 2);
    CHECK_NEAR(y[2 * 1000], 1.0, 1e-5);
}

TEST(dsp_delay_tail_rings_after_bypass_then_stops) {
    auto fx = make("delay");
    set(*fx, "time", 50);
    set(*fx, "feedback", 0.5f);
    set(*fx, "mix", 1.0f);
    fx->setEnabled(true);
    runBlocks(*fx, sine(440));
    fx->setEnabled(false);
    auto tail = runBlocks(*fx, std::vector<float>((size_t)N * 20 * 2, 0.0f));
    float m = 0;
    for (float v : tail) m = std::max(m, std::fabs(v));
    CHECK(m > 0.01f);
    auto quiet = runBlocks(*fx, std::vector<float>((size_t)N * 400 * 2, 0.0f));
    float q = 0;
    for (size_t i = quiet.size() - 2 * N; i < quiet.size(); i++) q = std::max(q, std::fabs(quiet[i]));
    CHECK(q < 1e-4f);
    CHECK(fx->tailQuiet());
}

TEST(dsp_crush_quantises) {
    auto fx = make("crush");
    set(*fx, "bits", 3);
    set(*fx, "downsample", 1);
    fx->setEnabled(true);
    runBlocks(*fx, sine(440, 10));
    auto y = runBlocks(*fx, sine(440, 10));
    std::set<int> levels;
    for (size_t i = 0; i < y.size(); i += 2) levels.insert((int)std::lround(y[i] * 4));
    CHECK(levels.size() <= 9);
}

TEST(dsp_crush_downsample_holds) {
    auto fx = make("crush");
    set(*fx, "bits", 16);
    set(*fx, "downsample", 8);
    fx->setEnabled(true);
    std::mt19937 rng(0);
    std::normal_distribution<float> g;
    std::vector<float> x((size_t)N * 4 * 2);
    for (auto& v : x) v = g(rng);
    runBlocks(*fx, x);
    auto y = runBlocks(*fx, x);
    for (int i = N * 2; i + 8 < N * 4; i += 8)
        for (int k = 1; k < 8; k++) CHECK_NEAR(y[2 * (i + k)], y[2 * i], 1e-6);
}

TEST(dsp_stutter_repeats_slice) {
    auto fx = make("stutter");
    set(*fx, "size", 100);
    std::mt19937 rng(2);
    std::normal_distribution<float> g;
    std::vector<float> x((size_t)N * 100 * 2);
    for (auto& v : x) v = g(rng) * 0.2f;
    runBlocks(*fx, x);
    fx->setEnabled(true);
    auto y = runBlocks(*fx, std::vector<float>((size_t)N * 60 * 2, 0.0f));
    const int size = SR / 10;
    for (int i = 0; i < size; i++) CHECK_NEAR(y[2 * (N * 4 + i)], y[2 * (N * 4 + size + i)], 1e-6);
    float m = 0;
    for (float v : y) m = std::max(m, std::fabs(v));
    CHECK(m > 0.01f);
}

TEST(dsp_reverb_has_tail) {
    auto fx = make("reverb");
    set(*fx, "mix", 1.0f);
    set(*fx, "size", 0.9f);
    fx->setEnabled(true);
    std::vector<float> x((size_t)N * 20 * 2, 0.0f);
    x[200] = x[201] = 0.8f;
    auto y = runBlocks(*fx, x);
    float late = 0;
    for (size_t i = (size_t)N * 10 * 2; i < y.size(); i++) late = std::max(late, std::fabs(y[i]));
    CHECK(late > 1e-3f);
}

TEST(dsp_tremolo_modulates) {
    auto fx = make("tremolo");
    set(*fx, "rate", 4);
    set(*fx, "depth", 1.0f);
    fx->setEnabled(true);
    std::vector<float> x((size_t)(SR / N * N) * 2, 0.5f);
    auto y = runBlocks(*fx, x);
    float mx = 0, mn = 1;
    for (size_t i = (size_t)N * 4 * 2; i < y.size(); i += 2) { mx = std::max(mx, y[i]); mn = std::min(mn, y[i]); }
    CHECK(mx > 0.49f && mn < 0.02f);
}

TEST(dsp_enable_crossfade_has_no_jump) {
    auto fx = make("tremolo");
    set(*fx, "rate", 0.1f);
    set(*fx, "depth", 1.0f);
    auto x = sine(200, 20, 0.9f);
    for (int b = 0; b < 20; b++) {
        if (b == 5) fx->setEnabled(true);
        if (b == 12) fx->setEnabled(false);
        fx->run(&x[2 * b * N], N);
    }
    float jump = 0, engaged = 0;
    for (int i = 1; i < 20 * N; i++) jump = std::max(jump, std::fabs(x[2 * i] - x[2 * (i - 1)]));
    for (int i = N * 8; i < N * 12; i++) engaged = std::max(engaged, std::fabs(x[2 * i]));
    CHECK(jump < 0.03f);
    CHECK(engaged < 0.5f);
}

TEST(dsp_limiter_caps_peaks) {
    Limiter lim;
    lim.prepare(SR);
    std::vector<float> x;
    for (int k = 0; k < 5; k++) {
        x.assign((size_t)N * 2, 3.0f);
        lim.process(x.data(), N);
    }
    float m = 0;
    for (float v : x) m = std::max(m, std::fabs(v));
    CHECK(m <= 1.0f);
    CHECK(lim.reductionDb() < -9);
}

TEST(dsp_smoother_settles_exactly) {
    Smoother s;
    s.init(0, SR, 10);
    s.set(1.0f);
    for (int i = 0; i < 1000; i++) s.next();
    CHECK(s.settled() && s.value() == 1.0f);
    s.set(-1.0f);
    s.advance(100000);
    CHECK(s.value() == -1.0f);
}

TEST(dsp_param_clamp_and_enum) {
    const ParamSpec& mode = effectDescs()[effectIndex("filter")].params[0];
    CHECK(mode.choiceIndex("bandpass") == 2);
    CHECK(mode.clamp(99) == 2);
    CHECK(mode.clamp(-1) == 0);
    const ParamSpec& bits = effectDescs()[effectIndex("crush")].params[0];
    CHECK(bits.clamp(3.6f) == 4);
    CHECK(bits.clamp(NAN) == 8);
}

TEST(dsp_chain_speed_is_realtime) {
    Chain ch(SR, 1024);
    for (int i = 0; i < ch.size(); i++) ch.fx(i).setEnabled(true);
    auto x = sine(440, 200);
    auto t0 = std::chrono::steady_clock::now();
    ch.process(x.data(), (int)x.size() / 2);
    double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    double realtime = (double)x.size() / 2 / SR;
    std::printf("[all 8 fx: %.1f%% of real time] ", 100 * dt / realtime);
    CHECK(dt < realtime * 0.25);
}
