#include "core/Devices.h"

#include <chrono>
#include <cstring>

#include "core/Util.h"
#include "miniaudio.h"

namespace pifx {

const char* backendName(Backend b) {
    switch (b) {
    case Backend::Pulse: return "PipeWire/PulseAudio";
    case Backend::Alsa: return "ALSA";
    case Backend::Jack: return "JACK";
    default: return "Auto";
    }
}

Backend backendFromName(const std::string& s) {
    std::string l = toLower(s);
    if (l == "pulse" || l == "pulseaudio" || l == "pipewire" || startsWith(l, "pipewire/")) return Backend::Pulse;
    if (l == "alsa") return Backend::Alsa;
    if (l == "jack") return Backend::Jack;
    return Backend::Auto;
}

struct AudioIO::Out {
    AudioIO* io = nullptr;
    std::string key, name;
    ma_device dev{};
    bool inited = false;
    bool clock = false;
    FrameRing ring;
    std::atomic<float> gain{1.0f};
    float gainDb = 0;
    std::atomic<bool> mute{false};
    std::atomic<float> peak{0};
    std::atomic<uint32_t> underruns{0};
    std::atomic<bool> lost{false}, stopping{false}, primed{false};
    std::string error;
};

namespace {

void applyGainAndMeter(AudioIO::Out* o, float* out, int frames) {
    const float g = o->mute.load(std::memory_order_relaxed) ? 0.0f : o->gain.load(std::memory_order_relaxed);
    float pk = 0;
    for (int i = 0; i < 2 * frames; i++) {
        out[i] *= g;
        pk = std::max(pk, std::fabs(out[i]));
    }
    o->peak.store(std::max(pk, o->peak.load(std::memory_order_relaxed) * 0.95f), std::memory_order_relaxed);
}

void clockCb(ma_device* dev, void* out, const void*, ma_uint32 frames) {
    auto* o = (AudioIO::Out*)dev->pUserData;
    o->io->clockCallback(o, (float*)out, (int)frames);
}

void followerCb(ma_device* dev, void* outv, const void*, ma_uint32 framesU) {
    auto* o = (AudioIO::Out*)dev->pUserData;
    float* out = (float*)outv;
    const int frames = (int)framesU;
    const size_t block = (size_t)o->io->blockFrames();
    const size_t target = 3 * std::max(block, (size_t)frames);
    size_t avail = o->ring.readable();
    if (!o->primed.load(std::memory_order_relaxed)) {
        if (avail < target) {
            std::memset(out, 0, sizeof(float) * 2 * frames);
            return;
        }
        o->primed.store(true, std::memory_order_relaxed);
    }
    if (avail < (size_t)frames) {
        size_t got = o->ring.read(out, avail);
        std::memset(out + 2 * got, 0, sizeof(float) * 2 * (frames - got));
        o->underruns.fetch_add(1, std::memory_order_relaxed);
        o->primed.store(false, std::memory_order_relaxed);
    } else {
        o->ring.read(out, (size_t)frames);
        size_t left = avail - (size_t)frames;
        if (left > target + 4 * block) o->ring.skip(left - target);   // clock drift: catch up
    }
    applyGainAndMeter(o, out, frames);
}

void notifyCb(const ma_device_notification* n) {
    auto* o = (AudioIO::Out*)n->pDevice->pUserData;
    if (n->type == ma_device_notification_type_stopped && !o->stopping.load()) o->lost.store(true);
}

std::string keyFor(ma_backend b, const ma_device_info& d) {
    if (b == ma_backend_pulseaudio && d.id.pulse[0]) return d.id.pulse;
    if (b == ma_backend_alsa && d.id.alsa[0]) return d.id.alsa;
    return d.name;
}

}  // namespace

void AudioIO::clockCallback(Out* o, float* out, int frames) {
    engine_.render(out, frames);
    for (auto& f : outs_) {
        if (f.get() == o || !f->inited) continue;
        if (f->ring.write(out, (size_t)frames) < (size_t)frames) f->underruns.fetch_add(1, std::memory_order_relaxed);
    }
    applyGainAndMeter(o, out, frames);
}

AudioIO::AudioIO(Engine& engine, int blockFrames) : engine_(engine), block_(blockFrames) {}

AudioIO::~AudioIO() { shutdown(); }

void AudioIO::shutdown() {
    stopAll();
    if (ctx_) {
        ma_context_uninit(ctx_);
        delete ctx_;
        ctx_ = nullptr;
    }
    playback_.clear();
    capture_.clear();
}

bool AudioIO::init(Backend b, std::string* err) {
    shutdown();
    requested_ = b;
    ma_backend list[3];
    ma_uint32 count = 0;
    switch (b) {
    case Backend::Pulse: list[count++] = ma_backend_pulseaudio; break;
    case Backend::Alsa: list[count++] = ma_backend_alsa; break;
    case Backend::Jack: list[count++] = ma_backend_jack; break;
    default:
        list[count++] = ma_backend_pulseaudio;   // PipeWire answers as PulseAudio
        list[count++] = ma_backend_alsa;
        list[count++] = ma_backend_jack;
    }
    ma_context_config cfg = ma_context_config_init();
    cfg.pulse.pApplicationName = "pifx";
    cfg.jack.pClientName = "pifx";
    cfg.alsa.useVerboseDeviceEnumeration = MA_FALSE;
    ctx_ = new ma_context;
    ma_result r = ma_context_init(list, count, &cfg, ctx_);
    if (r != MA_SUCCESS) {
        delete ctx_;
        ctx_ = nullptr;
        if (err) *err = std::string("no audio backend available (") + backendName(b) + "): " + ma_result_description(r);
        startTimer();
        return false;
    }
    refresh();
    return true;
}

std::string AudioIO::activeBackend() const {
    if (!ctx_) return "none";
    switch (ctx_->backend) {
    case ma_backend_pulseaudio: return "PipeWire/PulseAudio";
    case ma_backend_alsa: return "ALSA";
    case ma_backend_jack: return "JACK";
    default: return ma_get_backend_name(ctx_->backend);
    }
}

bool AudioIO::isPulse() const { return ctx_ && ctx_->backend == ma_backend_pulseaudio; }

void AudioIO::refresh() {
    playback_.clear();
    capture_.clear();
    if (!ctx_) return;
    ma_device_info *pb = nullptr, *cp = nullptr;
    ma_uint32 npb = 0, ncp = 0;
    if (ma_context_get_devices(ctx_, &pb, &npb, &cp, &ncp) != MA_SUCCESS) return;
    auto add = [&](std::vector<DeviceInfo>& dst, const ma_device_info& d, bool cap) {
        // ALSA's "null" plugin discards audio as fast as it can: useless as a clock.
        if (ctx_->backend == ma_backend_alsa && std::string(d.id.alsa) == "null") return;
        DeviceInfo i;
        i.name = d.name;
        i.key = keyFor(ctx_->backend, d);
        i.isDefault = d.isDefault != 0;
        i.monitor = cap && (endsWith(i.key, ".monitor") || startsWith(i.name, "Monitor of"));
        i.id.resize(sizeof(ma_device_id));
        std::memcpy(i.id.data(), &d.id, sizeof(ma_device_id));
        dst.push_back(std::move(i));
    };
    for (ma_uint32 k = 0; k < npb; k++) add(playback_, pb[k], false);
    for (ma_uint32 k = 0; k < ncp; k++) add(capture_, cp[k], true);
}

static const DeviceInfo* findIn(const std::vector<DeviceInfo>& v, const std::string& s) {
    if (s.empty()) return nullptr;
    for (const auto& d : v) if (d.key == s) return &d;
    for (const auto& d : v) if (d.name == s) return &d;
    for (const auto& d : v) if (contains(d.name, s) || contains(d.key, s)) return &d;
    return nullptr;
}
const DeviceInfo* AudioIO::findPlayback(const std::string& s) const { return findIn(playback_, s); }
const DeviceInfo* AudioIO::findCapture(const std::string& s) const { return findIn(capture_, s); }

void AudioIO::stopAll() {
    stopTimer();
    std::lock_guard<std::mutex> lk(mu_);
    // Clock first (it iterates outs_ and feeds the followers), then the followers.
    for (int pass = 0; pass < 2; pass++) {
        for (auto& o : outs_) {
            if (!o->inited || o->clock != (pass == 0)) continue;
            o->stopping.store(true);
            ma_device_uninit(&o->dev);
            o->inited = false;
        }
    }
    outs_.clear();
    engine_.clocked.store(false);
}

bool AudioIO::setOutputs(const std::vector<std::string>& keys, std::string* err) {
    stopAll();
    std::string errors;
    std::vector<std::unique_ptr<Out>> outs;
    for (const auto& k : keys) {
        const DeviceInfo* d = findPlayback(k);
        if (!d || !ctx_) {
            errors += "output not found: " + k + "\n";
            continue;
        }
        bool dup = false;
        for (auto& o : outs) dup |= o->key == d->key;
        if (dup) continue;
        auto o = std::make_unique<Out>();
        o->io = this;
        o->key = d->key;
        o->name = d->name;
        o->clock = outs.empty();
        o->ring.reset((size_t)std::max(block_ * 32, 8192), 2);
        ma_device_id id;
        std::memcpy(&id, d->id.data(), sizeof id);
        ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
        cfg.playback.pDeviceID = &id;
        cfg.playback.format = ma_format_f32;
        cfg.playback.channels = 2;
        cfg.sampleRate = (ma_uint32)engine_.sampleRate();
        cfg.periodSizeInFrames = (ma_uint32)block_;
        cfg.performanceProfile = ma_performance_profile_low_latency;
        cfg.dataCallback = o->clock ? clockCb : followerCb;
        cfg.notificationCallback = notifyCb;
        cfg.pUserData = o.get();
        cfg.noClip = MA_TRUE;
        ma_result r = ma_device_init(ctx_, &cfg, &o->dev);
        if (r != MA_SUCCESS) {
            errors += d->name + ": " + ma_result_description(r) + "\n";
            continue;
        }
        o->inited = true;
        outs.push_back(std::move(o));
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        outs_ = std::move(outs);
    }
    // Followers first (they output silence until primed), then the clock.
    for (int pass = 0; pass < 2; pass++) {
        for (auto& o : outs_) {
            if (o->clock != (pass == 1)) continue;
            ma_result r = ma_device_start(&o->dev);
            if (r != MA_SUCCESS) {
                o->error = ma_result_description(r);
                errors += o->name + ": " + o->error + "\n";
            }
        }
    }
    bool clockOk = !outs_.empty() && outs_[0]->clock && ma_device_get_state(&outs_[0]->dev) == ma_device_state_started;
    if (clockOk) engine_.clocked.store(true);
    else {
        // Nothing to clock the engine (or the clock failed): drop everything, use the timer.
        if (!outs_.empty()) {
            stopAll();
            if (!keys.empty()) errors += "no output could be started; scope-only mode\n";
        }
        startTimer();
    }
    if (err) *err = trim(errors);
    return errors.empty();
}

std::vector<std::string> AudioIO::outputKeys() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<std::string> k;
    for (auto& o : outs_) k.push_back(o->key);
    return k;
}

