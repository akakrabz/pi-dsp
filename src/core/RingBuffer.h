// Lock-free single-producer / single-consumer queues used between the audio
// callbacks and the UI thread. Neither side ever blocks or allocates.
#pragma once
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace pifx {

inline size_t nextPow2(size_t n) {
    size_t p = 1;
    while (p < n) p <<= 1;
    return p;
}

// Interleaved float frames (`channels` floats per frame).
class FrameRing {
public:
    FrameRing() = default;
    FrameRing(size_t frames, int channels) { reset(frames, channels); }

    // Not thread safe: call before the producer/consumer start.
    void reset(size_t frames, int channels) {
        ch_ = channels;
        cap_ = nextPow2(std::max<size_t>(frames, 16));
        mask_ = cap_ - 1;
        buf_.assign(cap_ * ch_, 0.0f);
        w_.store(0);
        r_.store(0);
    }

    int channels() const { return ch_; }
    size_t capacity() const { return cap_; }
    size_t readable() const { return w_.load(std::memory_order_acquire) - r_.load(std::memory_order_acquire); }
    size_t writable() const { return cap_ - readable(); }

    // Producer. Writes up to n frames, returns frames written.
    size_t write(const float* src, size_t n) {
        const size_t w = w_.load(std::memory_order_relaxed);
        const size_t r = r_.load(std::memory_order_acquire);
        n = std::min(n, cap_ - (w - r));
        copyIn(w, src, n);
        w_.store(w + n, std::memory_order_release);
        return n;
    }

    // Consumer. Reads up to n frames, returns frames read.
    size_t read(float* dst, size_t n) {
        const size_t r = r_.load(std::memory_order_relaxed);
        const size_t w = w_.load(std::memory_order_acquire);
        n = std::min(n, w - r);
        copyOut(r, dst, n);
        r_.store(r + n, std::memory_order_release);
        return n;
    }

    // Consumer. Drops up to n frames (latency control).
    size_t skip(size_t n) {
        const size_t r = r_.load(std::memory_order_relaxed);
        const size_t w = w_.load(std::memory_order_acquire);
        n = std::min(n, w - r);
        r_.store(r + n, std::memory_order_release);
        return n;
    }

private:
    void copyIn(size_t pos, const float* src, size_t n) {
        size_t i = pos & mask_;
        size_t first = std::min(n, cap_ - i);
        std::memcpy(&buf_[i * ch_], src, first * ch_ * sizeof(float));
        if (n > first) std::memcpy(&buf_[0], src + first * ch_, (n - first) * ch_ * sizeof(float));
    }
    void copyOut(size_t pos, float* dst, size_t n) const {
        size_t i = pos & mask_;
        size_t first = std::min(n, cap_ - i);
        std::memcpy(dst, &buf_[i * ch_], first * ch_ * sizeof(float));
        if (n > first) std::memcpy(dst + first * ch_, &buf_[0], (n - first) * ch_ * sizeof(float));
    }

    int ch_ = 2;
    size_t cap_ = 0, mask_ = 0;
    std::vector<float> buf_;
    alignas(64) std::atomic<size_t> w_{0};
    alignas(64) std::atomic<size_t> r_{0};
};

// Fixed-size queue of trivially copyable messages.
template <typename T, size_t N>
class SpscQueue {
    static_assert((N & (N - 1)) == 0, "N must be a power of two");
public:
    bool push(const T& v) {
        const size_t w = w_.load(std::memory_order_relaxed);
        if (w - r_.load(std::memory_order_acquire) >= N) return false;
        items_[w & (N - 1)] = v;
        w_.store(w + 1, std::memory_order_release);
        return true;
    }
    bool pop(T& out) {
        const size_t r = r_.load(std::memory_order_relaxed);
        if (r == w_.load(std::memory_order_acquire)) return false;
        out = items_[r & (N - 1)];
        r_.store(r + 1, std::memory_order_release);
        return true;
    }
    bool empty() const { return r_.load() == w_.load(); }

private:
    T items_[N];
    alignas(64) std::atomic<size_t> w_{0};
    alignas(64) std::atomic<size_t> r_{0};
};

}  // namespace pifx
