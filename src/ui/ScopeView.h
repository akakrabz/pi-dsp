// The scope: the main view of pifx.
//
//   Heatmap      persistence / probability density of the triggered waveform
//   Waveform     classic traces (min/max envelope when zoomed out)
//   X-Y          left vs right as a phosphor heatmap (oscilloscope music, phase)
//   Spectrum     FFT per channel, log frequency
//   Spectrogram  rolling time x frequency heatmap
//
// One timebase (window length) drives Heatmap and Waveform; the spectrogram has its
// own time span. All analysis lives in core/Analysis; this file acquires, draws and
// owns the settings.
#pragma once
#include <string>
#include <vector>

#include "core/Analysis.h"
#include "core/PadMap.h"
#include "ui/Gl.h"

namespace pifx {

class Rig;

class ScopeView {
public:
    enum View { Heat, Wave, XY, Spec, Sgram, kViews };

    ScopeView();
    void load(const json& j);
    json save() const;
    void setView(const std::string& name);
    std::string viewName() const;

    void update(Rig& rig, float dt);
    void draw(Rig& rig);

    // Pre-aggregated min/max per block of samples, so long windows draw cheaply.
    struct Blocks {
        static constexpr int B = 64;
        std::vector<float> mn[4], mx[4];
        uint64_t done = 0;           // blocks completed (absolute)
        size_t cap = 0;
        void ingest(const History& h);
    };

private:
    struct Trace {
        std::vector<double> x, lo, hi;
        bool raw = true;
    };
    void acquireHeat(Rig& rig, float dt);
    void acquireXY(Rig& rig, float dt);
    void acquireSpectrum(Rig& rig);
    void buildTrace(const History& h, int ch, double start, double frames, int buckets, Trace& t) const;
    void drawHeat(Rig& rig, float plotH);
    void drawWave(Rig& rig, float plotH);
    void drawXY(Rig& rig, float plotH);
    void drawSpectrum(Rig& rig, float plotH);
    void drawSpectrogram(Rig& rig, float plotH);
    void drawControls(Rig& rig);
    void drawReadout(Rig& rig);
    void timeAxis(double& x0, double& x1, const char*& unit, double& scale) const;
    void triggerTools(double xScale);
    void wheel(bool timeAxis);
    double windowFrames(int sr) const { return window * sr; }

public:
    // ---- settings (persisted)
    View view = Heat;
    double window = 0.005;               // seconds
    TriggerSettings trig;
    bool show[4] = {true, false, true, false};
    int heatChan = 2;                    // tap channel shown in the heatmap
    float range = 1.0f;                  // amplitude +-range
    float persistence = 1.5f;            // seconds (time constant)
    bool infinite = false;
    int splat = (int)SplatMode::Beam;
    int norm = (int)NormMode::PerColumn;
    int scale = (int)IntensityScale::Log;
    float gamma = 0.5f;
    int cmapHeat = -1, cmapXY = -1, cmapSgram = -1;
    int res = 1;                         // 0 low, 1 medium, 2 high
    bool overlay = true, marginal = true;
    int xyPair = 1;                      // 0 = input L/R, 1 = output L/R
    float xyPersistence = 0.4f, xyRange = 1.0f;
    int fftIdx = 3;                      // 1024 << idx
    float specAvg = 0.6f, specFloor = -120;
    bool specShow[4] = {true, false, true, false};
    float sgSpan = 10, sgDbMin = -110, sgDbMax = -10;
    int sgFftIdx = 1;
    unsigned sgMask = 0b1100;
    bool hold = false;

private:
    // ---- state
    Sweeper sweeper_;
    std::vector<Sweep> sweeps_;
    Density heat_, xy_;
    Spectrum spec_[4];
    std::vector<float> specDb_;
    Spectrogram sgram_;
    Blocks blocks_;
    Texture heatTex_, xyTex_, sgTex_;
    ColorLut lutHeat_, lutXY_, lutSg_;
    std::vector<float> img_, marg_, scratch_, scratch2_;
    std::vector<uint32_t> rgba_;
    Sweep last_;
    bool haveSweep_ = false;
    Trace traces_[4];
    Trace overlay_;
    double sweepRate_ = 0, sweepAcc_ = 0, rateClock_ = 0;
    bool lastTriggered_ = false;
    Measurements meas_;
    float xyCorr_ = 0, xyPeak_ = 0;
    double xyCursor_ = -1;
    std::string sig_;
    bool wasHeld_ = false;
    std::string pendingTab_;
    float controlsH_ = 0;
    std::string lastRigView_;
    int sr_ = 48000;
};

std::string formatSeconds(double s);
std::string formatHz(double hz);

}  // namespace pifx
