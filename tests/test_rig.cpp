// The Rig end to end without audio hardware: a timer clocks the engine, so tones,
// effects, presets and pads all run through the real audio path into the History.
#include <thread>
#include <unistd.h>

#include "check.h"
#include "core/Hijack.h"
#include "core/Rig.h"
#include "core/Util.h"

using namespace pifx;

struct TmpRig {
    std::string dir;
    std::unique_ptr<Rig> rig;
    TmpRig() {
        char tmpl[] = "/tmp/pifx-rig-XXXXXX";
        dir = mkdtemp(tmpl);
        Rig::Options o;
        o.dataDir = dir + "/data";
        o.mediaDir = dir + "/media";
        o.audio = false;
        o.launchpad = false;
        rig = std::make_unique<Rig>(o);
        rig->start();
    }
    ~TmpRig() {
        rig.reset();
        runCapture("rm -rf " + shellQuote(dir));
    }
    void run(double seconds) {
        auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
        while (std::chrono::steady_clock::now() < end) {
            rig->update();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        rig->update();
    }
};

TEST(rig_timer_clock_feeds_history_with_the_tone) {
    TmpRig t;
    ToneParams p;
    p.freq = 1000;
    p.level = -6;
    t.rig->setTone(p);
    t.run(0.3);
    History& h = t.rig->history();
    CHECK(h.end() > 48000 / 10);
    std::vector<float> x(4800);
    h.copy(kInL, (int64_t)h.end() - 4800, 4800, x.data());
    Measurements m = measure(x.data(), 4800, 48000);
    CHECK_NEAR(m.freq, 1000, 2);
    CHECK_NEAR(m.max, 0.501, 0.01);
    // post-FX path = master -6 dB
    h.copy(kOutL, (int64_t)h.end() - 4800, 4800, x.data());
    CHECK_NEAR(measure(x.data(), 4800, 48000).max, 0.501 * 0.501, 0.01);
}

TEST(rig_tap_tempo_and_delay_div) {
    TmpRig t;
    t.rig->setTempo(100);
    t.rig->delayDiv(0.25);
    int d = effectIndex("delay");
    CHECK(t.rig->fx()[d].enabled);
    CHECK_NEAR(t.rig->fx()[d].values[effectDescs()[d].paramIndex("time")], 600.0, 0.01);
    for (int i = 0; i < 4; i++) {
        t.rig->tap();
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
    CHECK(t.rig->tempo() > 180 && t.rig->tempo() < 210);
}

TEST(rig_hold_param_restores) {
    TmpRig t;
    // default pad map: (1,6) = hold filter cutoff 250 lowpass q 1.2
    int f = effectIndex("filter");
    t.rig->setParam("filter", "cutoff", json(1500));
    t.rig->padEvent(1, 6, true);
    CHECK(t.rig->fx()[f].enabled);
    CHECK_NEAR(t.rig->fx()[f].values[1], 250, 1e-3);
    CHECK(t.rig->padHeld(1, 6));
    t.rig->padEvent(1, 6, false);
    CHECK(!t.rig->fx()[f].enabled);
    CHECK_NEAR(t.rig->fx()[f].values[1], 1500, 1e-3);
    CHECK(!t.rig->padHeld(1, 6));
}

TEST(rig_presets_roundtrip_python_format) {
    TmpRig t;
    t.rig->setParam("delay", "mix", json(0.77));
    t.rig->setEnabled(effectIndex("delay"), true);
    t.rig->setParam("filter", "mode", json("highpass"));
    t.rig->setMasterDb(-3);
    CHECK(t.rig->savePreset("slot2"));
    CHECK(t.rig->currentPreset() == 2);
    t.rig->panic();
    CHECK(!t.rig->fx()[effectIndex("delay")].enabled);
    // a preset written by the old Python version (enum stored as index)
    writeFile(t.dir + "/data/presets/old.json",
              R"({"chain":{"master_db":-9,"bypass_all":false,"fx":[{"id":"drive","enabled":true,"params":{"mode":3,"drive":20}}]},
                  "source":{"kind":"tone","mode":"shape","shape":"rose","a":5,"freq":80},"tempo_bpm":90})");
    CHECK(t.rig->loadPreset("old"));
    CHECK(t.rig->fx()[effectIndex("drive")].enabled);
    CHECK(t.rig->fx()[effectIndex("drive")].values[0] == 3);
    CHECK(t.rig->tone().mode == 1 && kShapes[t.rig->tone().shape] == "rose" && t.rig->tone().a == 5);
    CHECK_NEAR(t.rig->masterDb(), -9, 1e-6);
    CHECK(t.rig->loadPreset("slot2"));
    CHECK(t.rig->fx()[effectIndex("delay")].enabled);
    CHECK_NEAR(t.rig->fx()[effectIndex("delay")].values[2], 0.77, 1e-6);
    CHECK(t.rig->fx()[effectIndex("filter")].values[0] == 1);
    CHECK(t.rig->presets().size() == 2);
}

TEST(rig_file_source_plays_demo_loop) {
    TmpRig t;
    auto files = t.rig->mediaFiles();
    CHECK(files.size() == 1 && files[0] == "demo-tone-loop.wav");
    t.rig->stepFile(0);
    for (int i = 0; i < 200 && t.rig->sourceKind() != SourceKind::File; i++) t.run(0.01);
    CHECK(t.rig->sourceKind() == SourceKind::File);
    FileSource* f = t.rig->file();
    CHECK(f && f->clip().frames() == 4 * 48000);
    t.run(0.2);
    CHECK(f->pos.load() > 4800);
    f->seekTo.store(96000);
    t.run(0.05);
    CHECK(f->pos.load() >= 96000);
}

TEST(rig_pad_colors_follow_state) {
    TmpRig t;
    auto c = t.rig->padColors();
    CHECK(c[7 * 9 + 0] == dim("cyan"));                     // filter off
    t.rig->padEvent(0, 7, true);
    t.rig->padEvent(0, 7, false);
    c = t.rig->padColors();
    CHECK(c[7 * 9 + 0] == palette("cyan"));                  // filter on
    t.rig->padEvent(3, 4, true);                             // lissajous 3:4
    CHECK(t.rig->tone().mode == 1 && t.rig->tone().a == 3 && t.rig->tone().b == 4);
    CHECK(t.rig->padColors()[4 * 9 + 3] == palette("purple"));
}

TEST(hijack_parses_pactl_output) {
    auto j = Hijack::parseSinkInputsJson(
        R"([{"index":42,"sink":3,"properties":{"application.name":"Firefox"}},{"index":43,"sink":5,"properties":{"application.name":"pifx"}}])");
    CHECK(j.size() == 2 && j[0].index == 42 && j[0].sink == 3 && j[0].app == "Firefox" && j[1].app == "pifx");
    auto t = Hijack::parseSinkInputsText(
        "Sink Input #42\n\tDriver: protocol-native.c\n\tSink: 3\n\tProperties:\n\t\tapplication.name = \"Firefox\"\n"
        "Sink Input #43\n\tSink: 5\n\t\tapplication.name = \"pifx\"\n");
    CHECK(t.size() == 2 && t[0].index == 42 && t[0].sink == 3 && t[0].app == "Firefox" && t[1].sink == 5);
}
