#include "ui/App.h"

#include <SDL.h>
#include <GLES3/gl3.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "core/Rig.h"
#include "core/Util.h"
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl2.h"
#include "imgui_internal.h"
#include "implot.h"
#include "ui/Gl.h"
#include "ui/Panels.h"
#include "ui/ScopeView.h"
#include "ui/Theme.h"

extern const unsigned char pifx_font_sans[];
extern const unsigned int pifx_font_sans_size;
extern const unsigned char pifx_font_mono[];
extern const unsigned int pifx_font_mono_size;

namespace pifx {

ImFont* gMonoFont = nullptr;

static void buildLayout(ImGuiID dock, ImVec2 size) {
    ImGui::DockBuilderRemoveNode(dock);
    ImGui::DockBuilderAddNode(dock, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dock, size);
    ImGuiID left, center, right, rightBottom;
    ImGui::DockBuilderSplitNode(dock, ImGuiDir_Left, 0.22f, &left, &center);
    ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.27f, &right, &center);
    ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.40f, &rightBottom, &right);
    ImGui::DockBuilderDockWindow("Signal", left);
    ImGui::DockBuilderDockWindow("Scope", center);
    ImGui::DockBuilderDockWindow("Effects", right);
    ImGui::DockBuilderDockWindow("Launchpad", rightBottom);
    ImGui::DockBuilderFinish(dock);
}

static void drawStatus(Rig& rig, float fps) {
    Engine& e = rig.engine();
    AudioIO& io = rig.io();
    uint32_t under = 0;
    for (const auto& o : io.outputs()) under += o.underruns;
    std::string clock = io.clockName();
    if (clock.size() > 34) clock = clock.substr(0, 32) + "...";
    std::string s = format("%s   %d Hz   %d fr (%.1f ms)   DSP %.0f%%   %s%.0f fps", clock.c_str(), e.sampleRate(),
                           io.blockFrames(), 1000.0 * io.blockFrames() / e.sampleRate(), 100 * e.load.load(),
                           under ? format("underruns %u   ", under).c_str() : "", fps);
    const float w = ImGui::CalcTextSize(s.c_str()).x;
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX() + 20, ImGui::GetWindowWidth() - w - 14));
    ImGui::PushStyleColor(ImGuiCol_Text, theme::kMuted);
    ImGui::TextUnformatted(s.c_str());
    ImGui::PopStyleColor();
}

static void drawToasts(Rig& rig) {
    auto& msgs = rig.messages();
    if (msgs.empty()) return;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - 14, vp->WorkPos.y + vp->WorkSize.y - 14), ImGuiCond_Always,
                            ImVec2(1, 1));
    ImGui::SetNextWindowBgAlpha(0.92f);
    ImGuiWindowFlags f = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDocking;
    if (ImGui::Begin("##toasts", nullptr, f)) {
        const double now = nowSeconds();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 34);
        for (const auto& m : msgs) {
            float a = (float)std::clamp(8.0 - (now - m.second), 0.0, 1.0);
            ImGui::TextColored(ImVec4(0.85f, 0.88f, 0.92f, a), "%s", m.first.c_str());
        }
        ImGui::PopTextWrapPos();
        if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(0)) msgs.clear();
    }
    ImGui::End();
}

