#include <algorithm>
#include <cmath>

#include "core/Rig.h"
#include "core/Util.h"
#include "implot.h"
#include "ui/Panels.h"
#include "ui/ScopeView.h"
#include "ui/Theme.h"

namespace pifx {

static bool segmentedRow(const char* id, int* v, const char* const* items, int n) {
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

static bool comboStrings(const char* id, int* cur, const std::vector<std::string>& items) {
    bool changed = false;
    const char* preview = *cur >= 0 && *cur < (int)items.size() ? items[*cur].c_str() : "";
    if (ImGui::BeginCombo(id, preview)) {
        for (int i = 0; i < (int)items.size(); i++)
            if (ImGui::Selectable(items[i].c_str(), i == *cur)) {
                *cur = i;
                changed = true;
            }
        ImGui::EndCombo();
    }
    return changed;
}

static std::string mmss(double s) {
    int m = (int)(s / 60);
    return format("%d:%04.1f", m, s - m * 60);
}

void SignalPanel::draw(Rig& rig) {
    drawSource(rig);
    drawSystemAudio(rig);
    drawRouting(rig);
    drawDac(rig);
}

// ---------------------------------------------------------------- source
void SignalPanel::drawSource(Rig& rig) {
    sectionHeader("SOURCE");
    const SourceKind k = rig.sourceKind();
    int kind = k == SourceKind::Tone ? (rig.tone().mode == 1 ? 1 : 0)
             : k == SourceKind::File ? 2
             : k == SourceKind::Capture ? 3 : 4;
    if (rig.loading()) kind = 2;
    static const char* kinds[] = {"Tone", "X-Y", "File", "Input", "Off"};
    if (segmentedRow("kind", &kind, kinds, 5)) {
        ToneParams p = rig.tone();
        std::string err;
        switch (kind) {
        case 0: p.mode = 0; rig.setTone(p); break;
        case 1: p.mode = 1; rig.setTone(p); break;
        case 2: rig.stepFile(0); break;
        case 3:
            if (!rig.useCapture(rig.settings().value("input", std::string()), &err)) rig.notify(err);
            break;
        default: rig.useSilence();
        }
    }
    ImGui::Spacing();
    const float w = ImGui::GetContentRegionAvail().x;
    ImGui::PushItemWidth(w * 0.68f);
    if (kind == 0 || kind == 1) {
        ToneParams p = rig.tone();
        bool ch = false;
        if (kind == 0) {
            ch |= comboStrings("Wave", &p.wave, kWaves);
            if (kWaves[p.wave] == "sweep") {
                ch |= ImGui::SliderFloat("From", &p.sweepLo, 10, 20000, "%.0f Hz", ImGuiSliderFlags_Logarithmic);
                ch |= ImGui::SliderFloat("To", &p.sweepHi, 10, 24000, "%.0f Hz", ImGuiSliderFlags_Logarithmic);
                ch |= ImGui::SliderFloat("Time", &p.sweepSec, 0.5f, 60, "%.1f s");
            } else if (kWaves[p.wave] != "white" && kWaves[p.wave] != "pink") {
                ch |= ImGui::SliderFloat("Freq", &p.freq, 10, 20000, "%.1f Hz", ImGuiSliderFlags_Logarithmic);
            }
        } else {
            ch |= comboStrings("Shape", &p.shape, kShapes);
            const std::string& s = kShapes[p.shape];
            if (s == "lissajous") {
                ch |= ImGui::SliderInt("a", &p.a, 1, 16);
                ch |= ImGui::SliderInt("b", &p.b, 1, 16);
                ch |= ImGui::SliderFloat("Phase", &p.phaseDeg, 0, 360, "%.0f deg");
            } else if (s == "rose" || s == "star" || s == "spiral") {
                ch |= ImGui::SliderInt(s == "rose" ? "Petals" : s == "star" ? "Points" : "Turns", &p.a, 1, 16);
            }
            ch |= ImGui::SliderFloat("Rate", &p.freq, 1, 2000, "%.1f /s", ImGuiSliderFlags_Logarithmic);
            ImGui::SetItemTooltip("How many times per second the shape is traced");
        }
        ch |= ImGui::SliderFloat("Level", &p.level, -60, 0, "%.1f dBFS");
        if (ch) rig.setTone(p);
    } else if (kind == 2) {
        drawFile(rig);
    } else if (kind == 3) {
        drawCapture(rig);
    } else {
        ImGui::TextDisabled("Silence. The scope still runs.");
    }
    ImGui::PopItemWidth();
}

void SignalPanel::drawFile(Rig& rig) {
    const double now = ImGui::GetTime();
    if (now - filesAt_ > 1.0) {
        files_ = rig.mediaFiles();
        filesAt_ = now;
    }
    FileSource* f = rig.file();
    if (rig.loading()) ImGui::TextColored(theme::kWarn, "decoding %s ...", rig.loadingName().c_str());
    const float rowH = ImGui::GetTextLineHeightWithSpacing();
    if (ImGui::BeginChild("##files", ImVec2(-1, rowH * std::min<size_t>(7, std::max<size_t>(2, files_.size())) + 8),
                          ImGuiChildFlags_Borders)) {
        for (const auto& n : files_) {
            bool sel = f && f->clip().name == n;
            if (ImGui::Selectable(n.c_str(), sel)) rig.playFile(n);
        }
        if (files_.empty()) ImGui::TextDisabled("no .wav/.flac/.mp3 in %s", rig.mediaDir().c_str());
    }
    ImGui::EndChild();
    if (ImGui::Button("< Prev")) rig.stepFile(-1);
    ImGui::SameLine();
    if (ImGui::Button("Next >")) rig.stepFile(+1);
    ImGui::SameLine();
    ImGui::TextDisabled("media/");
    ImGui::SetItemTooltip("%s", rig.mediaDir().c_str());
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize("Open").x - ImGui::GetStyle().FramePadding.x * 2 -
                            ImGui::GetStyle().ItemSpacing.x);
    bool enter = ImGui::InputTextWithHint("##path", "/any/path/file.wav", path_, sizeof path_, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if ((ImGui::Button("Open") || enter) && path_[0]) rig.playFile(path_);

    if (!f) return;
    const AudioClip& clip = f->clip();
    if (ovClip_ != &clip) {
        ovClip_ = &clip;
        const int B = 600;
        std::vector<float> mono(clip.frames());
        for (size_t i = 0; i < mono.size(); i++) mono[i] = 0.5f * (clip.data[2 * i] + clip.data[2 * i + 1]);
        std::vector<float> lo(B), hi(B);
        minMaxBuckets(mono.data(), mono.size(), B, lo.data(), hi.data());
        ovX_.resize(B);
        ovLo_.assign(lo.begin(), lo.end());
        ovHi_.assign(hi.begin(), hi.end());
        for (int b = 0; b < B; b++) ovX_[b] = (b + 0.5) * clip.seconds() / B;
    }
    const double pos = (double)f->pos.load() / clip.sampleRate;
    if (ImPlot::BeginPlot("##overview", ImVec2(-1, 74 * ImGui::GetStyle().FontScaleMain),
                          ImPlotFlags_CanvasOnly | ImPlotFlags_NoFrame)) {
        ImPlot::SetupAxes(nullptr, nullptr, ImPlotAxisFlags_NoDecorations | ImPlotAxisFlags_Lock,
                          ImPlotAxisFlags_NoDecorations | ImPlotAxisFlags_Lock);
        ImPlot::SetupAxesLimits(0, clip.seconds(), -1, 1, ImPlotCond_Always);
        ImPlotSpec sp;
        sp.FillColor = theme::kAccent;
        sp.FillAlpha = 0.55f;
        ImPlot::PlotShaded("##w", ovX_.data(), ovLo_.data(), ovHi_.data(), (int)ovX_.size(), sp);
        double ph = pos;
        ImPlot::DragLineX(1, &ph, ImVec4(1, 1, 1, 0.9f), 1.5f, ImPlotDragToolFlags_NoFit);
        if (ph != pos) f->seekTo.store((int64_t)(std::clamp(ph, 0.0, clip.seconds()) * clip.sampleRate));
        if (ImPlot::IsPlotHovered() && ImGui::IsMouseClicked(0))
            f->seekTo.store((int64_t)(std::clamp(ImPlot::GetPlotMousePos().x, 0.0, clip.seconds()) * clip.sampleRate));
        ImPlot::EndPlot();
    }
    bool playing = f->playing.load();
    if (ImGui::Button(playing ? "Pause" : "Play", ImVec2(ImGui::GetFontSize() * 4.5f, 0))) f->playing.store(!playing);
    ImGui::SameLine();
    bool loop = f->loop.load();
    if (ImGui::Checkbox("Loop", &loop)) f->loop.store(loop);
    ImGui::SameLine();
    if (gMonoFont) ImGui::PushFont(gMonoFont, 0.0f);
    ImGui::AlignTextToFramePadding();
    ImGui::Text("%s / %s", mmss(pos).c_str(), mmss(clip.seconds()).c_str());
    if (gMonoFont) ImGui::PopFont();
    float g = f->gainDb.load();
    if (ImGui::SliderFloat("Gain", &g, -40, 12, "%.1f dB")) f->gainDb.store(g);
}

void SignalPanel::drawCapture(Rig& rig) {
    AudioIO& io = rig.io();
    const auto& caps = io.capture();
    std::vector<std::string> labels;
    int cur = -1;
    for (size_t i = 0; i < caps.size(); i++) {
        labels.push_back(caps[i].name + (caps[i].monitor ? "  [monitor]" : ""));
        if (caps[i].key == rig.captureKey()) cur = (int)i;
    }
    if (comboStrings("Device", &cur, labels)) {
        std::string err;
        if (!rig.useCapture(caps[cur].key, &err)) rig.notify(err);
    }
    ImGui::SetItemTooltip("A \"monitor\" input records what an output is playing, without changing anything");
    if (CaptureSource* c = rig.capture()) {
        levelMeter("##in", c->peak.load(), ImGui::GetContentRegionAvail().x, 6);
        float g = c->gainDb.load();
        if (ImGui::SliderFloat("Gain", &g, -40, 40, "%.1f dB")) c->gainDb.store(g);
        ImGui::TextDisabled("%s   under %u  over %u", c->deviceName().c_str(), c->underruns.load(), c->overruns.load());
    }
}

// ---------------------------------------------------------------- system audio
void SignalPanel::drawSystemAudio(Rig& rig) {
    sectionHeader("SYSTEM AUDIO");
    AudioIO& io = rig.io();
    Hijack& hj = rig.hijack();
    bool permanentSink = false;
    for (const auto& d : io.playback()) permanentSink |= d.key == Hijack::kPersistentSink;
    bool on = hj.active();
    ImGui::BeginDisabled(!io.isPulse() && !on);
    if (ImGui::Checkbox("Route all apps through pifx", &on)) {
        std::string err;
        if (on) {
            if (!rig.startHijack(&err)) rig.notify(err);
        } else {
            rig.stopHijack();
        }
    }
    helpMarker("Makes pifx's dummy output the system default and moves every playing app onto it (browser, "
               "video, music...). pifx records it, runs it through the effects and plays it on the outputs "
               "ticked under Routing. Turning it off, or quitting pifx, puts the previous default back.\n\n"
               "With the permanent 'pi-dsp' output (install.sh --system-audio) the dummy output always exists "
               "and shows up in the desktop's sound menu; otherwise pifx creates a temporary one.\n\n"
               "Needs the PipeWire/PulseAudio backend and pactl.");
    bool atStart = rig.hijackOnStart();
    if (ImGui::Checkbox("at startup", &atStart)) rig.setHijackOnStart(atStart);
    ImGui::EndDisabled();
    if (!io.isPulse()) {
        ImGui::TextDisabled("needs Routing > Backend: PipeWire");
    } else if (hj.active()) {
        ImGui::TextColored(theme::kAccent, "apps -> %s -> pifx -> outputs", hj.permanent() ? "pi-dsp" : "pifx-hijack");
        if (!(rig.capture() && Hijack::isVirtualSink(rig.captureKey()))) {
            ImGui::TextColored(theme::kWarn, "apps are muted: the source is not system audio");
            ImGui::SameLine();
            if (ImGui::SmallButton("listen again")) {
                std::string err;
                if (!rig.useCapture(hj.monitor(), &err)) rig.notify(err);
            }
        }
        ImGui::TextDisabled("restores %s when off", hj.previousSink().empty() ? "nothing (no other output)" : hj.previousSink().c_str());
        if (io.outputKeys().empty()) ImGui::TextColored(theme::kWarn, "no output ticked: you will hear nothing");
    } else {
        ImGui::TextDisabled(permanentSink ? "permanent pi-dsp output: ready" : "no permanent pi-dsp output (temporary one is used)");
    }
}

// ---------------------------------------------------------------- routing
void SignalPanel::drawRouting(Rig& rig) {
    sectionHeader("ROUTING");
    AudioIO& io = rig.io();
    static const char* backends[] = {"auto", "pulse", "alsa", "jack"};
    static const char* labels[] = {"Auto", "PipeWire / PulseAudio", "ALSA (direct)", "JACK"};
    int b = (int)backendFromName(rig.settings().value("backend", std::string("auto")));
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.68f);
    if (ImGui::Combo("Backend", &b, labels, 4)) {
        std::string err;
        if (!rig.setBackend(backends[b], &err)) rig.notify(err);
    }
    ImGui::TextDisabled("active: %s", io.activeBackend().c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Rescan")) io.refresh();

    auto keys = io.outputKeys();
    auto views = io.outputs();
    ImGui::Spacing();
    ImGui::TextUnformatted("Play on");
    for (const auto& d : io.playback()) {
        if (Hijack::isVirtualSink(d.key) || Hijack::isVirtualSink(d.name)) continue;   // pifx's own inputs
        auto it = std::find(keys.begin(), keys.end(), d.key);
        bool on = it != keys.end();
        ImGui::PushID(d.key.c_str());
        if (ImGui::Checkbox("##o", &on)) {
            std::vector<std::string> nk = keys;
            if (on) nk.push_back(d.key);
            else nk.erase(std::find(nk.begin(), nk.end(), d.key));
            std::string err;
            if (!rig.setOutputs(nk, &err) && !err.empty()) rig.notify(err);
        }
        ImGui::SameLine();
        std::string label = d.name + (d.isDefault ? "  (default)" : "");
        ImGui::TextUnformatted(label.c_str());
        ImGui::SetItemTooltip("%s", d.key.c_str());
        if (on) {
            size_t idx = (size_t)(it - keys.begin());
            if (idx < views.size()) {
                if (views[idx].clock) {
                    ImGui::SameLine();
                    ImGui::TextColored(theme::kAccent, "clock");
                    ImGui::SetItemTooltip("This output drives the engine; the others follow it");
                }
                ImGui::Indent(ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x);
                levelMeter("##m", views[idx].peak, ImGui::GetContentRegionAvail().x, 4);
                if (!views[idx].error.empty()) ImGui::TextColored(theme::kBad, "%s", views[idx].error.c_str());
                ImGui::Unindent(ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x);
            }
        }
        ImGui::PopID();
    }
    if (io.playback().empty()) ImGui::TextDisabled("no outputs found");
    if (keys.empty()) ImGui::TextColored(theme::kWarn, "no output: scope only");
    if (!rig.hasHat()) {
        float g = rig.outputGainDb();
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.68f);
        if (ImGui::SliderFloat("Level", &g, -60, 0, "%.1f dB")) rig.setOutputGainAll(g);
        ImGui::SameLine();
        bool m = rig.muted();
        if (ImGui::Checkbox("Mute", &m)) rig.setMute(m);
    }
}

// ---------------------------------------------------------------- DAC
void SignalPanel::drawDac(Rig& rig) {
    sectionHeader("DAC HAT");
    Mixer& mx = rig.mixer();
    if (rig.hasHat()) {
        const HatStatus& st = rig.hat();
        ImGui::TextColored(theme::kAccent, "%s", st.card ? st.card->name.c_str() : "PCM5122");
        ImGui::SameLine();
        ImGui::TextDisabled("hw:%d", mx.card());
        if (!dacDragging_) {
            auto v = mx.volumeDb();
            dacVol_ = v && std::isfinite(*v) ? (float)*v : -103.5f;
        }
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.68f);
        bool ch = ImGui::SliderFloat("Volume", &dacVol_, -60, 0, "%.1f dB");
        dacDragging_ = ImGui::IsItemActive();
        // amixer is a subprocess: throttle while dragging, always send the final value
        if ((ch && ImGui::GetTime() - dacSentAt_ > 0.08) || ImGui::IsItemDeactivatedAfterEdit()) {
            rig.setVolumeDb(dacVol_);
            dacSentAt_ = ImGui::GetTime();
        }
        ImGui::SetItemTooltip("PCM5122 digital volume. pifx never goes above 0 dB (digital gain clips).");
        bool m = rig.muted();
        if (ImGui::Checkbox("Mute", &m)) rig.setMute(m);
        ImGui::SameLine();
        int an = mx.analogDb().value_or(0) >= -3 ? 1 : 0;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6);
        if (ImGui::Combo("Analogue", &an, "-6 dB\0" "0 dB\0")) mx.setAnalogDb(an ? 0 : -6);
        auto progs = mx.dspPrograms();
        if (!progs.empty()) {
            int cur = (int)(std::find(progs.begin(), progs.end(), mx.dspProgram()) - progs.begin());
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.68f);
            if (comboStrings("Filter", &cur, progs)) mx.setDspProgram(cur);
            ImGui::SetItemTooltip("PCM5122 interpolation filter");
        }
    } else {
        ImGui::TextDisabled("no PCM5122 detected - any output above works");
    }
    if (ImGui::Button("Diagnostics...")) {
        diagText_ = formatReport(rig.hat());
        showDiag_ = true;
        ImGui::OpenPopup("HAT diagnostics");
    }
    ImGui::SameLine();
    if (ImGui::Button("Rescan HAT")) rig.rescanHat();
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 48, ImGui::GetFontSize() * 26), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("HAT diagnostics", &showDiag_)) {
        if (gMonoFont) ImGui::PushFont(gMonoFont, 0.0f);
        ImGui::InputTextMultiline("##diag", diagText_.data(), diagText_.size() + 1, ImVec2(-1, -ImGui::GetFrameHeightWithSpacing()),
                                  ImGuiInputTextFlags_ReadOnly);
        if (gMonoFont) ImGui::PopFont();
        if (ImGui::Button("Rescan")) {
            rig.rescanHat();
            diagText_ = formatReport(rig.hat());
        }
        ImGui::SameLine();
        if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

}  // namespace pifx
