#include "core/Engine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>

namespace pifx {

const char* tapChannelName(int ch) {
    static const char* names[] = {"In L", "In R", "Out L", "Out R"};
    return ch >= 0 && ch < 4 ? names[ch] : "?";
}

Engine::Engine(int sampleRate, int maxBlock)
    : sr_(sampleRate), maxBlock_(maxBlock), chain_(sampleRate, maxBlock) {
    for (int i = 0; i < 2; i++) {
        outPeak[i] = 0; outRms[i] = 0; inPeak[i] = 0;
    }
    in_.assign((size_t)maxBlock * 2, 0.0f);
    tapBuf_.assign((size_t)maxBlock * kTapChannels, 0.0f);
    tap_.reset((size_t)sampleRate, kTapChannels);   // ~1 s of slack for the UI
    active_ = uiSource_ = new SilenceSource();
}

Engine::~Engine() {
    collect();
    Command c;
    while (cmds_.pop(c))
        if (c.type == Cmd::SetSource && c.src != active_) delete c.src;
    delete active_;
}

bool Engine::post(const Command& c) {
    if (cmds_.push(c)) return true;
    if (!clocked.load()) {            // nobody is draining the queue: apply it here
        flushOffline();
        return cmds_.push(c);
    }
    for (int i = 0; i < 100; i++) {   // a running clock drains it within one period
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (cmds_.push(c)) return true;
    }
    return false;
}

void Engine::setSource(Source* s) {
    Command c;
    c.type = Cmd::SetSource;
    c.src = s;
    uiSource_ = s;
    if (!post(c)) {
        delete s;   // cannot happen in practice
    }
}

void Engine::collect() {
    Source* s;
    while (garbage_.pop(s))
        if (s != uiSource_) delete s;
}

bool Engine::retire(Source* old, int timeoutMs) {
    if (!old) return true;
    if (uiSource_ == old) setSource(new SilenceSource());
    for (int waited = 0; waited <= timeoutMs; waited += 2) {
        if (!clocked.load()) applyCommands();
        Source* s;
        bool found = false;
        while (garbage_.pop(s)) {
            if (s == old) found = true;
            if (s != uiSource_) delete s;
        }
        if (found) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

void Engine::flushOffline() {
    applyCommands();
    collect();
}

void Engine::applyCommands() {
    Command c;
    while (cmds_.pop(c)) {
        switch (c.type) {
        case Cmd::SetSource:
            if (c.src && c.src != active_) {
                Source* old = active_;
                active_ = c.src;
                if (!garbage_.push(old)) { /* leak rather than free on the audio thread */ }
            }
            break;
        case Cmd::ToneParams:
            if (active_ && active_->kind() == SourceKind::Tone) static_cast<ToneSource*>(active_)->setParams(c.tone);
            break;
        case Cmd::FxParam:
            if (c.fx >= 0 && c.fx < chain_.size()) chain_.fx(c.fx).setParam(c.param, c.value);
            break;
        case Cmd::FxEnable:
            if (c.fx >= 0 && c.fx < chain_.size()) chain_.fx(c.fx).setEnabled(c.flag);
            break;
        case Cmd::BypassAll: chain_.setBypassAll(c.flag); break;
        case Cmd::MasterDb: chain_.setMasterDb(c.value); break;
        case Cmd::Panic: chain_.panic(); break;
        }
    }
}

void Engine::render(float* out, int n) {
    auto t0 = std::chrono::steady_clock::now();
    applyCommands();
    for (int off = 0; off < n; off += maxBlock_) renderChunk(out + 2 * off, std::min(maxBlock_, n - off));
    double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    double budget = (double)n / sr_;
    load.store((float)(0.9 * load.load() + 0.1 * (budget > 0 ? dt / budget : 0)));
    limiterDb.store(chain_.limiterDb());
    frames.fetch_add((uint64_t)n);
}

void Engine::renderChunk(float* out, int n) {
    float* in = in_.data();
    active_->pull(in, n);
    for (int i = 0; i < 2 * n; i++)
        if (!std::isfinite(in[i])) { in[i] = 0; errors.fetch_add(1, std::memory_order_relaxed); }
    std::memcpy(out, in, sizeof(float) * 2 * n);
    chain_.process(out, n);

    float pk[2] = {0, 0}, ss[2] = {0, 0}, ipk[2] = {0, 0};
    float* t = tapBuf_.data();
    for (int i = 0; i < n; i++) {
        for (int ch = 0; ch < 2; ch++) {
            float x = in[2 * i + ch], y = out[2 * i + ch];
            t[4 * i + ch] = x;
            t[4 * i + 2 + ch] = y;
            ipk[ch] = std::max(ipk[ch], std::fabs(x));
            pk[ch] = std::max(pk[ch], std::fabs(y));
            ss[ch] += y * y;
        }
    }
    if (tap_.write(t, (size_t)n) < (size_t)n) tapDrops.fetch_add(1, std::memory_order_relaxed);
    for (int ch = 0; ch < 2; ch++) {
        peakHold_[ch] = std::max(pk[ch], peakHold_[ch] * 0.97f);
        outPeak[ch].store(peakHold_[ch], std::memory_order_relaxed);
        outRms[ch].store(std::sqrt(ss[ch] / std::max(1, n)), std::memory_order_relaxed);
        inPeak[ch].store(std::max(ipk[ch], inPeak[ch].load(std::memory_order_relaxed) * 0.97f), std::memory_order_relaxed);
    }
}

}  // namespace pifx
