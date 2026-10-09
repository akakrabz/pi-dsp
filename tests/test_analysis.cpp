#include <algorithm>
#include <cmath>
#include <numeric>

#include "check.h"
#include "core/Analysis.h"
#include "core/RingBuffer.h"

using namespace pifx;

static constexpr int SR = 48000;

static void feedSine(History& h, double f, int frames, float amp = 0.5f, double phase0 = 0) {
    std::vector<float> buf((size_t)frames * 4);
    const uint64_t start = h.end();
    for (int i = 0; i < frames; i++) {
        float v = amp * (float)std::sin(2 * M_PI * f * (double)(start + i) / SR + phase0);
        for (int c = 0; c < 4; c++) buf[4 * i + c] = v;
    }
    h.append(buf.data(), (size_t)frames);
}

TEST(ring_wraps_and_counts) {
    FrameRing r(16, 2);
    float in[40], out[40];
    for (int i = 0; i < 40; i++) in[i] = (float)i;
    CHECK(r.write(in, 10) == 10);
    CHECK(r.read(out, 4) == 4);
    CHECK(out[0] == 0 && out[7] == 7);
    CHECK(r.write(in, 20) == 10);           // only 10 free (cap 16, 6 still queued)
    CHECK(r.readable() == 16);
    CHECK(r.skip(6) == 6);
    CHECK(r.read(out, 16) == 10);
    CHECK(out[0] == 0 && out[19] == 19);
}

TEST(history_indexing_and_wrap) {
    History h(1024);
    feedSine(h, 1000, 3000);
    CHECK(h.end() == 3000);
    CHECK(h.begin() == 3000 - h.capacity());
    CHECK(h.at(0, 10) == 0.0f);              // overwritten
    CHECK_NEAR(h.at(2, 2999), 0.5 * std::sin(2 * M_PI * 1000 * 2999 / SR), 1e-6);
    CHECK_NEAR(h.lerp(0, 2998.5), 0.5 * (h.at(0, 2998) + h.at(0, 2999)), 1e-6);
}

TEST(sweeper_triggers_on_rising_edges_with_subsample_phase) {
    History h;
    const double f = 1003.7;                 // not a divisor of the rate: crossings drift through samples
    feedSine(h, f, SR / 2);
    Sweeper sw;
    sw.reset(0);
    TriggerSettings t;
    t.mode = TrigMode::Rising;
    t.level = 0;
    t.pre = 0.25f;
    std::vector<Sweep> out;
    const double W = SR * 0.002;             // 2 ms window
    int n = sw.collect(h, t, W, SR, out, 10000);
    CHECK(n > 100);
    for (const auto& s : out) {
        CHECK(s.triggered);
        double trig = s.start + 0.25 * W;
        // the true rising zero crossing nearest to the trigger point
        double k = std::round(trig * f / SR);
        double exact = k * SR / f;
        CHECK_NEAR(trig, exact, 0.05);
    }
    // sweeps never overlap past the trigger point (re-arm after the sweep)
    for (size_t i = 1; i < out.size(); i++) CHECK(out[i].start + 0.25 * W >= out[i - 1].start + W - 1e-9);
}

TEST(sweeper_free_run_and_auto) {
    History h;
    std::vector<float> zeros((size_t)SR * 4, 0.0f);
    h.append(zeros.data(), SR);
    Sweeper sw;
    sw.reset(0);
    TriggerSettings t;
    std::vector<Sweep> out;
    t.mode = TrigMode::Free;
    CHECK(sw.collect(h, t, 4800, SR, out, 1000) == 10);
    // silence never triggers; auto mode still produces a sweep after the timeout
    Sweeper s2;
    s2.reset(0);
    t.mode = TrigMode::Rising;
    out.clear();
    s2.collect(h, t, 4800, SR, out, 1000);
    CHECK(out.size() == 1 && !out[0].triggered);
    t.autoRun = false;
    Sweeper s3;
    s3.reset(0);
    out.clear();
    s3.collect(h, t, 4800, SR, out, 1000);
    CHECK(out.empty());
}

