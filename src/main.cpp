// pifx - waveform scope, audio router and effects box for the Raspberry Pi 5.
//
//   pifx                      ImGui front end (default)
//   pifx headless             engine + Launchpad, no window (for a service at boot)
//   pifx diag                 DAC HAT report, mixer state, audio devices
//   pifx devices              audio outputs / inputs per backend, MIDI ports
//   pifx tone [Hz] [s]        test tone straight to an output
//   pifx unhijack             undo a system-audio hijack left behind by a crash
//
// Run `pifx --help` for the options.
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "core/Devices.h"
#include "core/Hat.h"
#include "core/Hijack.h"
#include "core/Launchpad.h"
#include "core/Rig.h"
#include "core/Util.h"

#ifdef PIFX_HAVE_GUI
#include "ui/App.h"
#endif

using namespace pifx;

static volatile std::sig_atomic_t g_stop = 0;
static void onSignal(int) { g_stop = 1; }

static void usage() {
    std::printf(
        "pifx %s - waveform scope, audio router and effects box\n\n"
        "usage: pifx [command] [options]\n\n"
        "commands:\n"
        "  (none) | gui       ImGui front end\n"
        "  headless           engine + Launchpad without a window (Ctrl-C to stop)\n"
        "  diag               DAC HAT report, mixer state and audio devices\n"
        "  devices            list outputs, inputs (incl. monitors) and MIDI ports\n"
        "  tone [Hz] [sec]    play a test tone (default 1000 Hz, 3 s, -12 dBFS)\n"
        "  unhijack           restore PipeWire routing after a crash during a hijack\n\n"
        "options:\n"
        "  --backend B        auto | pulse (PipeWire) | alsa | jack\n"
        "  --output DEV       output device (key or part of its name); repeat for several;\n"
        "                     'none' = scope only. Default: last used, else the DAC HAT\n"
        "  --input DEV        capture device for the input source\n"
        "  --source S         tone | shape | sweep | noise | file[:name] | capture[:dev] | silence\n"
        "  --preset NAME      load a saved preset at start (slot1..slot8 or a name)\n"
        "  --hijack           route all system audio through pifx from the start (PipeWire)\n"
        "  --rate HZ          engine sample rate (48000)\n"
        "  --block N          frames per period (256 = 5.3 ms)\n"
        "  --data DIR         settings, presets, padmap.json (default ./data next to the repo)\n"
        "  --media DIR        audio files for the file source (default ./media)\n"
        "  --no-launchpad     do not look for a Launchpad\n"
        "  --midi-port P      Launchpad port (hw:1,0,1 or part of its name)\n"
        "  --level DB         tone level for `pifx tone` (-12)\n"
        "  --fullscreen       GUI: start fullscreen\n"
        "  --size WxH         GUI: window size\n"
        "  --scale F          GUI: UI scale (or $PIFX_UI_SCALE), e.g. 1.5 on a 4K screen\n"
        "  --view V           GUI: heat | wave | xy | spectrum | spectrogram\n"
        "  --screenshot PNG   GUI: render, save a screenshot, exit (with --frames N)\n"
        "  --no-audio         no audio devices; a timer clocks the engine\n",
        PIFX_VERSION);
}

static std::string findRoot() {
    std::string d = exeDir();
    for (int up = 0; up < 3 && !d.empty(); up++) {
        if (isDir(joinPath(d, "media")) || isDir(joinPath(d, "data")) ||
            fileExists(joinPath(d, "src/core/Rig.h")))
            return d;
        d = parentDir(d);
    }
    std::string fallback = joinPath(homeDir(), ".local/share/pifx");
    makeDirs(fallback);
    return fallback;
}

