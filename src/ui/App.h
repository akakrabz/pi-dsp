// The ImGui front end: SDL2 window + OpenGL ES 3 + Dear ImGui (docking) + ImPlot.
#pragma once
#include <csignal>
#include <string>

namespace pifx {

class Rig;

struct GuiOptions {
    bool fullscreen = false;
    int width = 1600, height = 960;
    float scale = 0;              // UI scale; 0 = from $PIFX_UI_SCALE or 1.0
    std::string view;             // heat | wave | xy | spectrum | spectrogram
    std::string screenshot;       // save a PNG after `frames` frames and exit
    int frames = 90;
};

int runGui(Rig& rig, const GuiOptions& opt, volatile std::sig_atomic_t* stop);

}  // namespace pifx
