#include "ui/Gl.h"

#include <GLES3/gl3.h>

#include "implot.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#pragma GCC diagnostic pop

namespace pifx {

Texture::~Texture() {
    if (tex_) glDeleteTextures(1, &tex_);
}

void Texture::upload(const uint32_t* rgba, int w, int h, bool smooth) {
    if (!tex_) glGenTextures(1, &tex_);
    glBindTexture(GL_TEXTURE_2D, tex_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    if (w != w_ || h != h_ || smooth != smooth_) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, smooth ? GL_LINEAR : GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, smooth ? GL_LINEAR : GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        w_ = w;
        h_ = h;
        smooth_ = smooth;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    }
}

bool saveScreenshot(const std::string& path, int w, int h) {
    std::vector<unsigned char> px((size_t)w * h * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
    stbi_flip_vertically_on_write(1);
    return stbi_write_png(path.c_str(), w, h, 4, px.data(), w * 4) != 0;
}

void ColorLut::build(int c) {
    cmap = c;
    for (int i = 0; i < 256; i++) {
        ImVec4 v = ImPlot::SampleColormap(i / 255.0f, c);
        rgba[i] = ImGui::ColorConvertFloat4ToU32(ImVec4(v.x, v.y, v.z, 1.0f));   // ABGR = RGBA bytes in memory
    }
}

void toRgba(const std::vector<float>& img, const ColorLut& lut, std::vector<uint32_t>& out) {
    out.resize(img.size());
    for (size_t i = 0; i < img.size(); i++) out[i] = lut(img[i]);
}

}  // namespace pifx