static int cmdDiag(Backend b) {
    HatStatus st = detectHat();
    std::printf("%s", formatReport(st).c_str());
    if (st.detected && st.card) {
        AmixerMixer mx(st.card->index);
        std::printf("\n");
        if (mx.ok()) {
            auto v = mx.volumeDb();
            auto a = mx.analogDb();
            std::printf("Mixer (card %d):\n", st.card->index);
            std::printf("  Digital volume : %+.1f dB   (207 raw = 0 dB; pifx caps it at 0 dB)\n", v ? *v : -999.0);
            std::printf("  Mute           : %s\n", mx.muted().value_or(false) ? "muted" : "on");
            std::printf("  Analogue stage : %+.0f dB\n", a ? *a : 0.0);
            std::printf("  DSP program    : %s\n", mx.dspProgram().c_str());
        } else {
            std::printf("Mixer: %s\n", mx.error().c_str());
        }
    }
    Engine e;
    AudioIO io(e);
    std::string err;
    if (io.init(b, &err)) {
        std::printf("\nAudio backend: %s\nOutputs:\n", io.activeBackend().c_str());
        for (const auto& d : io.playback())
            std::printf("  %s %-40s %s\n", d.isDefault ? "*" : " ", d.name.c_str(), d.key.c_str());
    } else {
        std::printf("\nAudio: %s\n", err.c_str());
    }
    return st.detected ? 0 : 1;
}

static int cmdDevices(Backend b) {
    Engine e;
    AudioIO io(e);
    std::string err;
    if (!io.init(b, &err)) {
        std::printf("audio: %s\n", err.c_str());
    } else {
        std::printf("backend: %s\n\noutputs (use the key or part of the name with --output):\n", io.activeBackend().c_str());
        for (const auto& d : io.playback())
            std::printf("  %s %-44s key: %s\n", d.isDefault ? "*" : " ", d.name.c_str(), d.key.c_str());
        std::printf("\ninputs (monitors record what an output is playing):\n");
        for (const auto& d : io.capture())
            std::printf("  %s %-44s key: %s%s\n", d.isDefault ? "*" : " ", d.name.c_str(), d.key.c_str(),
                        d.monitor ? "  [monitor]" : "");
    }
    std::printf("\nMIDI ports:\n");
    std::string why;
    if (!RawMidi::available(&why)) std::printf("  (%s)\n", why.c_str());
    for (const auto& p : RawMidi::list())
        std::printf("  %-10s %s%s\n", p.hw.c_str(), p.name.c_str(), identifyLaunchpad(p.name) ? "  <-- Launchpad" : "");
    return 0;
}

static int cmdTone(Backend b, double hz, double secs, float level, const std::vector<std::string>& outs) {
    Engine e;
    AudioIO io(e);
    std::string err;
    if (!io.init(b, &err)) {
        std::fprintf(stderr, "audio: %s\n", err.c_str());
        return 1;
    }
    auto* t = new ToneSource(e.sampleRate());
    ToneParams p;
    p.freq = (float)hz;
    p.level = level;
    t->setParams(p);
    e.setSource(t);
    Command c;
    c.type = Cmd::MasterDb;
    c.value = 0;
    e.post(c);
    std::vector<std::string> keys = outs;
    if (keys.empty()) {
        HatStatus st = detectHat();
        for (const auto& d : io.playback())
            if (st.card && (contains(d.name, st.card->id) || contains(d.key, st.card->id))) keys.push_back(d.key);
        if (keys.empty())
            for (const auto& d : io.playback())
                if (d.isDefault) keys.push_back(d.key);
    }
    if (keys.empty()) std::fprintf(stderr, "no output device found (pifx devices lists them; use --output)\n");
    if (!io.setOutputs(keys, &err) && !err.empty()) std::fprintf(stderr, "%s\n", err.c_str());
    std::printf("playing %.0f Hz at %.0f dBFS for %.1f s on %s\n", hz, level, secs, io.clockName().c_str());
    std::this_thread::sleep_for(std::chrono::milliseconds((int)(secs * 1000)));
    c.value = -60;
    e.post(c);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    io.shutdown();
    return 0;
}