int runGui(Rig& rig, const GuiOptions& opt, volatile std::sig_atomic_t* stop) {
    SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        std::fprintf(stderr, "SDL: %s\n(no display? run `pifx headless`)\n", SDL_GetError());
        return 1;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    Uint32 wf = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
    if (opt.fullscreen) wf |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    SDL_Window* win = SDL_CreateWindow("pifx", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, opt.width, opt.height, wf);
    if (!win) {
        std::fprintf(stderr, "SDL window: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    SDL_GLContext gl = SDL_GL_CreateContext(win);
    if (!gl) {
        std::fprintf(stderr, "OpenGL ES 3 context: %s\n", SDL_GetError());
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 1;
    }
    SDL_GL_MakeCurrent(win, gl);
    SDL_GL_SetSwapInterval(opt.screenshot.empty() ? 1 : 0);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_NavEnableKeyboard;
    const std::string ini = joinPath(rig.options().dataDir, "imgui.ini");
    const bool firstRun = !fileExists(ini) || !opt.screenshot.empty();
    static std::string iniStore;
    iniStore = ini;
    io.IniFilename = opt.screenshot.empty() ? iniStore.c_str() : nullptr;

    float scale = opt.scale;
    if (scale <= 0) {
        const char* env = std::getenv("PIFX_UI_SCALE");
        scale = env ? (float)std::atof(env) : 1.0f;
    }
    scale = std::clamp(scale, 0.5f, 3.0f);
    ImFontConfig fc;
    fc.FontDataOwnedByAtlas = false;
    fc.OversampleH = 2;
    io.Fonts->AddFontFromMemoryTTF((void*)pifx_font_sans, (int)pifx_font_sans_size, 15.0f * scale, &fc);
    gMonoFont = io.Fonts->AddFontFromMemoryTTF((void*)pifx_font_mono, (int)pifx_font_mono_size, 14.0f * scale, &fc);
    ImGui::GetStyle().FontScaleMain = 1.0f;
    applyTheme(scale);

    ImGui_ImplSDL2_InitForOpenGL(win, gl);
    ImGui_ImplOpenGL3_Init("#version 300 es");

    ScopeView scope;
    if (rig.settings().contains("scope")) scope.load(rig.settings()["scope"]);
    if (!opt.view.empty()) scope.setView(opt.view);
    rig.view = scope.viewName();
    SignalPanel signal;
    FxPanel fx;
    PadPanel pads;
    bool showSignal = true, showFx = true, showPads = true, showDemo = false, showPlotDemo = false, showHelp = false;
    bool resetLayout = firstRun;
    bool done = false;
    int frame = 0;
    int rc = 0;
    Uint64 last = SDL_GetPerformanceCounter();
    float fps = 60;

    while (!done) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            ImGui_ImplSDL2_ProcessEvent(&ev);
            if (ev.type == SDL_QUIT) done = true;
            if (ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_CLOSE && ev.window.windowID == SDL_GetWindowID(win))
                done = true;
        }
        if (stop && *stop) done = true;
        if (SDL_GetWindowFlags(win) & SDL_WINDOW_MINIMIZED) {
            rig.update();
            SDL_Delay(20);
            continue;
        }
        const Uint64 now = SDL_GetPerformanceCounter();
        float dt = (float)((double)(now - last) / SDL_GetPerformanceFrequency());
        last = now;
        dt = std::clamp(dt, 0.0f, 0.25f);
        fps += (1.0f / std::max(dt, 1e-3f) - fps) * 0.05f;

        rig.update();
        scope.update(rig, dt);
        static double lastSave = ImGui::GetTime();
        if (opt.screenshot.empty() && ImGui::GetTime() - lastSave > 15.0) {   // survive a crash
            rig.settings()["scope"] = scope.save();
            rig.saveSettings();
            lastSave = ImGui::GetTime();
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        // keyboard shortcuts (not while typing)
        if (!io.WantTextInput) {
            if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) scope.hold = !scope.hold;
            static const char* views[] = {"heat", "wave", "xy", "spectrum", "spectrogram"};
            for (int i = 0; i < 5; i++)
                if (ImGui::IsKeyPressed((ImGuiKey)(ImGuiKey_1 + i), false)) {
                    scope.setView(views[i]);
                    rig.view = views[i];
                }
            if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket)) scope.window = std::max(1e-4, scope.window / 2);
            if (ImGui::IsKeyPressed(ImGuiKey_RightBracket)) scope.window = std::min(20.0, scope.window * 2);
            if (ImGui::IsKeyPressed(ImGuiKey_F11, false)) {
                bool fs = SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN_DESKTOP;
                SDL_SetWindowFullscreen(win, fs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
            }
        }

        if (ImGui::BeginMainMenuBar()) {
            ImGui::TextColored(theme::kAccent, "pifx");
            ImGui::SameLine(0, 14);
            if (ImGui::BeginMenu("View")) {
                ImGui::MenuItem("Signal", nullptr, &showSignal);
                ImGui::MenuItem("Effects", nullptr, &showFx);
                ImGui::MenuItem("Launchpad", nullptr, &showPads);
                ImGui::Separator();
                if (ImGui::MenuItem("Reset layout")) resetLayout = true;
                if (ImGui::MenuItem("Fullscreen", "F11")) {
                    bool fs = SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN_DESKTOP;
                    SDL_SetWindowFullscreen(win, fs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
                }
                ImGui::Separator();
                ImGui::MenuItem("ImGui demo", nullptr, &showDemo);
                ImGui::MenuItem("ImPlot demo", nullptr, &showPlotDemo);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Help")) {
                ImGui::MenuItem("Keys", nullptr, &showHelp);
                ImGui::TextDisabled("pifx %s", PIFX_VERSION);
                ImGui::EndMenu();
            }
            drawStatus(rig, fps);
            ImGui::EndMainMenuBar();
        }

        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGuiID dock = ImGui::DockSpaceOverViewport(0, vp);
        if (resetLayout) {
            buildLayout(dock, vp->WorkSize);
            resetLayout = false;
        }

        ImGui::Begin("Scope", nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        scope.draw(rig);
        ImGui::End();
        if (showSignal) {
            ImGui::Begin("Signal", &showSignal);
            signal.draw(rig);
            ImGui::End();
        }
        if (showFx) {
            ImGui::Begin("Effects", &showFx);
            fx.draw(rig);
            ImGui::End();
        }
        if (showPads) {
            ImGui::Begin("Launchpad", &showPads);
            pads.draw(rig);
            ImGui::End();
        }
        if (showHelp) {
            ImGui::Begin("Keys", &showHelp, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking);
            ImGui::TextUnformatted("Space        hold / run\n1 - 5        heatmap, waveform, X-Y, spectrum, spectrogram\n"
                                   "[  ]         halve / double the window\nwheel        window (time) over a plot\n"
                                   "shift+wheel  vertical range\ndrag yellow  trigger level\nF11          fullscreen");
            ImGui::End();
        }
        if (showDemo) ImGui::ShowDemoWindow(&showDemo);
        if (showPlotDemo) ImPlot::ShowDemoWindow(&showPlotDemo);
        drawToasts(rig);

        ImGui::Render();
        int w, h;
        SDL_GL_GetDrawableSize(win, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.043f, 0.051f, 0.063f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        frame++;
        if (!opt.screenshot.empty() && frame >= opt.frames) {
            if (!saveScreenshot(opt.screenshot, w, h)) {
                std::fprintf(stderr, "could not write %s\n", opt.screenshot.c_str());
                rc = 1;
            }
            done = true;
        }
        SDL_GL_SwapWindow(win);
        if (!opt.screenshot.empty()) SDL_Delay(16);   // let the timer clock produce audio in real time
    }

    if (opt.screenshot.empty()) {
        rig.settings()["scope"] = scope.save();
        rig.saveSettings();
    }
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(gl);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return rc;
}

}  // namespace pifx
