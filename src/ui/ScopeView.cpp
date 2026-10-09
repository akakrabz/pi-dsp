#include "ui/ScopeView.h"

#include <algorithm>
#include <cmath>

#include "core/Rig.h"
#include "core/Util.h"
#include "implot.h"
#include "ui/Panels.h"
#include "ui/Theme.h"

namespace pifx {

#define gMonoFontScope gMonoFont

static const char* kViewNames[ScopeView::kViews] = {"heat", "wave", "xy", "spectrum", "spectrogram"};
static const char* kViewLabels[ScopeView::kViews] = {"Heatmap", "Waveform", "X-Y", "Spectrum", "Spectrogram"};
static const int kResCols[3] = {256, 512, 1024}, kResRows[3] = {128, 256, 512};
static constexpr double kMinWindow = 1e-4, kMaxWindow = 20.0;

std::string formatSeconds(double s) {
    double a = std::fabs(s);
    if (a < 1e-3) return format("%.0f us", s * 1e6);
    if (a < 1e-2) return format("%.2f ms", s * 1e3);
    if (a < 1) return format("%.1f ms", s * 1e3);
    return format("%.2f s", s);
}

std::string formatHz(double hz) {
    if (hz >= 1000) return format("%.2f kHz", hz / 1000);
    return format("%.1f Hz", hz);
}

// Segmented control: a row of buttons, one selected.
static bool segmented(const char* id, int* v, const char* const* items, int n) {
    bool changed = false;
    ImGui::PushID(id);
    for (int i = 0; i < n; i++) {
        if (i) ImGui::SameLine(0, 1);
        bool sel = *v == i;
        if (sel) {
            ImGui::PushStyleColor(ImGuiCol_Button, hexColor("#24413D"));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hexColor("#2C524D"));
            ImGui::PushStyleColor(ImGuiCol_Text, hexColor("#9FF3E2"));
        }
        if (ImGui::Button(items[i])) {
            *v = i;
            changed = true;
        }
        if (sel) ImGui::PopStyleColor(3);
    }
    ImGui::PopID();
    return changed;
}

// A log-scaled slider for a time/frequency value with a custom readout.
static bool logSlider(const char* id, double* v, double lo, double hi, const std::string& text, float width) {
    float l = (float)std::log10(*v);
    ImGui::SetNextItemWidth(width);
    std::string fmt = text;
    for (size_t p = fmt.find('%'); p != std::string::npos; p = fmt.find('%', p + 2)) fmt.insert(p, "%");
    bool ch = ImGui::SliderFloat(id, &l, (float)std::log10(lo), (float)std::log10(hi), fmt.c_str());
    if (ch) *v = std::pow(10.0, (double)l);
    return ch;
}

// Puts the next widget group on this line if `w` pixels fit, else wraps.
static void flow(float w) {
    ImGui::SameLine();
    if (ImGui::GetContentRegionAvail().x < w) ImGui::NewLine();
}

// A "nice" range (1, 2, 5 x 10^n) that fits the peak with some headroom.
static float niceRange(float peak) {
    peak = std::max(peak * 1.15f, 1e-3f);
    float e = std::pow(10.0f, std::floor(std::log10(peak)));
    for (float m : {1.0f, 2.0f, 2.5f, 5.0f, 10.0f})
        if (m * e >= peak) return m * e;
    return 10 * e;
}

static void channelToggle(const char* label, bool* v, int ch) {
    ImVec4 c = theme::channel(ch);
    ImGui::PushStyleColor(ImGuiCol_CheckMark, c);
    ImGui::PushStyleColor(ImGuiCol_Text, *v ? c : theme::kMuted);
    ImGui::Checkbox(label, v);
    ImGui::PopStyleColor(2);
}

// ---------------------------------------------------------------- blocks
void ScopeView::Blocks::ingest(const History& h) {
    if (cap == 0) {
        cap = h.capacity() / B;
        for (int c = 0; c < 4; c++) {
            mn[c].assign(cap, 0.0f);
            mx[c].assign(cap, 0.0f);
        }
    }
    const uint64_t endBlocks = h.end() / B;
    const uint64_t firstValid = (h.begin() + B - 1) / B;
    if (done < firstValid) done = firstValid;
    if (endBlocks > done + cap) done = endBlocks - cap;
    for (; done < endBlocks; done++) {
        const int64_t s = (int64_t)(done * B);
        const size_t slot = (size_t)(done % cap);
        for (int c = 0; c < 4; c++) {
            float lo = h.at(c, s), hi = lo;
            for (int k = 1; k < B; k++) {
                float v = h.at(c, s + k);
                lo = std::min(lo, v);
                hi = std::max(hi, v);
            }
            mn[c][slot] = lo;
            mx[c][slot] = hi;
        }
    }
}

// ---------------------------------------------------------------- settings
ScopeView::ScopeView() {
    trig.mode = TrigMode::Rising;
    trig.channel = 0;
    trig.level = 0.0f;
    trig.pre = 0.1f;
}

void ScopeView::setView(const std::string& name) {
    for (int i = 0; i < kViews; i++)
        if (name == kViewNames[i] || name == kViewLabels[i]) {
            view = (View)i;
            pendingTab_ = kViewLabels[i];
        }
}

std::string ScopeView::viewName() const { return kViewNames[view]; }

json ScopeView::save() const {
    return {{"view", viewName()},     {"window", window},
            {"trig_mode", (int)trig.mode}, {"trig_channel", trig.channel}, {"trig_level", trig.level},
            {"trig_pre", trig.pre},  {"trig_holdoff", trig.holdoff}, {"trig_auto", trig.autoRun},
            {"show", {show[0], show[1], show[2], show[3]}},
            {"heat_channel", heatChan}, {"range", range}, {"persistence", persistence}, {"infinite", infinite},
            {"splat", splat}, {"norm", norm}, {"scale", scale}, {"gamma", gamma},
            {"cmap_heat", cmapHeat}, {"cmap_xy", cmapXY}, {"cmap_sgram", cmapSgram}, {"res", res},
            {"overlay", overlay}, {"marginal", marginal}, {"xy_pair", xyPair}, {"xy_persistence", xyPersistence},
            {"xy_range", xyRange}, {"fft", fftIdx}, {"spec_avg", specAvg}, {"spec_floor", specFloor},
            {"spec_show", {specShow[0], specShow[1], specShow[2], specShow[3]}},
            {"sg_span", sgSpan}, {"sg_db_min", sgDbMin}, {"sg_db_max", sgDbMax}, {"sg_fft", sgFftIdx}, {"sg_mask", sgMask}};
}

