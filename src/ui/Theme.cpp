#include "ui/Theme.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <initializer_list>

#include "implot.h"

namespace pifx {

int kCmapInferno = -1, kCmapPhosphor = -1, kCmapIce = -1;

ImVec4 hexColor(const char* hex, float alpha) {
    if (*hex == '#') hex++;
    unsigned v = (unsigned)std::strtoul(hex, nullptr, 16);
    return ImVec4(((v >> 16) & 255) / 255.0f, ((v >> 8) & 255) / 255.0f, (v & 255) / 255.0f, alpha);
}

ImVec4 theme::channel(int ch) {
    static const char* c[4] = {"#4FD1C5", "#63B3ED", "#F6AD55", "#FC8181"};
    return hexColor(c[ch & 3]);
}

void applyTheme(float scale) {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowPadding = ImVec2(10, 10);
    s.FramePadding = ImVec2(8, 4);
    s.CellPadding = ImVec2(6, 3);
    s.ItemSpacing = ImVec2(8, 6);
    s.ItemInnerSpacing = ImVec2(6, 4);
    s.IndentSpacing = 16;
    s.ScrollbarSize = 12;
    s.GrabMinSize = 10;
    s.WindowBorderSize = 1;
    s.ChildBorderSize = 1;
    s.PopupBorderSize = 1;
    s.FrameBorderSize = 0;
    s.TabBorderSize = 0;
    s.WindowRounding = 0;
    s.ChildRounding = 4;
    s.FrameRounding = 4;
    s.PopupRounding = 4;
    s.ScrollbarRounding = 6;
    s.GrabRounding = 3;
    s.TabRounding = 4;
    s.WindowTitleAlign = ImVec2(0.0f, 0.5f);
    s.SeparatorTextBorderSize = 1;
    s.SeparatorTextPadding = ImVec2(0, 3);
    s.DockingSeparatorSize = 2;

    ImVec4* c = s.Colors;
    c[ImGuiCol_Text] = hexColor("#D7DCE2");
    c[ImGuiCol_TextDisabled] = hexColor("#6B7480");
    c[ImGuiCol_WindowBg] = hexColor("#0E1013");
    c[ImGuiCol_ChildBg] = hexColor("#0E1013", 0.0f);
    c[ImGuiCol_PopupBg] = hexColor("#15181D", 0.98f);
    c[ImGuiCol_Border] = hexColor("#252A31");
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = hexColor("#1A1E24");
    c[ImGuiCol_FrameBgHovered] = hexColor("#222831");
    c[ImGuiCol_FrameBgActive] = hexColor("#2A313C");
    c[ImGuiCol_TitleBg] = hexColor("#0B0D10");
    c[ImGuiCol_TitleBgActive] = hexColor("#11151A");
    c[ImGuiCol_TitleBgCollapsed] = hexColor("#0B0D10");
    c[ImGuiCol_MenuBarBg] = hexColor("#0B0D10");
    c[ImGuiCol_ScrollbarBg] = hexColor("#0E1013");
    c[ImGuiCol_ScrollbarGrab] = hexColor("#2A313C");
    c[ImGuiCol_ScrollbarGrabHovered] = hexColor("#36404D");
    c[ImGuiCol_ScrollbarGrabActive] = hexColor("#425062");
    c[ImGuiCol_CheckMark] = theme::kAccent;
    c[ImGuiCol_SliderGrab] = hexColor("#3FD0B4", 0.85f);
    c[ImGuiCol_SliderGrabActive] = hexColor("#7FF0D8");
    c[ImGuiCol_Button] = hexColor("#1E242C");
    c[ImGuiCol_ButtonHovered] = hexColor("#2A3440");
    c[ImGuiCol_ButtonActive] = hexColor("#34414F");
    c[ImGuiCol_Header] = hexColor("#1D232B");
    c[ImGuiCol_HeaderHovered] = hexColor("#26303B");
    c[ImGuiCol_HeaderActive] = hexColor("#2E3A47");
    c[ImGuiCol_Separator] = hexColor("#252A31");
    c[ImGuiCol_SeparatorHovered] = hexColor("#3FD0B4", 0.6f);
    c[ImGuiCol_SeparatorActive] = theme::kAccent;
    c[ImGuiCol_ResizeGrip] = hexColor("#3FD0B4", 0.15f);
    c[ImGuiCol_ResizeGripHovered] = hexColor("#3FD0B4", 0.5f);
    c[ImGuiCol_ResizeGripActive] = theme::kAccent;
    c[ImGuiCol_TabHovered] = hexColor("#26303B");
    c[ImGuiCol_Tab] = hexColor("#14181D");
    c[ImGuiCol_TabSelected] = hexColor("#1E2630");
    c[ImGuiCol_TabSelectedOverline] = theme::kAccent;
    c[ImGuiCol_TabDimmed] = hexColor("#101317");
    c[ImGuiCol_TabDimmedSelected] = hexColor("#181E26");
    c[ImGuiCol_TabDimmedSelectedOverline] = hexColor("#3FD0B4", 0.4f);
    c[ImGuiCol_DockingPreview] = hexColor("#3FD0B4", 0.35f);
    c[ImGuiCol_DockingEmptyBg] = hexColor("#0B0D10");
    c[ImGuiCol_PlotLines] = theme::kAccent;
    c[ImGuiCol_PlotHistogram] = theme::kAccent;
    c[ImGuiCol_TableHeaderBg] = hexColor("#161A20");
    c[ImGuiCol_TableBorderStrong] = hexColor("#252A31");
    c[ImGuiCol_TableBorderLight] = hexColor("#1C2027");
    c[ImGuiCol_TableRowBgAlt] = hexColor("#FFFFFF", 0.02f);
    c[ImGuiCol_TextSelectedBg] = hexColor("#3FD0B4", 0.3f);
    c[ImGuiCol_NavCursor] = theme::kAccent;
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.6f);

