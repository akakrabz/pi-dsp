#include <algorithm>

#include "core/Rig.h"
#include "ui/Panels.h"
#include "ui/Theme.h"

namespace pifx {

void PadPanel::draw(Rig& rig) {
    if (LaunchpadManager* lp = rig.launchpad()) {
        auto st = lp->status();
        if (st.connected) ImGui::TextColored(theme::kAccent, "%s", st.model.c_str());
        else ImGui::TextDisabled("%s", st.available ? "no Launchpad found - plug one in" : st.error.c_str());
        if (st.connected) ImGui::SetItemTooltip("%s", st.name.c_str());
    } else {
        ImGui::TextDisabled("Launchpad disabled (--no-launchpad)");
    }
    const auto colors = rig.padColors();
    const float gap = 3;
    const float footer = ImGui::GetTextLineHeightWithSpacing() + gap * 4;
    const float cell = std::max(10.0f, std::min({44.0f * ImGui::GetStyle().FontScaleMain,
                                                 (ImGui::GetContentRegionAvail().x - gap * 11) / 9.0f,
                                                 (ImGui::GetContentRegionAvail().y - footer - gap * 11) / 9.0f}));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    for (int row = 0; row < 9; row++) {
        const int y = 8 - row;   // top row first
        for (int x = 0; x < 9; x++) {
            if (x == 8 && y == 8) continue;
            ImVec2 p(origin.x + x * (cell + gap) + (x == 8 ? gap * 2 : 0), origin.y + row * (cell + gap) + (y < 8 ? gap * 2 : 0));
            ImGui::SetCursorScreenPos(p);
            ImGui::PushID(y * 9 + x);
            ImGui::InvisibleButton("##pad", ImVec2(cell, cell));
            if (ImGui::IsItemActivated()) rig.padEvent(x, y, true);
            if (ImGui::IsItemDeactivated()) rig.padEvent(x, y, false);
            const unsigned rgb = paletteRgb(colors[y * 9 + x]);
            ImU32 fill = IM_COL32((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255, 255);
            const bool round = x == 8 || y == 8;
            const bool held = ImGui::IsItemActive() || rig.padHeld(x, y);
            ImVec2 a = p, b(p.x + cell, p.y + cell);
            if (round) dl->AddCircleFilled(ImVec2(p.x + cell / 2, p.y + cell / 2), cell * 0.38f, fill, 20);
            else dl->AddRectFilled(a, b, fill, 4);
            if (held) {
                if (round) dl->AddCircle(ImVec2(p.x + cell / 2, p.y + cell / 2), cell * 0.38f + 2, IM_COL32_WHITE, 20, 2);
                else dl->AddRect(a, b, IM_COL32_WHITE, 4.0f, ImDrawFlags_None, 2.0f);
            } else if (ImGui::IsItemHovered()) {
                if (!round) dl->AddRect(a, b, IM_COL32(255, 255, 255, 90), 4);
            }
            std::string label = rig.padLabel(x, y);
            if (!label.empty()) ImGui::SetItemTooltip("%s", label.c_str());
            ImGui::PopID();
        }
    }
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + 9 * (cell + gap) + gap * 3));
    ImGui::TextDisabled("pads mirror the hardware - edit data/padmap.json");
}

}  // namespace pifx