TEST(density_column_probabilities_of_a_triggered_sine) {
    History h;
    const double f = 997.0;
    feedSine(h, f, SR, 0.8f);
    Sweeper sw;
    sw.reset(0);
    TriggerSettings t;
    t.pre = 0.0f;
    std::vector<Sweep> sweeps;
    const double W = SR / f;                 // exactly one period
    sw.collect(h, t, W, SR, sweeps, 100000);
    Density d;
    d.resize(64, 64);
    std::vector<float> y;
    for (const auto& s : sweeps) {
        int64_t first = (int64_t)std::ceil(s.start);
        int n = (int)std::floor(s.start + W) - (int)first + 1;
        y.resize((size_t)n);
        h.copy(0, first, (size_t)n, y.data());
        d.addTrace(y.data(), n, (first - s.start) / W * 64, 64.0 / W, -1.0f, 1.0f, SplatMode::Beam);
    }
    CHECK(d.sweeps() == sweeps.size());
    std::vector<float> img;
    d.render(NormMode::PerColumn, IntensityScale::Linear, 1.0f, img);
    // a triggered sine is a thin curve: every column's probability mass sits near
    // the sine value at that phase
    for (int c = 2; c < 62; c++) {
        int best = 0;
        for (int r = 1; r < 64; r++)
            if (img[(size_t)r * 64 + c] > img[(size_t)best * 64 + c]) best = r;
        double phase = (c + 0.5) / 64.0;
        double v = 0.8 * std::sin(2 * M_PI * phase);
        int expectRowFromTop = (int)((1.0 - (v + 1.0) / 2.0) * 64);
        CHECK(std::abs(best - expectRowFromTop) <= 2);
    }
    std::vector<float> marg;
    d.marginal(marg);
    CHECK_NEAR(std::accumulate(marg.begin(), marg.end(), 0.0), 1.0, 1e-4);
    // arcsine distribution: a sine spends most time near its peaks (+-0.8 here)
    const size_t nearPeak = (size_t)((0.75 + 1.0) / 2.0 * 64), mid = 32;
    CHECK(marg[nearPeak] > 2 * marg[mid]);
}

TEST(density_beam_dims_fast_edges) {
    Density d;
    d.resize(100, 100);
    // a square-ish wave: long flat tops, instant vertical edges
    std::vector<float> y;
    for (int i = 0; i < 20; i++) y.push_back(i < 10 ? -0.5f : 0.5f);
    d.addTrace(y.data(), (int)y.size(), 10, 4, -1, 1, SplatMode::Beam);   // 4 columns per sample
    auto maxIn = [&](int r0, int r1, int c0, int c1) {
        float m = 0;
        for (int r = r0; r < r1; r++)
            for (int c = c0; c < c1; c++) m = std::max(m, d.data()[(size_t)r * 100 + c]);
        return m;
    };
    float flat = maxIn(20, 30, 15, 40);                  // low flat part (row 25)
    float edge = maxIn(40, 60, 40, 60);                  // middle of the 50-row jump
    CHECK(flat > edge * 5);
    d.decay(0.5f);
    float before = d.data()[(size_t)24 * 100 + 20];
    d.decay(0.5f);
    CHECK(before > 0);
    CHECK_NEAR(d.data()[(size_t)24 * 100 + 20], before * 0.5, 1e-6);
}

TEST(density_xy_circle) {
    Density d;
    d.resize(64, 64);
    std::vector<float> x(1000), y(1000);
    for (int i = 0; i < 1000; i++) { x[i] = 0.5f * std::cos(2 * M_PI * i / 999); y[i] = 0.5f * std::sin(2 * M_PI * i / 999); }
    d.addXY(x.data(), y.data(), 1000, 1.0f, SplatMode::Beam);
    std::vector<float> img;
    d.render(NormMode::Global, IntensityScale::Linear, 1, img);
    CHECK(img[(size_t)32 * 64 + 32] < 0.01f);             // centre empty
    CHECK(img[(size_t)32 * 64 + 48] > 0.2f);              // on the ring (x = +0.5)
}

TEST(measure_sine) {
    std::vector<float> x(SR / 10);
    for (size_t i = 0; i < x.size(); i++) x[i] = 0.25f + 0.5f * (float)std::sin(2 * M_PI * 440.0 * i / SR);
    Measurements m = measure(x.data(), (int)x.size(), SR);
    CHECK_NEAR(m.freq, 440.0, 0.5);
    CHECK_NEAR(m.max, 0.75, 1e-3);
    CHECK_NEAR(m.min, -0.25, 1e-3);
    CHECK_NEAR(m.mean, 0.25, 2e-3);
}

TEST(spectrum_full_scale_sine_reads_0db) {
    Spectrum s;
    s.configure(4096, SR);
    std::vector<float> x(4096), db;
    const double f = 1000.0 * 4096 / SR * SR / 4096;   // bin-centred-ish
    for (int i = 0; i < 4096; i++) x[i] = (float)std::sin(2 * M_PI * f * i / SR);
    s.compute(x.data(), db);
    int peak = (int)(std::max_element(db.begin(), db.end()) - db.begin());
    CHECK_NEAR(peak * s.binHz(), 1000.0, s.binHz());
    CHECK_NEAR(db[peak], 0.0, 1.6);                     // Hann scalloping < 1.5 dB
    CHECK(db[peak / 2] < -60);
}

TEST(spectrogram_tracks_a_tone) {
    History h;
    feedSine(h, 3000, SR, 0.5f);
    Spectrogram sg;
    sg.configure(2048, SR, 32, 64, 20, 20000);
    sg.update(h, 0b1100, 0.5);
    std::vector<float> img;
    sg.render(-100, 0, img);
    // the newest column's brightest row should be at ~3 kHz
    int best = 0;
    for (int r = 0; r < 64; r++)
        if (img[(size_t)r * 32 + 31] > img[(size_t)best * 32 + 31]) best = r;
    float f = sg.rowFreq(63 - best);
    CHECK(f > 2500 && f < 3600);
}