int main(int argc, char** argv) {
    std::string cmd = "gui";
    Rig::Options opt;
    std::vector<std::string> positional;
    float level = -12;
    std::string backend = "auto";
#ifdef PIFX_HAVE_GUI
    GuiOptions gui;
#endif
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto val = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs a value\n", name);
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "-h" || a == "--help") { usage(); return 0; }
        else if (a == "--version") { std::printf("pifx %s\n", PIFX_VERSION); return 0; }
        else if (a == "--backend") backend = val("--backend");
        else if (a == "--output") { opt.outputs.push_back(val("--output")); opt.outputsGiven = true; }
        else if (a == "--input") opt.input = val("--input");
        else if (a == "--source") opt.source = val("--source");
        else if (a == "--preset") opt.preset = val("--preset");
        else if (a == "--hijack") opt.hijack = true;
        else if (a == "--rate") opt.sampleRate = std::atoi(val("--rate").c_str());
        else if (a == "--block") opt.block = std::atoi(val("--block").c_str());
        else if (a == "--data") opt.dataDir = val("--data");
        else if (a == "--media") opt.mediaDir = val("--media");
        else if (a == "--no-launchpad") opt.launchpad = false;
        else if (a == "--midi-port") opt.midiPort = val("--midi-port");
        else if (a == "--level") level = (float)std::atof(val("--level").c_str());
        else if (a == "--no-audio") opt.audio = false;
#ifdef PIFX_HAVE_GUI
        else if (a == "--fullscreen") gui.fullscreen = true;
        else if (a == "--size") std::sscanf(val("--size").c_str(), "%dx%d", &gui.width, &gui.height);
        else if (a == "--scale") gui.scale = (float)std::atof(val("--scale").c_str());
        else if (a == "--view") gui.view = val("--view");
        else if (a == "--screenshot") gui.screenshot = val("--screenshot");
        else if (a == "--frames") gui.frames = std::atoi(val("--frames").c_str());
#endif
        else if (!a.empty() && a[0] == '-') { std::fprintf(stderr, "unknown option %s (try --help)\n", a.c_str()); return 2; }
        else if (cmd == "gui" && positional.empty() &&
                 (a == "gui" || a == "headless" || a == "diag" || a == "devices" || a == "tone" || a == "unhijack"))
            cmd = a;
        else positional.push_back(a);
    }
    opt.backend = backend;
    const std::string root = findRoot();
    if (opt.dataDir.empty()) opt.dataDir = joinPath(root, "data");
    if (opt.mediaDir.empty()) opt.mediaDir = joinPath(root, "media");
    opt.sampleRate = std::max(8000, std::min(192000, opt.sampleRate));
    opt.block = std::max(32, std::min(4096, opt.block));

    if (cmd == "diag") return cmdDiag(backendFromName(backend));
    if (cmd == "devices") return cmdDevices(backendFromName(backend));
    if (cmd == "tone") {
        double hz = positional.size() > 0 ? std::atof(positional[0].c_str()) : 1000;
        double s = positional.size() > 1 ? std::atof(positional[1].c_str()) : 3;
        return cmdTone(backendFromName(backend), hz, s, level, opt.outputs);
    }
    if (cmd == "unhijack") {
        std::string m = Hijack::repair(joinPath(opt.dataDir, "hijack.json"));
        std::printf("%s\n", m.empty() ? "nothing to restore" : m.c_str());
        return 0;
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

#ifdef PIFX_HAVE_GUI
    if (cmd == "gui") {
        Rig rig(opt);
        rig.start();
        int rc = runGui(rig, gui, &g_stop);
        rig.shutdown();
        return rc;
    }
#else
    if (cmd == "gui") std::fprintf(stderr, "built without the GUI (PIFX_GUI=OFF): running headless\n");
#endif
    Rig rig(opt);
    rig.start();
    std::printf("pifx %s headless: engine on %s (%d Hz, %d frames). Ctrl-C to stop.\n", PIFX_VERSION,
                rig.io().clockName().c_str(), rig.engine().sampleRate(), opt.block);
    while (!g_stop) {
        rig.update();
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    rig.shutdown();
    return 0;
}