void ScopeView::load(const json& j) {
    if (!j.is_object()) return;
    setView(j.value("view", viewName()));
    pendingTab_.clear();
    window = std::clamp(j.value("window", window), kMinWindow, kMaxWindow);
    trig.mode = (TrigMode)std::clamp(j.value("trig_mode", (int)trig.mode), 0, 3);
    trig.channel = std::clamp(j.value("trig_channel", trig.channel), 0, 3);
    trig.level = j.value("trig_level", trig.level);
    trig.pre = std::clamp(j.value("trig_pre", trig.pre), 0.0f, 1.0f);
    trig.holdoff = j.value("trig_holdoff", trig.holdoff);
    trig.autoRun = j.value("trig_auto", trig.autoRun);
    if (j.contains("show") && j["show"].is_array() && j["show"].size() == 4)
        for (int i = 0; i < 4; i++) show[i] = j["show"][i].get<bool>();
    heatChan = std::clamp(j.value("heat_channel", heatChan), 0, 3);
    range = std::clamp(j.value("range", range), 0.001f, 4.0f);
    persistence = std::clamp(j.value("persistence", persistence), 0.02f, 120.0f);
    infinite = j.value("infinite", infinite);
    splat = std::clamp(j.value("splat", splat), 0, 1);
    norm = std::clamp(j.value("norm", norm), 0, 1);
    scale = std::clamp(j.value("scale", scale), 0, 2);
    gamma = j.value("gamma", gamma);
    cmapHeat = j.value("cmap_heat", cmapHeat);
    cmapXY = j.value("cmap_xy", cmapXY);
    cmapSgram = j.value("cmap_sgram", cmapSgram);
    res = std::clamp(j.value("res", res), 0, 2);
    overlay = j.value("overlay", overlay);
    marginal = j.value("marginal", marginal);
    xyPair = std::clamp(j.value("xy_pair", xyPair), 0, 1);
    xyPersistence = j.value("xy_persistence", xyPersistence);
    xyRange = j.value("xy_range", xyRange);
    fftIdx = std::clamp(j.value("fft", fftIdx), 0, 4);
    specAvg = j.value("spec_avg", specAvg);
    specFloor = j.value("spec_floor", specFloor);
    if (j.contains("spec_show") && j["spec_show"].is_array() && j["spec_show"].size() == 4)
        for (int i = 0; i < 4; i++) specShow[i] = j["spec_show"][i].get<bool>();
    sgSpan = std::clamp(j.value("sg_span", sgSpan), 1.0f, 20.0f);
    sgDbMin = j.value("sg_db_min", sgDbMin);
    sgDbMax = j.value("sg_db_max", sgDbMax);
    sgFftIdx = std::clamp(j.value("sg_fft", sgFftIdx), 0, 2);
    sgMask = j.value("sg_mask", sgMask);
}

// ---------------------------------------------------------------- acquisition
void ScopeView::update(Rig& rig, float dt) {
    History& h = rig.history();
    sr_ = rig.engine().sampleRate();
    blocks_.ingest(h);
    if (rig.view != lastRigView_) {           // a Launchpad "view" pad
        lastRigView_ = rig.view;
        setView(rig.view);
    }
    if (rig.timebaseFactor != 1.0) {
        window = std::clamp(window * rig.timebaseFactor, kMinWindow, kMaxWindow);
        rig.timebaseFactor = 1.0;
    }
    if (hold) {
        wasHeld_ = true;
        return;
    }
    if (wasHeld_) {
        wasHeld_ = false;
        sweeper_.reset(h.end());
        xyCursor_ = -1;
    }
    acquireHeat(rig, dt);
    if (view == XY) acquireXY(rig, dt);
    if (view == Spec) acquireSpectrum(rig);
    sgram_.configure(1024 << sgFftIdx, sr_, 600, 256, 20.0f, sr_ / 2.0f);
    sgram_.update(h, sgMask, sgSpan);
}

void ScopeView::acquireHeat(Rig& rig, float dt) {
    History& h = rig.history();
    const double W = windowFrames(sr_);
    heat_.resize(kResCols[res], kResRows[res]);
    std::string sig = format("%g|%d|%d|%g|%g|%g|%g|%d|%g|%d|%d|%d", window, (int)trig.mode, trig.channel, trig.level,
                             trig.pre, trig.holdoff, trig.hysteresis, heatChan, range, res, splat, (int)trig.autoRun);
    if (sig != sig_) {
        sig_ = sig;
        heat_.clear();
        haveSweep_ = false;
        double back = std::min(2 * W + sr_ * 0.05, (double)sr_ * 2);
        sweeper_.reset((uint64_t)std::max<double>((double)h.begin(), (double)h.end() - back));
    }
    sweeps_.clear();
    int n = sweeper_.collect(h, trig, W, sr_, sweeps_, 4000);
    const int cols = heat_.cols();
    for (const Sweep& s : sweeps_) {
        const int64_t first = (int64_t)std::floor(s.start), last = (int64_t)std::ceil(s.start + W);
        const int cnt = (int)(last - first + 1);
        scratch_.resize((size_t)cnt);
        h.copy(heatChan, first, (size_t)cnt, scratch_.data());
        heat_.addTrace(scratch_.data(), cnt, (first - s.start) / W * cols, cols / W, -range, range, (SplatMode)splat);
    }
    if (n > 0) {
        last_ = sweeps_.back();
        lastTriggered_ = last_.triggered;
        haveSweep_ = true;
        const int cnt = (int)std::min(W, (double)sr_ * 2);
        scratch2_.resize((size_t)std::max(cnt, 1));
        h.copy(heatChan, (int64_t)std::ceil(last_.start), (size_t)cnt, scratch2_.data());
        meas_ = measure(scratch2_.data(), cnt, sr_);
    }
    if (!infinite) heat_.decay(std::exp(-dt / std::max(0.02f, persistence)));
    sweepAcc_ += n;
    rateClock_ += dt;
    if (rateClock_ >= 0.5) {
        sweepRate_ = sweepAcc_ / rateClock_;
        sweepAcc_ = 0;
        rateClock_ = 0;
    }
}

