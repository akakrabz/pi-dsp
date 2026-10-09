// Minimal OpenGL ES helpers: an RGBA texture for heatmaps and a framebuffer grab.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "imgui.h"

namespace pifx {

class Texture {
public:
    ~Texture();
    // Uploads w*h RGBA8 pixels (row 0 = top). Reallocates when the size changes.
    void upload(const uint32_t* rgba, int w, int h, bool smooth = true);
    ImTextureID id() const { return (ImTextureID)(intptr_t)tex_; }
    bool valid() const { return tex_ != 0; }

private:
    unsigned tex_ = 0;
    int w_ = 0, h_ = 0;
    bool smooth_ = true;
};

// Reads the current back buffer and writes a PNG.
bool saveScreenshot(const std::string& path, int w, int h);

// 256-entry RGBA lookup for an ImPlot colormap.
struct ColorLut {
    uint32_t rgba[256];
    int cmap = -1;
    void build(int cmap);
    inline uint32_t operator()(float u) const {
        int i = (int)(u * 255.0f + 0.5f);
        return rgba[i < 0 ? 0 : i > 255 ? 255 : i];
    }
};

void toRgba(const std::vector<float>& img, const ColorLut& lut, std::vector<uint32_t>& out);

}  // namespace pifx
