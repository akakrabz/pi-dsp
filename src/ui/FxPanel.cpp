#include <algorithm>
#include <cmath>

#include "core/Rig.h"
#include "core/Util.h"
#include "ui/Panels.h"
#include "ui/Theme.h"

namespace pifx {

void FxPanel::draw(Rig& rig) {
    const float u = ImGui::GetFontSize();
    // ---- master
    sectionHeader("MASTER");
    float m = rig.masterDb();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.62f);
    if (ImGui::SliderFloat("Master", &m, -60, 12, "%.1f dB")) rig.setMasterDb(m);
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) rig.setMasterDb(-6);
    const float gr = rig.engine().limiterDb.load();
    ImGui::SameLine();
    ImGui::TextColored(gr < -0.5f ? theme::kWarn : theme::kMuted, "lim %.1f", gr);
    ImGui::SetItemTooltip("Limiter gain reduction (dB). Always last in the chain.");
    levelMeter("##out", std::max(rig.engine().outPeak[0].load(), rig.engine().outPeak[1].load()),
               ImGui::GetContentRegionAvail().x, 5);

    bool by = rig.bypassAll();
    if (by) ImGui::PushStyleColor(ImGuiCol_Button, hexColor("#5A3A12"));
    if (ImGui::Button(by ? "Bypassed" : "Bypass all")) rig.setBypassAll(!by);
    if (by) ImGui::PopStyleColor();
    ImGui::SameLine();
    if (ImGui::Button("All off")) rig.allOff();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, hexColor("#4A1E22"));
    if (ImGui::Button("Panic")) rig.panic();
    ImGui::PopStyleColor();
    ImGui::SetItemTooltip("Every effect off, every parameter back to default");

    // ---- tempo
    sectionHeader("TEMPO");
    bool flash = rig.tapFlash();
    if (flash) ImGui::PushStyleColor(ImGuiCol_Button, hexColor("#2C524D"));
    if (ImGui::Button("TAP", ImVec2(u * 4, 0))) rig.tap();
    if (flash) ImGui::PopStyleColor();
    ImGui::SameLine();
    float bpm = rig.tempo();
    ImGui::SetNextItemWidth(u * 5);
    if (ImGui::DragFloat("##bpm", &bpm, 0.2f, 30, 300, "%.1f bpm")) rig.setTempo(bpm);
    ImGui::SameLine();
    static const std::pair<double, const char*> divs[] = {{1.0 / 16, "1/16"}, {1.0 / 8, "1/8"}, {1.0 / 4, "1/4"},
                                                          {3.0 / 8, "3/8"},   {1.0 / 2, "1/2"}};
    for (const auto& d : divs) {
        ImGui::SameLine(0, 2);
        if (ImGui::GetContentRegionAvail().x < ImGui::CalcTextSize(d.second).x + 12) ImGui::NewLine();
        if (ImGui::SmallButton(d.second)) rig.delayDiv(d.first);
        ImGui::SetItemTooltip("delay time = %s note at the tempo", d.second);
    }

    // ---- effects
    sectionHeader("EFFECTS");
    const auto& descs = effectDescs();
    for (int i = 0; i < (int)descs.size(); i++) {
        const EffectDesc& d = descs[i];
        const FxState& st = rig.fx()[i];
        ImVec4 col = hexColor(d.color.c_str());
        ImGui::PushID(i);
        if (lampButton("##lamp", st.enabled, col, ImGui::GetFrameHeight() * 0.26f)) rig.setEnabled(i, !st.enabled);
        ImGui::SetItemTooltip("%s on/off", d.name.c_str());
        ImGui::SameLine(0, 4);
        ImGui::PushStyleColor(ImGuiCol_Text, st.enabled ? col : ImGui::GetStyle().Colors[ImGuiCol_Text]);
        bool open = ImGui::TreeNodeEx(d.name.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_FramePadding);
        ImGui::PopStyleColor();
        if (open) {
            ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * 0.62f);
            for (int p = 0; p < (int)d.params.size(); p++) {
                const ParamSpec& s = d.params[p];
                float v = st.values[p];
                ImGui::PushID(p);
                if (s.isEnum()) {
                    for (int c = 0; c < (int)s.choices.size(); c++) {
                        if (c) ImGui::SameLine(0, 1);
                        bool sel = (int)v == c;
                        if (sel) {
                            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(col.x * 0.35f, col.y * 0.35f, col.z * 0.35f, 1));
                            ImGui::PushStyleColor(ImGuiCol_Text, col);
                        }
                        if (ImGui::SmallButton(s.choices[c].c_str())) rig.setParam(i, p, (float)c);
                        if (sel) ImGui::PopStyleColor(2);
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", s.label.c_str());
                } else {
                    std::string fmt = s.step >= 1 ? "%.0f" : (s.max - s.min) >= 100 ? "%.0f" : (s.max - s.min) >= 10 ? "%.1f" : "%.2f";
                    if (!s.unit.empty()) fmt += " " + s.unit;
                    ImGuiSliderFlags fl = s.log ? ImGuiSliderFlags_Logarithmic : 0;
                    if (ImGui::SliderFloat(s.label.c_str(), &v, s.min, s.max, fmt.c_str(), fl)) rig.setParam(i, p, v);
                    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) rig.setParam(i, p, s.def);
                }
                ImGui::PopID();
            }
            ImGui::PopItemWidth();
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    ImGui::TextDisabled("right-click a slider: default   ctrl+click: type");

    // ---- presets
    sectionHeader("PRESETS");
    auto names = rig.presets();
    for (int s = 1; s <= 8; s++) {
        if (s > 1) ImGui::SameLine(0, 3);
        std::string n = format("slot%d", s);
        bool exists = std::find(names.begin(), names.end(), n) != names.end();
        bool cur = rig.currentPreset() == s;
        if (cur) ImGui::PushStyleColor(ImGuiCol_Button, hexColor("#5A4A12"));
        else if (exists) ImGui::PushStyleColor(ImGuiCol_Button, hexColor("#2A2F1A"));
        if (ImGui::Button(format("%d", s).c_str(), ImVec2(u * 1.9f, 0))) {
            if (ImGui::GetIO().KeyShift || ImGui::GetIO().KeyCtrl || !exists) {
                rig.savePreset(n);
                rig.notify(format("saved preset slot %d", s));
            } else {
                rig.loadPreset(n);
            }
        }
        if (cur || exists) ImGui::PopStyleColor();
        ImGui::SetItemTooltip(exists ? "click: load   shift+click: overwrite" : "empty - click to save here");
    }
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - u * 4);
    ImGui::InputTextWithHint("##pname", "preset name", presetName_, sizeof presetName_);
    ImGui::SameLine();
    if (ImGui::Button("Save") && presetName_[0]) {
        rig.savePreset(presetName_);
        presetName_[0] = 0;
    }
    for (const auto& n : names) {
        if (startsWith(n, "slot")) continue;
        ImGui::PushID(n.c_str());
        if (ImGui::SmallButton("load")) rig.loadPreset(n);
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) rig.deletePreset(n);
        ImGui::SameLine();
        ImGui::TextUnformatted(n.c_str());
        ImGui::PopID();
    }
}

}  // namespace pifx