void ScopeView::acquireXY(Rig& rig, float dt) {
    History& h = rig.history();
    xy_.resize(kResRows[res] * 2 > 512 ? 512 : kResRows[res] * 2, kResRows[res] * 2 > 512 ? 512 : kResRows[res] * 2);
    const double end = (double)h.end(), begin = (double)h.begin();
    if (xyCursor_ < 0 || xyCursor_ < begin || end - xyCursor_ > sr_) xyCursor_ = std::max(begin, end - sr_ / 30.0);
    const int64_t start = std::max<int64_t>((int64_t)begin, (int64_t)xyCursor_ - 1);
    const int n = (int)(end - start);
    if (n >= 2) {
        const int cx = xyPair ? 2 : 0, cy = xyPair ? 3 : 1;
        scratch_.resize((size_t)n);
        scratch2_.resize((size_t)n);
        h.copy(cx, start, (size_t)n, scratch_.data());
        h.copy(cy, start, (size_t)n, scratch2_.data());
        xy_.addXY(scratch_.data(), scratch2_.data(), n, xyRange, (SplatMode)splat);
        double sxy = 0, sxx = 0, syy = 0;
        for (int i = 0; i < n; i++) {
            sxy += (double)scratch_[i] * scratch2_[i];
            sxx += (double)scratch_[i] * scratch_[i];
            syy += (double)scratch2_[i] * scratch2_[i];
        }
        float c = (sxx > 1e-12 && syy > 1e-12) ? (float)(sxy / std::sqrt(sxx * syy)) : 0.0f;
        xyCorr_ += (c - xyCorr_) * 0.1f;
        float pk = 0;
        for (int i = 0; i < n; i++) pk = std::max(pk, std::max(std::fabs(scratch_[i]), std::fabs(scratch2_[i])));
        xyPeak_ = std::max(pk, xyPeak_ * 0.98f);
    }
    xyCursor_ = end;
    xy_.decay(std::exp(-dt / std::max(0.02f, xyPersistence)));
}

void ScopeView::acquireSpectrum(Rig& rig) {
    History& h = rig.history();
    const int N = 1024 << fftIdx;
    scratch_.resize((size_t)N);
    for (int c = 0; c < 4; c++) {
        if (!specShow[c]) continue;
        spec_[c].configure(N, sr_);
        h.copy(c, (int64_t)h.end() - N, (size_t)N, scratch_.data());
        spec_[c].compute(scratch_.data(), specDb_);
        spec_[c].smooth(specDb_, 1.0f - specAvg * 0.95f);
    }
}

void ScopeView::buildTrace(const History& h, int ch, double start, double frames, int buckets, Trace& t) const {
    t.x.clear();
    t.lo.clear();
    t.hi.clear();
    buckets = std::max(16, buckets);
    if (frames <= buckets * 2) {
        t.raw = true;
        for (int64_t k = (int64_t)std::ceil(start); k <= (int64_t)std::floor(start + frames); k++) {
            float v = h.at(ch, k);
            t.x.push_back((double)k - start);
            t.lo.push_back(v);
            t.hi.push_back(v);
        }
        return;
    }
    t.raw = false;
    const double per = frames / buckets;
    const uint64_t oldestBlock = blocks_.done > blocks_.cap ? blocks_.done - blocks_.cap : 0;
    for (int b = 0; b < buckets; b++) {
        const double a = start + b * per, e = a + per;
        float lo = 0, hi = 0;
        bool any = false;
        if (per < Blocks::B * 2) {
            for (int64_t k = (int64_t)std::ceil(a); k < (int64_t)std::ceil(e); k++) {
                float v = h.at(ch, k);
                if (!any) { lo = hi = v; any = true; }
                lo = std::min(lo, v);
                hi = std::max(hi, v);
            }
        } else {
            const int64_t bA = (int64_t)std::ceil(a / Blocks::B), bE = (int64_t)std::floor(e / Blocks::B);
            for (int64_t k = bA; k < bE; k++) {
                if (k < (int64_t)oldestBlock || k >= (int64_t)blocks_.done) continue;
                const size_t slot = (size_t)((uint64_t)k % blocks_.cap);
                if (!any) { lo = blocks_.mn[ch][slot]; hi = blocks_.mx[ch][slot]; any = true; }
                lo = std::min(lo, blocks_.mn[ch][slot]);
                hi = std::max(hi, blocks_.mx[ch][slot]);
            }
        }
        t.x.push_back(a + per * 0.5 - start);
        t.lo.push_back(lo);
        t.hi.push_back(hi);
    }
}

// ---------------------------------------------------------------- drawing
void ScopeView::timeAxis(double& x0, double& x1, const char*& unit, double& sc) const {
    if (window < 2e-3) { unit = "us"; sc = 1e6; }
    else if (window < 2.0) { unit = "ms"; sc = 1e3; }
    else { unit = "s"; sc = 1.0; }
    const double pre = trig.mode == TrigMode::Free ? 0.0 : trig.pre;
    x0 = -pre * window * sc;
    x1 = (1.0 - pre) * window * sc;
}

