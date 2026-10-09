#include "core/Analysis.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "core/RingBuffer.h"

namespace pifx {

static constexpr double kPi = 3.14159265358979323846;

// ---------------------------------------------------------------- history
History::History(size_t frames) {
    cap_ = nextPow2(std::max<size_t>(frames, 1024));
    mask_ = cap_ - 1;
    buf_.assign(cap_ * kChannels, 0.0f);
}

void History::append(const float* f, size_t n) {
    if (n > cap_) {               // keep only what fits
        f += (n - cap_) * kChannels;
        end_ += n - cap_;
        n = cap_;
    }
    size_t i = (size_t)(end_ & mask_);
    size_t first = std::min(n, cap_ - i);
    std::memcpy(&buf_[i * kChannels], f, first * kChannels * sizeof(float));
    if (n > first) std::memcpy(&buf_[0], f + first * kChannels, (n - first) * kChannels * sizeof(float));
    end_ += n;
}

void History::clear() {
    std::fill(buf_.begin(), buf_.end(), 0.0f);
    end_ = 0;
}

float History::at(int ch, int64_t abs) const {
    if (abs < (int64_t)begin() || abs >= (int64_t)end_) return 0.0f;
    return buf_[(size_t)(abs & (int64_t)mask_) * kChannels + ch];
}

float History::lerp(int ch, double pos) const {
    double fl = std::floor(pos);
    int64_t i = (int64_t)fl;
    float f = (float)(pos - fl);
    float a = at(ch, i), b = at(ch, i + 1);
    return a + (b - a) * f;
}

void History::copy(int ch, int64_t start, size_t n, float* out) const {
    const int64_t b = (int64_t)begin(), e = (int64_t)end_;
    for (size_t k = 0; k < n; k++) {
        int64_t a = start + (int64_t)k;
        out[k] = (a < b || a >= e) ? 0.0f : buf_[(size_t)(a & (int64_t)mask_) * kChannels + ch];
    }
}

// ---------------------------------------------------------------- sweeper
const char* trigModeName(TrigMode m) {
    switch (m) {
    case TrigMode::Free: return "Free run";
    case TrigMode::Rising: return "Rising";
    case TrigMode::Falling: return "Falling";
    default: return "Both";
    }
}

int Sweeper::collect(const History& h, const TriggerSettings& t, double windowFrames, int sampleRate,
                     std::vector<Sweep>& out, int maxSweeps) {
    const double W = std::max(2.0, windowFrames);
    const double end = (double)h.end(), begin = (double)h.begin();
    int added = 0;
    if (cursor_ < begin) cursor_ = begin;
    // Fell far behind (UI paused, huge holdoff...): skip to recent data.
    const double maxLag = W * std::max(1, maxSweeps) + sampleRate;
    if (end - cursor_ > maxLag) {
        cursor_ = std::max(begin, end - (W + 0.5 * sampleRate));
        armed_ = false;
    }

    if (t.mode == TrigMode::Free) {
        while (cursor_ + W <= end && added < maxSweeps) {
            out.push_back({cursor_, false});
            cursor_ += W;
            added++;
        }
        if (added == maxSweeps && cursor_ + W <= end) cursor_ = end - W;
        lastTrigEnd_ = cursor_;
        return added;
    }

    const double preF = (double)std::clamp(t.pre, 0.0f, 1.0f) * W, postF = W - preF;
    const double hold = std::max(0.0, t.holdoff) * sampleRate;
    const float lvl = t.level, hys = std::max(1e-6f, t.hysteresis);
    const bool up = t.mode == TrigMode::Rising || t.mode == TrigMode::Both;
    const bool dn = t.mode == TrigMode::Falling || t.mode == TrigMode::Both;

    int64_t i = std::max((int64_t)std::ceil(cursor_), (int64_t)begin + 1);
    const int64_t stop = (int64_t)end;
    bool armUp = armed_, armDn = armed_;
    while (i < stop && added < maxSweeps) {
        const float x = h.at(t.channel, i);
        if (x <= lvl - hys) armUp = true;
        if (x >= lvl + hys) armDn = true;
        const float prev = h.at(t.channel, i - 1);
        double pos = -1;
        bool wasUp = armUp, wasDn = armDn;
        if (up && armUp && prev < lvl && x >= lvl) { pos = (double)(i - 1) + (lvl - prev) / (x - prev); armUp = false; }
        else if (dn && armDn && prev > lvl && x <= lvl) { pos = (double)(i - 1) + (prev - lvl) / (prev - x); armDn = false; }
        if (pos >= 0) {
            if (pos + postF > end) {                     // sweep not complete yet: come back later
                armUp = wasUp;
                armDn = wasDn;
                break;
            }
            if (pos - preF >= begin) {
                out.push_back({pos - preF, true});
                added++;
                lastTrigEnd_ = pos + postF;
            }
            // re-arm after the sweep ends (+ holdoff), like a hardware scope
            i = std::max(i + 1, (int64_t)std::ceil(pos + postF + hold));
            armUp = armDn = false;
            continue;
        }
        i++;
    }
    cursor_ = (double)i;
    armed_ = armUp || armDn;

    // Auto: nothing triggered for a while -> show the latest window untriggered.
    if (t.autoRun && added == 0) {
        const double timeout = W + std::max(W, 0.1 * sampleRate);
        if (end - lastTrigEnd_ >= timeout && end - W >= begin) {
            out.push_back({end - W, false});
            lastTrigEnd_ = end;
            added++;
        }
    }
    return added;
}

// ---------------------------------------------------------------- density
void Density::resize(int cols, int rows) {
    if (cols == cols_ && rows == rows_) return;
    cols_ = std::max(1, cols);
    rows_ = std::max(1, rows);
    acc_.assign((size_t)cols_ * rows_, 0.0f);
    sweeps_ = 0;
}

void Density::clear() {
    std::fill(acc_.begin(), acc_.end(), 0.0f);
    sweeps_ = 0;
}

void Density::decay(float f) {
    if (f >= 1.0f) return;
    for (auto& v : acc_) v *= f;
}

inline void Density::splat(double c, double r, float w) {
    // pixel j covers [j, j+1); bilinear around the centre
    const double u = c - 0.5, v = r - 0.5;
    const double fu = std::floor(u), fv = std::floor(v);
    const int i0 = (int)fu, j0 = (int)fv;
    const float a = (float)(u - fu), b = (float)(v - fv);
    const float w00 = (1 - a) * (1 - b) * w, w10 = a * (1 - b) * w, w01 = (1 - a) * b * w, w11 = a * b * w;
    auto put = [&](int i, int j, float ww) {
        if (i >= 0 && i < cols_ && j >= 0 && j < rows_) acc_[(size_t)j * cols_ + i] += ww;
    };
    put(i0, j0, w00);
    put(i0 + 1, j0, w10);
    put(i0, j0 + 1, w01);
    put(i0 + 1, j0 + 1, w11);
}

void Density::segment(double c0, double r0, double c1, double r1, float w) {
    if ((c0 < -1 && c1 < -1) || (c0 > cols_ + 1 && c1 > cols_ + 1)) return;
    if ((r0 < -1 && r1 < -1) || (r0 > rows_ + 1 && r1 > rows_ + 1)) return;
    const double L = std::max(std::fabs(c1 - c0), std::fabs(r1 - r0));
    const int steps = std::max(1, std::min(4096, (int)std::ceil(L * 1.5)));
    const float ws = w / (float)steps;     // equal time per sample interval -> fast edges are faint
    for (int s = 0; s < steps; s++) {
        const double t = (s + 0.5) / steps;
        splat(c0 + (c1 - c0) * t, r0 + (r1 - r0) * t, ws);
    }
}

void Density::addTrace(const float* y, int n, double x0, double dx, float yMin, float yMax, SplatMode mode,
                       float weight) {
    if (n <= 0 || acc_.empty() || yMax <= yMin) return;
    const double rs = rows_ / (double)(yMax - yMin);
    if (mode == SplatMode::Points || n == 1) {
        for (int k = 0; k < n; k++) splat(x0 + k * dx, (y[k] - yMin) * rs, weight);
    } else {
        double pc = x0, pr = (y[0] - yMin) * rs;
        for (int k = 1; k < n; k++) {
            double c = x0 + k * dx, r = (y[k] - yMin) * rs;
            segment(pc, pr, c, r, weight);
            pc = c;
            pr = r;
        }
    }
    sweeps_++;
}

void Density::addXY(const float* x, const float* y, int n, float range, SplatMode mode, float weight) {
    if (n <= 0 || acc_.empty() || range <= 0) return;
    const double cs = cols_ / (2.0 * range), rs = rows_ / (2.0 * range);
    if (mode == SplatMode::Points || n == 1) {
        for (int k = 0; k < n; k++) splat((x[k] + range) * cs, (y[k] + range) * rs, weight);
    } else {
        double pc = (x[0] + range) * cs, pr = (y[0] + range) * rs;
        for (int k = 1; k < n; k++) {
            double c = (x[k] + range) * cs, r = (y[k] + range) * rs;
            segment(pc, pr, c, r, weight);
            pc = c;
            pr = r;
        }
    }
    sweeps_++;
}

void Density::render(NormMode norm, IntensityScale scale, float gamma, std::vector<float>& out) const {
    out.assign((size_t)cols_ * rows_, 0.0f);
    if (acc_.empty()) return;
    std::vector<float> colNorm((size_t)cols_, 1.0f);
    if (norm == NormMode::PerColumn) {
        for (int c = 0; c < cols_; c++) {
            double s = 0;
            for (int r = 0; r < rows_; r++) s += acc_[(size_t)r * cols_ + c];
            colNorm[c] = s > 1e-12 ? (float)(1.0 / s) : 0.0f;
        }
    }
    float m = 0;
    for (int r = 0; r < rows_; r++)
        for (int c = 0; c < cols_; c++) m = std::max(m, acc_[(size_t)r * cols_ + c] * colNorm[c]);
    if (m <= 0) return;
    const float inv = 1.0f / m;
    for (int r = 0; r < rows_; r++) {
        float* dst = &out[(size_t)(rows_ - 1 - r) * cols_];
        const float* src = &acc_[(size_t)r * cols_];
        for (int c = 0; c < cols_; c++) {
            float u = src[c] * colNorm[c] * inv;
            if (scale == IntensityScale::Log) u = std::log10(1.0f + 999.0f * u) / 3.0f;
            else if (scale == IntensityScale::Gamma) u = std::pow(u, gamma);
            dst[c] = std::min(1.0f, std::max(0.0f, u));
        }
    }
}

void Density::marginal(std::vector<float>& out) const {
    out.assign((size_t)rows_, 0.0f);
    double total = 0;
    for (int r = 0; r < rows_; r++) {
        double s = 0;
        for (int c = 0; c < cols_; c++) s += acc_[(size_t)r * cols_ + c];
        out[r] = (float)s;
        total += s;
    }
    if (total > 0)
        for (auto& v : out) v = (float)(v / total);
}

// ---------------------------------------------------------------- measurements
Measurements measure(const float* x, int n, int sampleRate) {
    Measurements m;
    if (n <= 0) return m;
    double sum = 0, ss = 0;
    m.min = m.max = x[0];
    for (int i = 0; i < n; i++) {
        m.min = std::min(m.min, x[i]);
        m.max = std::max(m.max, x[i]);
        sum += x[i];
        ss += (double)x[i] * x[i];
    }
    m.mean = (float)(sum / n);
    m.rms = (float)std::sqrt(ss / n);
    // rising mean-crossings with hysteresis (10% of the peak-to-peak)
    const float hys = 0.1f * (m.max - m.min);
    if (hys <= 1e-6f) return m;
    bool armed = false;
    double first = -1, last = -1;
    int count = 0;
    for (int i = 1; i < n; i++) {
        if (x[i] < m.mean - hys) armed = true;
        if (armed && x[i - 1] < m.mean && x[i] >= m.mean) {
            double pos = (i - 1) + (m.mean - x[i - 1]) / (double)(x[i] - x[i - 1]);
            if (first < 0) first = pos;
            last = pos;
            count++;
            armed = false;
        }
    }
    if (count >= 2 && last > first) m.freq = (float)((count - 1) * sampleRate / (last - first));
    return m;
}

void minMaxBuckets(const float* x, size_t n, int buckets, float* mins, float* maxs) {
    for (int b = 0; b < buckets; b++) {
        size_t a = n * (size_t)b / (size_t)buckets, e = n * (size_t)(b + 1) / (size_t)buckets;
        if (e <= a) e = std::min(n, a + 1);
        float lo = a < n ? x[a] : 0.0f, hi = lo;
        for (size_t i = a; i < e && i < n; i++) {
            lo = std::min(lo, x[i]);
            hi = std::max(hi, x[i]);
        }
        mins[b] = lo;
        maxs[b] = hi;
    }
}

// ---------------------------------------------------------------- FFT
void FFT::init(int n) {
    if (n == n_) return;
    n_ = n;
    int bits = 0;
    while ((1 << bits) < n) bits++;
    rev_.resize((size_t)n);
    for (int i = 0; i < n; i++) {
        int r = 0;
        for (int b = 0; b < bits; b++)
            if (i & (1 << b)) r |= 1 << (bits - 1 - b);
        rev_[i] = r;
    }
    tw_.resize((size_t)n / 2);
    for (int i = 0; i < n / 2; i++) tw_[i] = std::polar(1.0f, (float)(-2 * kPi * i / n));
}

void FFT::forward(std::vector<std::complex<float>>& a) const {
    const int n = n_;
    for (int i = 0; i < n; i++)
        if (i < rev_[i]) std::swap(a[i], a[rev_[i]]);
    for (int len = 2; len <= n; len <<= 1) {
        const int half = len / 2, step = n / len;
        for (int i = 0; i < n; i += len)
            for (int j = 0; j < half; j++) {
                std::complex<float> u = a[i + j], v = a[i + j + half] * tw_[(size_t)j * step];
                a[i + j] = u + v;
                a[i + j + half] = u - v;
            }
    }
}

void Spectrum::configure(int fftSize, int sampleRate) {
    sr_ = sampleRate;
    if (fft_.size() == fftSize && (int)win_.size() == fftSize) return;
    fft_.init(fftSize);
    win_.resize((size_t)fftSize);
    double s = 0;
    for (int i = 0; i < fftSize; i++) {
        win_[i] = (float)(0.5 - 0.5 * std::cos(2 * kPi * i / fftSize));
        s += win_[i];
    }
    winSum_ = (float)s;
    buf_.resize((size_t)fftSize);
    avg_.clear();
}

void Spectrum::compute(const float* x, std::vector<float>& db) {
    const int n = fft_.size();
    for (int i = 0; i < n; i++) buf_[i] = {x[i] * win_[i], 0.0f};
    fft_.forward(buf_);
    db.resize((size_t)bins());
    const float k = 2.0f / winSum_;
    for (int b = 0; b < bins(); b++) db[b] = 20.0f * std::log10(std::max(std::abs(buf_[b]) * k, 1e-7f));
}

void Spectrum::smooth(const std::vector<float>& db, float alpha) {
    if (avg_.size() != db.size()) avg_ = db;
    for (size_t i = 0; i < db.size(); i++) avg_[i] += (db[i] - avg_[i]) * alpha;
}

// ---------------------------------------------------------------- spectrogram
void Spectrogram::configure(int fftSize, int sampleRate, int cols, int rows, float fMin, float fMax) {
    bool same = cols == cols_ && rows == rows_ && sampleRate == sr_ && spec_.fftSize() == fftSize && fMin == fMin_ &&
                fMax == fMax_;
    if (same) return;
    sr_ = sampleRate;
    cols_ = std::max(2, cols);
    rows_ = std::max(2, rows);
    fMin_ = std::max(1.0f, fMin);
    fMax_ = std::max(fMin_ * 2, std::min(fMax, sampleRate / 2.0f));
    spec_.configure(fftSize, sampleRate);
    frame_.assign((size_t)fftSize, 0.0f);
    rowBin0_.resize((size_t)rows_);
    rowBin1_.resize((size_t)rows_);
    rowFrac_.assign((size_t)rows_, -1.0f);
    const float hz = spec_.binHz();
    for (int r = 0; r < rows_; r++) {
        float f0 = rowFreq(r), f1 = fMin_ * std::pow(fMax_ / fMin_, (float)(r + 1) / rows_);
        int b0 = std::max(1, (int)std::floor(f0 / hz)), b1 = std::max(b0, (int)std::floor(f1 / hz));
        rowBin0_[r] = std::min(b0, spec_.bins() - 1);
        rowBin1_[r] = std::min(b1, spec_.bins() - 1);
        if (b1 == b0) rowFrac_[r] = std::min(std::sqrt(f0 * f1) / hz, (float)spec_.bins() - 1.001f);
    }
    clear();
}

float Spectrogram::rowFreq(int row) const { return fMin_ * std::pow(fMax_ / fMin_, (float)row / rows_); }

void Spectrogram::clear() {
    img_.assign((size_t)cols_ * rows_, -140.0f);
    head_ = 0;
    next_ = -1;
}

void Spectrogram::update(const History& h, unsigned mask, double span) {
    if (cols_ <= 0 || spec_.fftSize() <= 0) return;
    hop_ = std::max(32.0, span * sr_ / cols_);
    const double end = (double)h.end();
    const int n = spec_.fftSize();
    if (next_ < 0 || end - next_ > hop_ * cols_ + n) next_ = end - hop_ * (cols_ - 1);   // (re)start
    int nch = 0;
    for (int c = 0; c < History::kChannels; c++) nch += (mask >> c) & 1;
    if (nch == 0) { mask = 1u << 2 | 1u << 3; nch = 2; }
    std::vector<float> tmp((size_t)n);
    int made = 0;
    while (next_ <= end && made < cols_) {
        const int64_t start = (int64_t)next_ - n;
        std::fill(frame_.begin(), frame_.end(), 0.0f);
        for (int c = 0; c < History::kChannels; c++) {
            if (!((mask >> c) & 1)) continue;
            h.copy(c, start, (size_t)n, tmp.data());
            for (int i = 0; i < n; i++) frame_[i] += tmp[i] / nch;
        }
        spec_.compute(frame_.data(), db_);
        float* col = &img_[(size_t)head_ * rows_];
        for (int r = 0; r < rows_; r++) {
            float m = -140.0f;
            if (rowFrac_[r] >= 0) {               // smooth the low end instead of a staircase
                int i = (int)rowFrac_[r];
                float t = rowFrac_[r] - (float)i;
                m = db_[i] * (1 - t) + db_[i + 1] * t;
            } else {
                for (int b = rowBin0_[r]; b <= rowBin1_[r]; b++) m = std::max(m, db_[b]);
            }
            col[r] = m;
        }
        head_ = (head_ + 1) % cols_;
        next_ += hop_;
        made++;
    }
}

void Spectrogram::render(float dbMin, float dbMax, std::vector<float>& out) const {
    out.assign((size_t)cols_ * rows_, 0.0f);
    const float inv = 1.0f / std::max(1.0f, dbMax - dbMin);
    for (int c = 0; c < cols_; c++) {
        const float* col = &img_[(size_t)((head_ + c) % cols_) * rows_];
        for (int r = 0; r < rows_; r++)
            out[(size_t)(rows_ - 1 - r) * cols_ + c] = std::clamp((col[r] - dbMin) * inv, 0.0f, 1.0f);
    }
}

}  // namespace pifx