std::vector<OutputView> AudioIO::outputs() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<OutputView> v;
    for (auto& o : outs_) {
        OutputView ov;
        ov.key = o->key;
        ov.name = o->name;
        ov.clock = o->clock;
        ov.ok = o->inited && !o->lost.load() && ma_device_get_state(&o->dev) == ma_device_state_started;
        ov.gainDb = o->gainDb;
        ov.mute = o->mute.load();
        ov.peak = o->peak.load();
        ov.underruns = o->underruns.load();
        ov.error = o->lost.load() ? "device stopped (unplugged?)" : o->error;
        v.push_back(ov);
    }
    return v;
}

void AudioIO::setOutputGain(size_t i, float db) {
    std::lock_guard<std::mutex> lk(mu_);
    if (i >= outs_.size()) return;
    outs_[i]->gainDb = db;
    outs_[i]->gain.store(db <= -60.0f ? 0.0f : dbToLin(db));
}

void AudioIO::setOutputMute(size_t i, bool mute) {
    std::lock_guard<std::mutex> lk(mu_);
    if (i < outs_.size()) outs_[i]->mute.store(mute);
}

std::string AudioIO::clockName() const {
    std::lock_guard<std::mutex> lk(mu_);
    if (!outs_.empty() && outs_[0]->clock) return outs_[0]->name;
    return "timer (no output)";
}