void ScopeView::wheel(bool timeAx) {
    if (!ImPlot::IsPlotHovered()) return;
    const float w = ImGui::GetIO().MouseWheel;
    if (w == 0) return;
    const double f = std::pow(1.25, -w);
    if (ImGui::GetIO().KeyShift || !timeAx) range = std::clamp(range * (float)f, 0.001f, 4.0f);
    else window = std::clamp(window * f, kMinWindow, kMaxWindow);
}

void ScopeView::triggerTools(double xScale) {
    if (trig.mode == TrigMode::Free) return;
    double lvl = trig.level;
    if (ImPlot::DragLineY(1, &lvl, theme::kTrigger, 1.0f, ImPlotDragToolFlags_NoFit))
        trig.level = (float)std::clamp(lvl, -4.0, 4.0);
    ImPlot::TagY(trig.level, theme::kTrigger, "T");
    double zero = 0;
    ImPlot::DragLineX(2, &zero, ImVec4(theme::kTrigger.x, theme::kTrigger.y, theme::kTrigger.z, 0.35f), 1.0f,
                      ImPlotDragToolFlags_NoInputs | ImPlotDragToolFlags_NoFit);
}

void ScopeView::draw(Rig& rig) {
    sr_ = rig.engine().sampleRate();
    if (cmapHeat < 0) cmapHeat = kCmapInferno;
    if (cmapXY < 0) cmapXY = kCmapPhosphor;
    if (cmapSgram < 0) cmapSgram = kCmapInferno;

    if (ImGui::BeginTabBar("##views", ImGuiTabBarFlags_NoTooltip)) {
        for (int i = 0; i < kViews; i++) {
            ImGuiTabItemFlags f = (!pendingTab_.empty() && pendingTab_ == kViewLabels[i]) ? ImGuiTabItemFlags_SetSelected : 0;
            if (ImGui::BeginTabItem(kViewLabels[i], nullptr, f)) {
                if (pendingTab_.empty() && view != i) {
                    view = (View)i;
                    rig.view = kViewNames[i];
                    lastRigView_ = rig.view;
                }
                ImGui::EndTabItem();
            }
        }
        pendingTab_.clear();
        ImGui::EndTabBar();
    }

    const float fh = ImGui::GetFrameHeightWithSpacing();
    const float controlsH = controlsH_ > 0 ? controlsH_ : fh * 2 + ImGui::GetTextLineHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y * 2;
    const float plotH = std::max(120.0f, ImGui::GetContentRegionAvail().y - controlsH);
    switch (view) {
    case Heat: drawHeat(rig, plotH); break;
    case Wave: drawWave(rig, plotH); break;
    case XY: drawXY(rig, plotH); break;
    case Spec: drawSpectrum(rig, plotH); break;
    case Sgram: drawSpectrogram(rig, plotH); break;
    default: break;
    }
    const float y0 = ImGui::GetCursorPosY();
    drawReadout(rig);
    drawControls(rig);
    controlsH_ = ImGui::GetCursorPosY() - y0 + ImGui::GetStyle().ItemSpacing.y;
}

static const ImPlotFlags kPlotFlags = ImPlotFlags_NoLegend | ImPlotFlags_NoMenus | ImPlotFlags_NoBoxSelect;
static const ImPlotAxisFlags kLocked = ImPlotAxisFlags_Lock | ImPlotAxisFlags_NoMenus | ImPlotAxisFlags_NoHighlight;

void ScopeView::drawHeat(Rig& rig, float plotH) {
    if (!hold || !heatTex_.valid()) {
        heat_.render((NormMode)norm, (IntensityScale)scale, gamma, img_);
        if (lutHeat_.cmap != cmapHeat) lutHeat_.build(cmapHeat);
        toRgba(img_, lutHeat_, rgba_);
        if (!rgba_.empty()) heatTex_.upload(rgba_.data(), heat_.cols(), heat_.rows());
        if (overlay && haveSweep_) buildTrace(rig.history(), heatChan, last_.start, windowFrames(sr_), 1600, overlay_);
    }
    double x0, x1, sc;
    const char* unit;
    timeAxis(x0, x1, unit, sc);
    const float margW = marginal ? 120.0f * ImGui::GetStyle().FontScaleMain : 0.0f;
    const float barW = 70.0f * ImGui::GetStyle().FontScaleMain;
    const float plotW = ImGui::GetContentRegionAvail().x - margW - barW - ImGui::GetStyle().ItemSpacing.x * (marginal ? 2 : 1);
    double ylo = -range, yhi = range;
    if (ImPlot::BeginPlot("##heat", ImVec2(plotW, plotH), kPlotFlags)) {
        ImPlot::SetupAxes(unit, nullptr, kLocked, kLocked);
        ImPlot::SetupAxesLimits(x0, x1, ylo, yhi, ImPlotCond_Always);
        if (heatTex_.valid())
            ImPlot::PlotImage("##density", ImTextureRef(heatTex_.id()), ImPlotPoint(x0, ylo), ImPlotPoint(x1, yhi));
        if (overlay && haveSweep_ && !overlay_.x.empty()) {
            std::vector<double> xs(overlay_.x.size());
            const double pre = trig.mode == TrigMode::Free ? 0.0 : trig.pre * window;
            for (size_t i = 0; i < xs.size(); i++) xs[i] = (overlay_.x[i] / sr_ - pre) * sc;
            ImPlotSpec sp;
            sp.LineColor = ImVec4(1, 1, 1, 0.45f);
            sp.FillColor = ImVec4(1, 1, 1, 0.15f);
            sp.LineWeight = 1.0f;
            if (overlay_.raw) ImPlot::PlotLine("##last", xs.data(), overlay_.lo.data(), (int)xs.size(), sp);
            else ImPlot::PlotShaded("##last", xs.data(), overlay_.lo.data(), overlay_.hi.data(), (int)xs.size(), sp);
        }
        triggerTools(sc);
        wheel(true);
        ImPlot::EndPlot();
    }
    ImGui::SameLine();
    ImPlot::ColormapScale(norm == (int)NormMode::PerColumn ? "P(a|t)" : "density", 0, 1, ImVec2(barW, plotH), "%.2f",
                          ImPlotColormapScaleFlags_None, cmapHeat);
    if (marginal) {
        ImGui::SameLine();
        heat_.marginal(marg_);
        if (ImPlot::BeginPlot("##marg", ImVec2(margW, plotH), kPlotFlags)) {
            float mx = 1e-9f;
            for (float v : marg_) mx = std::max(mx, v);
            ImPlot::SetupAxes("P(a)", nullptr, kLocked | ImPlotAxisFlags_NoTickLabels, kLocked);
            ImPlot::SetupAxesLimits(0, mx * 1.1, ylo, yhi, ImPlotCond_Always);
            const int rows = (int)marg_.size();
            std::vector<double> ys((size_t)rows), xs((size_t)rows);
            for (int r = 0; r < rows; r++) {
                ys[r] = ylo + (r + 0.5) * (yhi - ylo) / rows;
                xs[r] = marg_[r];
            }
            ImPlotSpec sp;
            sp.Flags = ImPlotBarsFlags_Horizontal;
            sp.FillColor = theme::channel(heatChan);
            sp.FillAlpha = 0.7f;
            sp.LineColor = ImVec4(0, 0, 0, 0);
            ImPlot::PlotBars("##p", xs.data(), ys.data(), rows, (yhi - ylo) / rows, sp);
            ImPlot::EndPlot();
        }
    }
}

