// Look and feel: a dark instrument panel, one teal accent, fixed channel colours.
#pragma once
#include "imgui.h"

namespace pifx {

namespace theme {
// Channel colours, shared by every view (In L, In R, Out L, Out R).
ImVec4 channel(int ch);
inline const ImVec4 kAccent = ImVec4(0.247f, 0.816f, 0.706f, 1.0f);      // #3FD0B4
inline const ImVec4 kAccentDim = ImVec4(0.247f, 0.816f, 0.706f, 0.35f);
inline const ImVec4 kWarn = ImVec4(0.965f, 0.678f, 0.333f, 1.0f);         // #F6AD55
inline const ImVec4 kBad = ImVec4(0.988f, 0.506f, 0.506f, 1.0f);          // #FC8181
inline const ImVec4 kMuted = ImVec4(0.55f, 0.60f, 0.66f, 1.0f);
inline const ImVec4 kTrigger = ImVec4(0.965f, 0.878f, 0.369f, 0.9f);      // #F6E05E
}  // namespace theme

// Custom colormaps registered with ImPlot (indices valid after applyTheme).
extern int kCmapInferno, kCmapPhosphor, kCmapIce;

void applyTheme(float scale);
ImVec4 hexColor(const char* hex, float alpha = 1.0f);

// Small widgets
void sectionHeader(const char* label);
bool lampButton(const char* id, bool on, const ImVec4& color, float radius);
void levelMeter(const char* id, float peak, float width, float height);   // peak is linear
void helpMarker(const char* text);

}  // namespace pifx
