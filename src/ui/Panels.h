// The side panels: Signal (source, routing, DAC), Effects (chain, tempo, presets) and
// the virtual Launchpad.
#pragma once
#include <string>
#include <vector>

#include "imgui.h"

namespace pifx {

class Rig;
struct AudioClip;

class SignalPanel {
public:
    void draw(Rig& rig);

private:
    void drawSource(Rig& rig);
    void drawRouting(Rig& rig);
    void drawDac(Rig& rig);
    void drawFile(Rig& rig);
    void drawCapture(Rig& rig);
    char path_[512] = {};
    const AudioClip* ovClip_ = nullptr;
    std::vector<double> ovX_, ovLo_, ovHi_;
    std::vector<std::string> files_;
    double filesAt_ = -10;
    float dacVol_ = -12;
    bool dacDragging_ = false;
    double dacSentAt_ = 0;
    bool showDiag_ = false;
    std::string diagText_;
};

class FxPanel {
public:
    void draw(Rig& rig);

private:
    char presetName_[64] = {};
};

class PadPanel {
public:
    void draw(Rig& rig);
};

extern ImFont* gMonoFont;

}  // namespace pifx