void ScopeView::drawWave(Rig& rig, float plotH) {
    double x0, x1, sc;
    const char* unit;
    timeAxis(x0, x1, unit, sc);
    History& h = rig.history();
    const double W = windowFrames(sr_);
    if (!hold) {
        double start = trig.mode == TrigMode::Free || !haveSweep_ ? (double)h.end() - W : last_.start;
        int buckets = (int)std::min(2048.0f, std::max(64.0f, ImGui::GetContentRegionAvail().x));
        for (int c = 0; c < 4; c++)
            if (show[c]) buildTrace(h, c, start, W, buckets, traces_[c]);
    }
    if (ImPlot::BeginPlot("##wave", ImVec2(-1, plotH), ImPlotFlags_NoMenus | ImPlotFlags_NoBoxSelect)) {
        ImPlot::SetupAxes(unit, nullptr, kLocked, kLocked);
        ImPlot::SetupAxesLimits(x0, x1, -range, range, ImPlotCond_Always);
        ImPlot::SetupLegend(ImPlotLocation_NorthEast);
        const double pre = trig.mode == TrigMode::Free ? 0.0 : trig.pre * window;
        for (int c = 0; c < 4; c++) {
            if (!show[c]) continue;
            Trace& t = traces_[c];
            std::vector<double> xs(t.x.size());
            for (size_t i = 0; i < xs.size(); i++) xs[i] = (t.x[i] / sr_ - pre) * sc;
            ImPlotSpec sp;
            sp.LineColor = theme::channel(c);
            sp.LineWeight = 1.5f;
            if (t.raw) {
                if (t.x.size() <= 64) {
                    sp.Marker = ImPlotMarker_Circle;
                    sp.MarkerSize = 2.5f;
                }
                ImPlot::PlotLine(tapChannelName(c), xs.data(), t.lo.data(), (int)xs.size(), sp);
            } else {
                sp.FillColor = theme::channel(c);
                sp.FillAlpha = 0.45f;
                ImPlot::PlotShaded(tapChannelName(c), xs.data(), t.lo.data(), t.hi.data(), (int)xs.size(), sp);
                sp.LineWeight = 1.0f;
                sp.Flags = ImPlotItemFlags_NoLegend;
                ImPlot::PlotLine("##hi", xs.data(), t.hi.data(), (int)xs.size(), sp);
                ImPlot::PlotLine("##lo", xs.data(), t.lo.data(), (int)xs.size(), sp);
            }
        }
        triggerTools(sc);
        wheel(true);
        ImPlot::EndPlot();
    }
}