    ImPlotStyle& p = ImPlot::GetStyle();
    p.PlotPadding = ImVec2(8, 8);
    p.LabelPadding = ImVec2(5, 3);
    p.LegendPadding = ImVec2(8, 8);
    p.MinorAlpha = 0.35f;
    p.PlotBorderSize = 1;
    p.Colors[ImPlotCol_FrameBg] = ImVec4(0, 0, 0, 0);
    p.Colors[ImPlotCol_PlotBg] = hexColor("#08090B");
    p.Colors[ImPlotCol_PlotBorder] = hexColor("#252A31");
    p.Colors[ImPlotCol_LegendBg] = hexColor("#0E1013", 0.85f);
    p.Colors[ImPlotCol_LegendBorder] = hexColor("#252A31");
    p.Colors[ImPlotCol_AxisText] = hexColor("#8C96A3");
    p.Colors[ImPlotCol_AxisGrid] = hexColor("#FFFFFF", 0.07f);
    p.Colors[ImPlotCol_AxisTick] = hexColor("#FFFFFF", 0.15f);
    p.Colors[ImPlotCol_InlayText] = hexColor("#C0C8D2");
    p.Colors[ImPlotCol_Crosshairs] = hexColor("#FFFFFF", 0.4f);

    if (kCmapInferno < 0) {
        // matplotlib "inferno" at 10 stops (perceptually uniform, good for probability)
        const ImU32 inferno[] = {IM_COL32(0, 0, 4, 255),       IM_COL32(27, 12, 65, 255),  IM_COL32(74, 12, 107, 255),
                                 IM_COL32(120, 28, 109, 255),  IM_COL32(165, 44, 96, 255), IM_COL32(207, 68, 70, 255),
                                 IM_COL32(237, 105, 37, 255),  IM_COL32(251, 155, 6, 255), IM_COL32(247, 209, 61, 255),
                                 IM_COL32(252, 255, 164, 255)};
        kCmapInferno = ImPlot::AddColormap("Inferno", inferno, 10, false);
        // green phosphor, like an analogue scope
        const ImU32 phosphor[] = {IM_COL32(0, 0, 0, 255),       IM_COL32(3, 26, 14, 255),   IM_COL32(8, 70, 38, 255),
                                  IM_COL32(20, 140, 74, 255),   IM_COL32(70, 220, 128, 255), IM_COL32(170, 255, 200, 255),
                                  IM_COL32(240, 255, 245, 255)};
        kCmapPhosphor = ImPlot::AddColormap("Phosphor", phosphor, 7, false);
        const ImU32 ice[] = {IM_COL32(4, 6, 10, 255),      IM_COL32(16, 36, 64, 255),  IM_COL32(28, 84, 128, 255),
                             IM_COL32(48, 150, 180, 255),  IM_COL32(110, 210, 210, 255), IM_COL32(230, 250, 250, 255)};
        kCmapIce = ImPlot::AddColormap("Ice", ice, 6, false);
    }
    if (scale != 1.0f) s.ScaleAllSizes(scale);
}

void sectionHeader(const char* label) {
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, theme::kMuted);
    ImGui::SeparatorText(label);
    ImGui::PopStyleColor();
}

bool lampButton(const char* id, bool on, const ImVec4& color, float r) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    float h = ImGui::GetFrameHeight();
    bool pressed = ImGui::InvisibleButton(id, ImVec2(h, h));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 c(p.x + h * 0.5f, p.y + h * 0.5f);
    ImVec4 off(color.x * 0.25f, color.y * 0.25f, color.z * 0.25f, 1.0f);
    if (on) dl->AddCircleFilled(c, r * 1.9f, ImGui::GetColorU32(ImVec4(color.x, color.y, color.z, 0.18f)), 24);
    dl->AddCircleFilled(c, r, ImGui::GetColorU32(on ? color : off), 24);
    dl->AddCircle(c, r, ImGui::GetColorU32(ImVec4(color.x, color.y, color.z, on ? 1.0f : 0.5f)), 24, 1.0f);
    if (ImGui::IsItemHovered()) dl->AddCircle(c, r + 2, ImGui::GetColorU32(ImGuiCol_Text), 24, 1.0f);
    return pressed;
}

void levelMeter(const char* id, float peak, float width, float height) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(width, height));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + width, p.y + height), ImGui::GetColorU32(hexColor("#15181D")), 2);
    float db = 20.0f * std::log10(std::max(peak, 1e-6f));
    float u = std::max(0.0f, std::min(1.0f, (db + 60.0f) / 60.0f));
    ImVec4 col = db > -1 ? theme::kBad : db > -12 ? theme::kWarn : theme::kAccent;
    if (u > 0) dl->AddRectFilled(p, ImVec2(p.x + width * u, p.y + height), ImGui::GetColorU32(col), 2);
    for (int m : {-48, -36, -24, -12, -6}) {
        float x = p.x + width * (m + 60.0f) / 60.0f;
        dl->AddLine(ImVec2(x, p.y), ImVec2(x, p.y + height), ImGui::GetColorU32(hexColor("#000000", 0.5f)));
    }
}

void helpMarker(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

}  // namespace pifx