std::string AudioIO::poll() {
    std::vector<std::string> keep;
    std::string lostName;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (outs_.empty() || !outs_[0]->lost.load()) return "";
        lostName = outs_[0]->name;
        for (size_t i = 1; i < outs_.size(); i++)
            if (!outs_[i]->lost.load()) keep.push_back(outs_[i]->key);
    }
    refresh();
    std::string err;
    setOutputs(keep, &err);
    return "output lost: " + lostName + (keep.empty() ? " - scope-only mode" : " - re-routed");
}

void AudioIO::startTimer() {
    stopTimer();
    timerStop_.store(false);
    engine_.clocked.store(true);
    timer_ = std::thread([this] {
        std::vector<float> buf((size_t)block_ * 2);
        const auto period = std::chrono::duration<double>((double)block_ / engine_.sampleRate());
        auto next = std::chrono::steady_clock::now();
        while (!timerStop_.load()) {
            engine_.render(buf.data(), block_);
            next += std::chrono::duration_cast<std::chrono::steady_clock::duration>(period);
            auto now = std::chrono::steady_clock::now();
            if (next > now) std::this_thread::sleep_until(next);
            else if (now - next > std::chrono::milliseconds(100)) next = now;   // fell far behind: resync
        }
    });
}

void AudioIO::stopTimer() {
    if (timer_.joinable()) {
        timerStop_.store(true);
        timer_.join();
    }
    engine_.clocked.store(false);
}

std::unique_ptr<CaptureSource> AudioIO::openCapture(const std::string& key, std::string* err) {
    if (!ctx_) {
        if (err) *err = "no audio backend";
        return nullptr;
    }
    const DeviceInfo* d = key.empty() ? nullptr : findCapture(key);
    if (!key.empty() && !d) {
        if (err) *err = "input not found: " + key;
        return nullptr;
    }
    auto src = std::make_unique<CaptureSource>(engine_.sampleRate(), block_);
    ma_device_id id;
    if (d) std::memcpy(&id, d->id.data(), sizeof id);
    if (!src->open(ctx_, d ? &id : nullptr, d ? d->name : "default input", err)) return nullptr;
    return src;
}

}  // namespace pifx