void ScopeView::drawXY(Rig& rig, float plotH) {
    if (!hold || !xyTex_.valid()) {
        xy_.render(NormMode::Global, (IntensityScale)scale, gamma, img_);
        if (lutXY_.cmap != cmapXY) lutXY_.build(cmapXY);
        toRgba(img_, lutXY_, rgba_);
        if (!rgba_.empty()) xyTex_.upload(rgba_.data(), xy_.cols(), xy_.rows());
    }
    const float barW = 70.0f * ImGui::GetStyle().FontScaleMain;
    const float side = std::min(ImGui::GetContentRegionAvail().x - barW - ImGui::GetStyle().ItemSpacing.x, plotH);
    const float pad = (ImGui::GetContentRegionAvail().x - side - barW - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    if (pad > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + pad);
    const double r = xyRange;
    if (ImPlot::BeginPlot("##xy", ImVec2(side, plotH), kPlotFlags)) {
        ImPlot::SetupAxes(xyPair ? "Out L" : "In L", xyPair ? "Out R" : "In R", kLocked, kLocked);
        ImPlot::SetupAxesLimits(-r, r, -r, r, ImPlotCond_Always);
        if (xyTex_.valid()) ImPlot::PlotImage("##xyimg", ImTextureRef(xyTex_.id()), ImPlotPoint(-r, -r), ImPlotPoint(r, r));
        if (ImPlot::IsPlotHovered() && ImGui::GetIO().MouseWheel != 0)
            xyRange = std::clamp(xyRange * (float)std::pow(1.25, -ImGui::GetIO().MouseWheel), 0.01f, 4.0f);
        ImPlot::EndPlot();
    }
    ImGui::SameLine();
    ImPlot::ColormapScale("intensity", 0, 1, ImVec2(barW, plotH), "%.2f", ImPlotColormapScaleFlags_None, cmapXY);
}

void ScopeView::drawSpectrum(Rig& rig, float plotH) {
    const int N = 1024 << fftIdx;
    if (ImPlot::BeginPlot("##spec", ImVec2(-1, plotH), ImPlotFlags_NoMenus | ImPlotFlags_NoBoxSelect)) {
        ImPlot::SetupAxes("Hz", "dBFS", ImPlotAxisFlags_NoMenus, ImPlotAxisFlags_NoMenus);
        ImPlot::SetupAxisScale(ImAxis_X1, ImPlotScale_Log10);
        ImPlot::SetupAxesLimits(20, sr_ / 2.0, specFloor, 6, ImPlotCond_Always);
        ImPlot::SetupLegend(ImPlotLocation_NorthEast);
        for (int c = 0; c < 4; c++) {
            if (!specShow[c]) continue;
            const auto& db = spec_[c].smoothed();
            if ((int)db.size() != N / 2 + 1) continue;
            // ~1 point per pixel on the log axis: max of the bins in each bucket
            const int buckets = std::max(64, (int)ImPlot::GetPlotSize().x);
            const double fLo = 20.0, fHi = sr_ / 2.0, binHz = (double)sr_ / N;
            std::vector<double> xs, ys;
            xs.reserve((size_t)buckets);
            ys.reserve((size_t)buckets);
            for (int k = 0; k < buckets; k++) {
                double f0 = fLo * std::pow(fHi / fLo, (double)k / buckets), f1 = fLo * std::pow(fHi / fLo, (double)(k + 1) / buckets);
                int b0 = std::max(1, (int)std::floor(f0 / binHz)), b1 = std::min((int)db.size() - 1, (int)std::floor(f1 / binHz));
                double v;
                if (b1 <= b0) {                      // narrower than a bin: interpolate
                    double fb = std::sqrt(f0 * f1) / binHz;
                    int i = std::min((int)db.size() - 2, std::max(1, (int)fb));
                    double t = std::clamp(fb - i, 0.0, 1.0);
                    v = db[i] * (1 - t) + db[i + 1] * t;
                } else {
                    v = db[b0];
                    for (int b = b0 + 1; b <= b1; b++) v = std::max(v, (double)db[b]);
                }
                xs.push_back(std::sqrt(f0 * f1));
                ys.push_back(std::max(v, (double)specFloor));
            }
            ImPlotSpec sp;
            sp.LineColor = theme::channel(c);
            sp.FillColor = theme::channel(c);
            sp.FillAlpha = 0.12f;
            sp.LineWeight = 1.5f;
            ImPlot::PlotShaded(tapChannelName(c), xs.data(), ys.data(), (int)xs.size(), specFloor, sp);
            sp.Flags = ImPlotItemFlags_NoLegend;
            ImPlot::PlotLine(tapChannelName(c), xs.data(), ys.data(), (int)xs.size(), sp);
            size_t pk = (size_t)(std::max_element(ys.begin(), ys.end()) - ys.begin());
            if (!ys.empty() && ys[pk] > specFloor + 20) ImPlot::TagX(xs[pk], theme::channel(c), "%s", formatHz(xs[pk]).c_str());
        }
        ImPlot::EndPlot();
    }
}

void ScopeView::drawSpectrogram(Rig& rig, float plotH) {
    if (!hold || !sgTex_.valid()) {
        sgram_.render(sgDbMin, sgDbMax, img_);
        if (lutSg_.cmap != cmapSgram) lutSg_.build(cmapSgram);
        toRgba(img_, lutSg_, rgba_);
        if (!rgba_.empty()) sgTex_.upload(rgba_.data(), sgram_.cols(), sgram_.rows());
    }
    const float barW = 70.0f * ImGui::GetStyle().FontScaleMain;
    const float w = ImGui::GetContentRegionAvail().x - barW - ImGui::GetStyle().ItemSpacing.x;
    const double fLo = sgram_.rowFreq(0), fHi = sgram_.rowFreq(sgram_.rows());
    if (ImPlot::BeginPlot("##sgram", ImVec2(w, plotH), kPlotFlags)) {
        ImPlot::SetupAxes("s", "Hz", kLocked, kLocked);
        ImPlot::SetupAxisScale(ImAxis_Y1, ImPlotScale_Log10);
        ImPlot::SetupAxesLimits(-sgSpan, 0, fLo, fHi, ImPlotCond_Always);
        if (sgTex_.valid()) ImPlot::PlotImage("##sg", ImTextureRef(sgTex_.id()), ImPlotPoint(-sgSpan, fLo), ImPlotPoint(0, fHi));
        if (ImPlot::IsPlotHovered() && ImGui::GetIO().MouseWheel != 0)
            sgSpan = std::clamp(sgSpan * (float)std::pow(1.25, -ImGui::GetIO().MouseWheel), 1.0f, 20.0f);
        ImPlot::EndPlot();
    }
    ImGui::SameLine();
    ImPlot::ColormapScale("dBFS", sgDbMin, sgDbMax, ImVec2(barW, plotH), "%.0f", ImPlotColormapScaleFlags_None, cmapSgram);
}

void ScopeView::drawReadout(Rig& rig) {
    ImGui::PushStyleColor(ImGuiCol_Text, theme::kMuted);
    if (view == Heat || view == Wave) {
        const bool trigMode = trig.mode != TrigMode::Free;
        if (gMonoFontScope) ImGui::PushFont(gMonoFontScope, 0.0f);
        ImGui::Text("%s  Vpp %.3f  RMS %.3f (%.1f dBFS)  mean %+.3f  f %s  %.0f sweeps/s", tapChannelName(heatChan),
                    meas_.max - meas_.min, meas_.rms, 20 * std::log10(std::max(meas_.rms, 1e-9f)), meas_.mean,
                    meas_.freq > 0 ? formatHz(meas_.freq).c_str() : "--", sweepRate_);
        if (gMonoFontScope) ImGui::PopFont();
        ImGui::SameLine(0, 12);
        if (!trigMode) ImGui::TextColored(theme::kMuted, "FREE RUN");
        else if (!haveSweep_) ImGui::TextColored(theme::kWarn, "WAITING");
        else ImGui::TextColored(lastTriggered_ ? theme::kAccent : theme::kWarn, "%s", lastTriggered_ ? "TRIGGERED" : "AUTO");
        if (hold) {
            ImGui::SameLine(0, 12);
            ImGui::TextColored(theme::kWarn, "HOLD");
        }
    } else if (view == XY) {
        ImGui::Text("stereo correlation %+.2f   (+1 mono, 0 wide, -1 out of phase)%s", xyCorr_, hold ? "   [HOLD]" : "");
    } else if (view == Spec) {
        const int N = 1024 << fftIdx;
        ImGui::Text("FFT %d   bin %.2f Hz   window %s (Hann)%s", N, (double)sr_ / N, formatSeconds((double)N / sr_).c_str(),
                    hold ? "   [HOLD]" : "");
    } else {
        ImGui::Text("span %s   hop %s   FFT %d   %d x %d%s", formatSeconds(sgSpan).c_str(),
                    formatSeconds(sgram_.hopFrames() / sr_).c_str(), 1024 << sgFftIdx, sgram_.cols(), sgram_.rows(),
                    hold ? "   [HOLD]" : "");
    }
    ImGui::PopStyleColor();
}

void ScopeView::drawControls(Rig& rig) {
    const float u = ImGui::GetFontSize();
    // ---- row 1: run/hold + time base + trigger (or the view's own controls)
    if (hold) ImGui::PushStyleColor(ImGuiCol_Button, hexColor("#5A3A12"));
    if (ImGui::Button(hold ? "  HOLD  " : "  RUN  ")) hold = !hold;
    if (hold) ImGui::PopStyleColor();
    ImGui::SetItemTooltip("Freeze every view (Space)");
    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
        heat_.clear();
        xy_.clear();
        sgram_.clear();
    }
    if (view == Heat || view == Wave) {
        flow(u * 16);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Window");
        ImGui::SameLine();
        logSlider("##win", &window, kMinWindow, kMaxWindow, formatSeconds(window), u * 9);
        ImGui::SetItemTooltip("Time interval shown (mouse wheel over the plot)");
        ImGui::SameLine(0, 2);
        if (ImGui::Button("/2")) window = std::max(kMinWindow, window / 2);
        ImGui::SameLine(0, 2);
        if (ImGui::Button("x2")) window = std::min(kMaxWindow, window * 2);
        flow(u * 12);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Trigger");
        ImGui::SameLine();
        int mode = (int)trig.mode;
        ImGui::SetNextItemWidth(u * 6);
        if (ImGui::Combo("##tmode", &mode, "Free run\0Rising\0Falling\0Both\0")) trig.mode = (TrigMode)mode;
        if (trig.mode != TrigMode::Free) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(u * 4.5f);
            ImGui::Combo("##tch", &trig.channel, "In L\0In R\0Out L\0Out R\0");
            flow(u * 7.5f);
            ImGui::SetNextItemWidth(u * 7);
            ImGui::SliderFloat("##tlvl", &trig.level, -range, range, "level %+.3f");
            ImGui::SetItemTooltip("Drag the yellow line on the plot, too");
            flow(u * 6.5f);
            float prePct = trig.pre * 100;
            ImGui::SetNextItemWidth(u * 6);
            if (ImGui::SliderFloat("##pre", &prePct, 0, 100, "pre %.0f%%")) trig.pre = prePct / 100;
            flow(u * 6);
            ImGui::Checkbox("Auto", &trig.autoRun);
            ImGui::SetItemTooltip("Free-run when nothing triggers for a while");
            ImGui::SameLine();
            if (ImGui::Button("...")) ImGui::OpenPopup("trigmore");
            if (ImGui::BeginPopup("trigmore")) {
                double ho = trig.holdoff;
                ImGui::SetNextItemWidth(u * 10);
                float hoMs = (float)(ho * 1000);
                if (ImGui::SliderFloat("Holdoff", &hoMs, 0, 500, "%.1f ms", ImGuiSliderFlags_Logarithmic)) trig.holdoff = hoMs / 1000;
                ImGui::SetNextItemWidth(u * 10);
                ImGui::SliderFloat("Hysteresis", &trig.hysteresis, 0.001f, 0.5f, "%.3f", ImGuiSliderFlags_Logarithmic);
                ImGui::EndPopup();
            }
        }
    } else if (view == XY) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Pair");
        ImGui::SameLine();
        static const char* pairs[] = {"Input", "Output"};
        segmented("pair", &xyPair, pairs, 2);
        ImGui::SameLine(0, u);
        double p = xyPersistence;
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Persistence");
        ImGui::SameLine();
        if (logSlider("##xyp", &p, 0.02, 30, formatSeconds(p), u * 8)) xyPersistence = (float)p;
        flow(u * 10);
        ImGui::SetNextItemWidth(u * 7);
        ImGui::SliderFloat("##xyr", &xyRange, 0.01f, 2.0f, "range %.2f", ImGuiSliderFlags_Logarithmic);
        ImGui::SameLine(0, 2);
        if (ImGui::Button("Fit##xy")) xyRange = niceRange(xyPeak_);
    } else if (view == Spec) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("FFT");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(u * 5);
        ImGui::Combo("##fft", &fftIdx, "1024\0" "2048\0" "4096\0" "8192\0" "16384\0");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(u * 7);
        ImGui::SliderFloat("##avg", &specAvg, 0, 1, "average %.2f");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(u * 7);
        ImGui::SliderFloat("##floor", &specFloor, -160, -40, "floor %.0f dB");
        for (int c = 0; c < 4; c++) {
            ImGui::SameLine();
            ImGui::PushID(c);
            channelToggle(tapChannelName(c), &specShow[c], c);
            ImGui::PopID();
        }
    } else {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Span");
        ImGui::SameLine();
        double sp = sgSpan;
        if (logSlider("##span", &sp, 1, 20, formatSeconds(sp), u * 8)) sgSpan = (float)sp;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(u * 5);
        ImGui::Combo("##sgfft", &sgFftIdx, "1024\0" "2048\0" "4096\0");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(u * 9);
        ImGui::DragFloatRange2("##db", &sgDbMin, &sgDbMax, 0.5f, -160, 6, "%.0f dB", "%.0f dB");
        for (int c = 0; c < 4; c++) {
            ImGui::SameLine();
            bool on = (sgMask >> c) & 1;
            ImGui::PushID(c + 10);
            channelToggle(tapChannelName(c), &on, c);
            ImGui::PopID();
            sgMask = on ? (sgMask | (1u << c)) : (sgMask & ~(1u << c));
        }
    }

    // ---- row 2: what the view draws
    if (view == Heat) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Channel");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(u * 4.5f);
        ImGui::Combo("##hch", &heatChan, "In L\0In R\0Out L\0Out R\0");
        flow(u * 14);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Persistence");
        ImGui::SameLine();
        ImGui::BeginDisabled(infinite);
        double p = persistence;
        if (logSlider("##pers", &p, 0.02, 120, formatSeconds(p), u * 7)) persistence = (float)p;
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::Checkbox("inf", &infinite);
        ImGui::SetItemTooltip("Infinite persistence: accumulate until Clear");
        flow(u * 6);
        static const char* modes[] = {"Points", "Beam"};
        segmented("splat", &splat, modes, 2);
        ImGui::SetItemTooltip("Beam: draw between samples with constant energy per sample interval\n"
                              "(slow parts glow, fast edges are faint - like an analogue scope)");
        flow(u * 7);
        static const char* norms[] = {"Density", "P(a|t)"};
        segmented("norm", &norm, norms, 2);
        ImGui::SetItemTooltip("P(a|t): every time column is a probability distribution over amplitude");
        flow(u * 11);
        static const char* scales[] = {"Lin", "Log", "Gamma"};
        segmented("scale", &scale, scales, 3);
        if (scale == 2) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(u * 4);
            ImGui::SliderFloat("##g", &gamma, 0.1f, 2.0f, "%.2f");
        }
        flow(u * 11.5f);
        ImPlot::ColormapButton(ImPlot::GetColormapName(cmapHeat), ImVec2(u * 5.5f, 0), cmapHeat);
        if (ImGui::IsItemClicked()) ImGui::OpenPopup("cmap");
        if (ImGui::BeginPopup("cmap")) {
            for (int i = 4; i < ImPlot::GetColormapCount(); i++)
                if (ImPlot::ColormapButton(ImPlot::GetColormapName(i), ImVec2(u * 8, 0), i)) {
                    cmapHeat = i;
                    ImGui::CloseCurrentPopup();
                }
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(u * 6);
        ImGui::Combo("##res", &res, "256x128\0" "512x256\0" "1024x512\0");
        flow(u * 9.5f);
        ImGui::SetNextItemWidth(u * 6);
        ImGui::SliderFloat("##range", &range, 0.01f, 2.0f, "+-%.2f", ImGuiSliderFlags_Logarithmic);
        ImGui::SetItemTooltip("Vertical range (Shift + wheel over the plot)");
        ImGui::SameLine(0, 2);
        if (ImGui::Button("Fit")) range = niceRange(std::max(std::fabs(meas_.min), std::fabs(meas_.max)));
        ImGui::SetItemTooltip("Fit the vertical range to the signal");
        flow(u * 8);
        ImGui::Checkbox("Trace", &overlay);
        ImGui::SetItemTooltip("Overlay the latest sweep");
        ImGui::SameLine();
        ImGui::Checkbox("P(a)", &marginal);
        ImGui::SetItemTooltip("Amplitude distribution over the whole window");
    } else if (view == Wave) {
        for (int c = 0; c < 4; c++) {
            if (c) ImGui::SameLine();
            ImGui::PushID(c + 20);
            channelToggle(tapChannelName(c), &show[c], c);
            ImGui::PopID();
        }
        flow(u * 9.5f);
        ImGui::SetNextItemWidth(u * 6);
        ImGui::SliderFloat("##range2", &range, 0.01f, 2.0f, "+-%.2f", ImGuiSliderFlags_Logarithmic);
        ImGui::SameLine(0, 2);
        if (ImGui::Button("Fit")) range = niceRange(std::max(std::fabs(meas_.min), std::fabs(meas_.max)));
        flow(u * 16);
        ImGui::TextDisabled("wheel: window   shift+wheel: range");
    } else if (view == XY) {
        static const char* modes[] = {"Points", "Beam"};
        segmented("splat2", &splat, modes, 2);
        ImGui::SameLine(0, u);
        static const char* scales[] = {"Lin", "Log", "Gamma"};
        segmented("scale2", &scale, scales, 3);
        ImGui::SameLine(0, u);
        ImPlot::ColormapButton(ImPlot::GetColormapName(cmapXY), ImVec2(u * 5.5f, 0), cmapXY);
        if (ImGui::IsItemClicked()) ImGui::OpenPopup("cmapxy");
        if (ImGui::BeginPopup("cmapxy")) {
            for (int i = 4; i < ImPlot::GetColormapCount(); i++)
                if (ImPlot::ColormapButton(ImPlot::GetColormapName(i), ImVec2(u * 8, 0), i)) {
                    cmapXY = i;
                    ImGui::CloseCurrentPopup();
                }
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("try Source > X-Y shape");
    } else if (view == Sgram) {
        ImPlot::ColormapButton(ImPlot::GetColormapName(cmapSgram), ImVec2(u * 5.5f, 0), cmapSgram);
        if (ImGui::IsItemClicked()) ImGui::OpenPopup("cmapsg");
        if (ImGui::BeginPopup("cmapsg")) {
            for (int i = 4; i < ImPlot::GetColormapCount(); i++)
                if (ImPlot::ColormapButton(ImPlot::GetColormapName(i), ImVec2(u * 8, 0), i)) {
                    cmapSgram = i;
                    ImGui::CloseCurrentPopup();
                }
            ImGui::EndPopup();
        }
    } else {
        ImGui::TextDisabled("compare In vs Out to see what the effects do to the spectrum");
    }
}

}  // namespace pifx
