// Signal analysis for the scope views. Pure CPU, no UI types, unit-testable.
//
//   History      the last ~22 s of the engine tap (4 channels), absolute frame indexing
//   Sweeper      a digital scope's acquisition: finds triggered, non-overlapping sweeps
//   Density      the persistence / probability heatmap: every sweep is splatted into a
//                time x amplitude (or X x Y) grid that decays with a time constant.
//                Points mode bins the samples; Beam mode draws the trace between samples
//                with constant energy per sample interval, so slow parts of the trace
//                glow and fast edges are faint - an analogue phosphor model, and the
//                CPU reference for a future GPU "ray-traced" beam renderer.
//   Spectrum     windowed FFT magnitude in dBFS
//   Spectrogram  rolling time x log-frequency image
#pragma once
#include <complex>
#include <cstdint>
#include <string>
#include <vector>

namespace pifx {

// ---------------------------------------------------------------- history
class History {
public:
    static constexpr int kChannels = 4;
    explicit History(size_t frames = size_t(1) << 20);
    void append(const float* frames4, size_t n);
    void clear();
    uint64_t end() const { return end_; }                         // one past the newest frame
    uint64_t begin() const { return end_ > cap_ ? end_ - cap_ : 0; }
    size_t capacity() const { return cap_; }
    float at(int ch, int64_t abs) const;                          // 0 outside the stored range
    // Linear interpolation at a fractional absolute position.
    float lerp(int ch, double pos) const;
    void copy(int ch, int64_t start, size_t n, float* out) const;

private:
    size_t cap_, mask_;
    std::vector<float> buf_;
    uint64_t end_ = 0;
};

// ---------------------------------------------------------------- trigger / acquisition
enum class TrigMode { Free, Rising, Falling, Both };
const char* trigModeName(TrigMode m);

struct TriggerSettings {
    TrigMode mode = TrigMode::Rising;
    int channel = 0;              // tap channel
    float level = 0.0f;
    float hysteresis = 0.02f;     // signal must move this far to re-arm (noise immunity)
    double holdoff = 0.0;         // seconds after a sweep before re-arming
    float pre = 0.1f;             // fraction of the window before the trigger point
    bool autoRun = true;          // no trigger for a while -> free-run sweeps
};

struct Sweep {
    double start = 0;             // absolute frame (fractional: sub-sample trigger)
    bool triggered = false;
};

class Sweeper {
public:
    void reset(uint64_t from) { cursor_ = (double)from; armed_ = false; lastTrigEnd_ = (double)from; }
    // Appends every sweep completed since the last call. windowFrames >= 2.
    // Returns the number of sweeps added (<= maxSweeps; skips ahead if it falls behind).
    int collect(const History& h, const TriggerSettings& t, double windowFrames, int sampleRate,
                std::vector<Sweep>& out, int maxSweeps);
    double cursor() const { return cursor_; }

private:
    double cursor_ = 0;
    bool armed_ = false;
    double lastTrigEnd_ = 0;
};

// ---------------------------------------------------------------- heatmap
enum class SplatMode { Points, Beam };
enum class NormMode { Global, PerColumn };   // PerColumn = P(amplitude | time)
enum class IntensityScale { Linear, Log, Gamma };

class Density {
public:
    void resize(int cols, int rows);
    void clear();
    void decay(float factor);
    int cols() const { return cols_; }
    int rows() const { return rows_; }
    uint64_t sweeps() const { return sweeps_; }
    const std::vector<float>& data() const { return acc_; }      // [row * cols + col], row 0 = bottom

    // One sweep of a time trace: sample k sits at column x0 + k*dx; amplitude
    // [yMin, yMax] maps to rows [0, rows).
    void addTrace(const float* y, int n, double x0, double dx, float yMin, float yMax, SplatMode mode,
                  float weight = 1.0f);
    // X-Y: point k at (x[k], y[k]), both in [-range, range].
    void addXY(const float* x, const float* y, int n, float range, SplatMode mode, float weight = 1.0f);

    // Display image in [0,1], row 0 = top. out is resized to rows*cols.
    void render(NormMode norm, IntensityScale scale, float gamma, std::vector<float>& out) const;
    // Overall amplitude distribution (sums over columns), normalised to sum 1. size rows, index 0 = bottom.
    void marginal(std::vector<float>& out) const;

private:
    inline void splat(double c, double r, float w);
    void segment(double c0, double r0, double c1, double r1, float w);
    int cols_ = 0, rows_ = 0;
    std::vector<float> acc_;
    uint64_t sweeps_ = 0;
};

// ---------------------------------------------------------------- measurements
struct Measurements {
    float min = 0, max = 0, mean = 0, rms = 0;
    float freq = 0;               // Hz, from interpolated mean-crossings (0 = none found)
};
Measurements measure(const float* x, int n, int sampleRate);

// Min/max per bucket for drawing long windows at pixel resolution.
void minMaxBuckets(const float* x, size_t n, int buckets, float* mins, float* maxs);

// ---------------------------------------------------------------- spectrum
class FFT {
public:
    void init(int n);                                // n = power of two
    int size() const { return n_; }
    void forward(std::vector<std::complex<float>>& a) const;

private:
    int n_ = 0;
    std::vector<int> rev_;
    std::vector<std::complex<float>> tw_;
};

class Spectrum {
public:
    void configure(int fftSize, int sampleRate);
    int fftSize() const { return fft_.size(); }
    int bins() const { return fft_.size() / 2 + 1; }
    float binHz() const { return (float)sr_ / (float)fft_.size(); }
    // Magnitude in dBFS (a full-scale sine reads 0 dB). x has fftSize samples.
    void compute(const float* x, std::vector<float>& db);
    // Exponential average across calls (attack/release in "frames").
    void smooth(const std::vector<float>& db, float alpha);
    const std::vector<float>& smoothed() const { return avg_; }

private:
    FFT fft_;
    int sr_ = 48000;
    std::vector<float> win_;
    float winSum_ = 1;
    std::vector<std::complex<float>> buf_;
    std::vector<float> avg_;
};

class Spectrogram {
public:
    void configure(int fftSize, int sampleRate, int cols, int rows, float fMin, float fMax);
    // Computes the columns that became available since the last call. mono = average of
    // the channels in chanMask (bit per tap channel).
    void update(const History& h, unsigned chanMask, double spanSeconds);
    void clear();
    // Display image in [0,1], row 0 = top (= fMax), oldest column first.
    void render(float dbMin, float dbMax, std::vector<float>& out) const;
    int cols() const { return cols_; }
    int rows() const { return rows_; }
    float rowFreq(int row) const;                    // row 0 = fMin
    double hopFrames() const { return hop_; }

private:
    Spectrum spec_;
    int sr_ = 48000, cols_ = 0, rows_ = 0, head_ = 0;
    float fMin_ = 20, fMax_ = 20000;
    double next_ = -1, hop_ = 512;
    std::vector<float> img_;            // [col * rows + row] in dB
    std::vector<float> frame_, db_;
    std::vector<int> rowBin0_, rowBin1_;
    std::vector<float> rowFrac_;        // fractional bin for rows narrower than one bin (-1 = use max)
};

}  // namespace pifx
