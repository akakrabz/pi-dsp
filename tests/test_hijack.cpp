// System audio routing against a fake `pactl` that keeps PipeWire's state in files:
// the sinks, the default sink, the playing streams, and a log of every call.
#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>

#include "check.h"
#include "core/Hijack.h"
#include "core/Util.h"

using namespace pifx;

static const char* kDac = "alsa_output.platform-soc_sound.stereo-fallback";

struct FakePactl {
    std::string dir, oldPath;
    FakePactl(bool permanentSink, const std::string& defaultSink = kDac) {
        char tmpl[] = "/tmp/pifx-pactl-XXXXXX";
        dir = mkdtemp(tmpl);
        std::string sinks = std::string("45\t") + kDac + "\tmodule-alsa-card.c\ts32le 2ch 48000Hz\tRUNNING\n";
        if (permanentSink) sinks += "60\tpi_dsp\tmodule-null-sink.c\tfloat32le 2ch 48000Hz\tIDLE\n";
        writeFile(dir + "/sinks", sinks);
        writeFile(dir + "/default", defaultSink + "\n");
        streams(45);
        writeFile(dir + "/log", "");
        const std::string D = dir;
        writeFile(dir + "/pactl",
                  "#!/bin/sh\n"
                  "D=" + D + "\n"
                  "echo \"$*\" >> $D/log\n"
                  "case \"$1\" in\n"
                  "  info) echo 'Server Name: PulseAudio (on PipeWire 1.2.7)'; echo \"Default Sink: $(cat $D/default)\";;\n"
                  "  get-default-sink) cat $D/default;;\n"
                  "  set-default-sink) echo \"$2\" > $D/default;;\n"
                  "  list) [ \"$2\" = short ] && [ \"$3\" = sinks ] && cat $D/sinks;;\n"
                  "  -f) cat $D/inputs.json;;\n"
                  "  load-module) printf '77\\tpifx_hijack\\tmodule-null-sink.c\\tfloat32le 2ch 48000Hz\\tIDLE\\n' >> $D/sinks;"
                  " echo 536870913;;\n"
                  "  unload-module) grep -v pifx_hijack $D/sinks > $D/s.tmp; mv $D/s.tmp $D/sinks;;\n"
                  "  move-sink-input) ;;\n"
                  "  *) exit 1;;\n"
                  "esac\n");
        chmod((dir + "/pactl").c_str(), 0755);
        oldPath = std::getenv("PATH") ? std::getenv("PATH") : "";
        setenv("PATH", (dir + ":" + oldPath).c_str(), 1);
    }
    ~FakePactl() {
        setenv("PATH", oldPath.c_str(), 1);
        runCapture("rm -rf " + shellQuote(dir));
    }
    // Firefox and pifx's own playback stream, both on sink `on`
    void streams(int on) {
        writeFile(dir + "/inputs.json",
                  format(R"([{"index":101,"sink":%d,"properties":{"application.name":"Firefox"}},)"
                         R"({"index":102,"sink":45,"properties":{"application.name":"pifx"}}])", on));
    }
    std::string def() const {
        std::string s;
        readFile(dir + "/default", s);
        return trim(s);
    }
    std::string log() const {
        std::string s;
        readFile(dir + "/log", s);
        return s;
    }
    std::string state() const { return dir + "/hijack.json"; }
};

TEST(hijack_uses_the_permanent_pi_dsp_output) {
    FakePactl pa(true);
    {
        Hijack h(pa.state());
        std::string err;
        CHECK(h.start(&err));
        CHECK(h.active() && h.permanent() && h.sink() == "pi_dsp" && h.monitor() == "pi_dsp.monitor");
        CHECK(pa.def() == "pi_dsp");
        CHECK(h.previousSink() == kDac);
        CHECK(pa.log().find("load-module") == std::string::npos);        // nothing created on the fly
        CHECK(pa.log().find("move-sink-input 101 pi_dsp") != std::string::npos);
        CHECK(pa.log().find("move-sink-input 102") == std::string::npos); // never our own playback
        CHECK(h.movedStreams() == 1);
        CHECK(fileExists(pa.state()));
        pa.streams(60);                                                   // Firefox now plays into pi_dsp
        h.stop();
        CHECK(!h.active());
        CHECK(pa.def() == kDac);
        CHECK(pa.log().find(std::string("move-sink-input 101 ") + kDac) != std::string::npos);
        CHECK(pa.log().find("unload-module") == std::string::npos);      // the permanent output stays
        CHECK(!fileExists(pa.state()));
    }
}

TEST(hijack_falls_back_to_a_temporary_sink) {
    FakePactl pa(false);
    Hijack h(pa.state());
    std::string err;
    CHECK(h.start(&err));
    CHECK(h.sink() == "pifx_hijack" && !h.permanent());
    CHECK(pa.log().find("load-module module-null-sink sink_name=pifx_hijack") != std::string::npos);
    CHECK(pa.def() == "pifx_hijack");
    h.stop();
    CHECK(pa.log().find("unload-module 536870913") != std::string::npos);
    CHECK(pa.def() == kDac);
}

TEST(hijack_never_restores_to_its_own_sink) {
    // WirePlumber remembered pi_dsp as the default from an earlier run.
    FakePactl pa(true, "pi_dsp");
    {
        Hijack h(pa.state());
        CHECK(h.start(nullptr, "alsa_output.usb-Volt"));
        CHECK(h.previousSink() == "alsa_output.usb-Volt");               // pifx's own output
    }
    {
        Hijack h(pa.state());
        pa.streams(45);
        writeFile(pa.dir + "/default", "pi_dsp\n");
        CHECK(h.start(nullptr, "pi_dsp"));                               // a virtual fallback is refused too
        CHECK(h.previousSink() == kDac);                                 // first real sink instead
        h.stop();
        CHECK(pa.def() == kDac);
    }
}

TEST(hijack_repair_after_a_crash) {
    FakePactl pa(true);
    std::string saved;
    {
        Hijack h(pa.state());
        CHECK(h.start(nullptr));
        readFile(pa.state(), saved);
    }                                                                     // (destructor restores)
    writeFile(pa.state(), saved);                                         // ...pretend it crashed instead
    writeFile(pa.dir + "/default", "pi_dsp\n");
    std::string msg = Hijack::repair(pa.state());
    CHECK(!msg.empty());
    CHECK(pa.def() == kDac);
    CHECK(!fileExists(pa.state()));
    CHECK(Hijack::repair(pa.state()).empty());                            // nothing left to do
}

TEST(hijack_virtual_sink_names) {
    for (const char* v : {"pi_dsp", "pi_dsp.monitor", "pifx_hijack", "pifx_hijack.monitor", "pi-dsp", "pifx-hijack"})
        CHECK(Hijack::isVirtualSink(v));
    for (const char* r : {kDac, "pi_dsp_extra", "alsa_output.pi", "BossDAC"}) CHECK(!Hijack::isVirtualSink(r));
}
