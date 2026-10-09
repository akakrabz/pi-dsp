// Port of the Python HAT tests: detection against a fake /proc tree, amixer parsing,
// and the mixer driving a stub amixer.
#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>

#include "check.h"
#include "core/Hat.h"
#include "core/Util.h"

using namespace pifx;

static const char* ASOUND_CARDS =
    " 0 [vc4hdmi0       ]: vc4-hdmi - vc4-hdmi-0\n"
    "                      vc4-hdmi-0\n"
    " 1 [vc4hdmi1       ]: vc4-hdmi - vc4-hdmi-1\n"
    "                      vc4-hdmi-1\n"
    " 2 [BossDAC        ]: BossDAC - BossDAC\n"
    "                      BossDAC\n";

static const char* AMIXER =
    "numid=1,iface=MIXER,name='DSP Program'\n"
    "  ; type=ENUMERATED,access=rw------,values=1,items=5\n"
    "  ; Item #0 'FIR interpolation with de-emphasis'\n"
    "  ; Item #1 'Low latency IIR with de-emphasis'\n"
    "  ; Item #2 'High attenuation with de-emphasis'\n"
    "  ; Item #3 'Fixed process flow'\n"
    "  ; Item #4 'Ringing-less low latency FIR'\n"
    "  : values=0\n"
    "numid=3,iface=MIXER,name='Analogue Playback Volume'\n"
    "  ; type=INTEGER,access=rw---R--,values=2,min=0,max=1,step=0\n"
    "  : values=1,1\n"
    "  | dBscale-min=-6.00dB,step=6.00dB,mute=0\n"
    "numid=5,iface=MIXER,name='Digital Playback Switch'\n"
    "  ; type=BOOLEAN,access=rw------,values=2\n"
    "  : values=on,on\n"
    "numid=2,iface=MIXER,name='Digital Playback Volume'\n"
    "  ; type=INTEGER,access=rw---R--,values=2,min=0,max=255,step=0\n"
    "  : values=207,207\n"
    "  | dBscale-min=-103.50dB,step=0.50dB,mute=1\n";

struct FakeRoot {
    std::string dir;
    FakeRoot(bool overlay = true, bool card = true, bool pi5 = true, bool eeprom = true) {
        char tmpl[] = "/tmp/pifx-hat-XXXXXX";
        dir = mkdtemp(tmpl);
        auto w = [&](const std::string& rel, const std::string& text) {
            std::string p = dir + rel;
            makeDirs(parentDir(p));
            writeFile(p, text);
        };
        if (pi5) w("/proc/device-tree/model", std::string("Raspberry Pi 5 Model B Rev 1.0") + '\0');
        if (eeprom) {
            w("/proc/device-tree/hat/vendor", std::string("InnoMaker") + '\0');
            w("/proc/device-tree/hat/product", std::string("HiFi DAC HAT") + '\0');
        }
        std::string cfg = "# config\ndtparam=audio=on\n";
        if (overlay) cfg += "dtoverlay=allo-boss-dac-pcm512x-audio  # DAC\n";
        cfg += "dtoverlay=vc4-kms-v3d\n";
        w("/boot/firmware/config.txt", cfg);
        w("/proc/modules", "snd_soc_pcm512x_i2c 12288 1 - Live 0x0\nsnd_soc_pcm512x 40960 1 snd_soc_pcm512x_i2c, Live\n"
                           "snd_soc_allo_boss_dac 16384 0 - Live\n");
        w("/sys/bus/i2c/devices/1-004d/name", "pcm5122\n");
        std::string cards = ASOUND_CARDS;
        if (!card) cards = cards.substr(0, cards.find(" 2 ["));
        w("/proc/asound/cards", cards);
        w("/proc/asound/card2/pcm0p/sub0/hw_params", "access: MMAP_INTERLEAVED\nformat: S32_LE\nrate: 48000 (48000/1)\n");
        setenv("PIFX_SYSROOT", dir.c_str(), 1);
    }
    ~FakeRoot() {
        unsetenv("PIFX_SYSROOT");
        runCapture("rm -rf " + shellQuote(dir));
    }
};

TEST(hat_detects_boss_dac_on_pi5) {
    FakeRoot root;
    HatStatus st = detectHat();
    CHECK(st.detected && st.card && st.card->id == "BossDAC" && st.card->index == 2);
    CHECK(st.isPi5);
    CHECK(st.audioOverlay == "allo-boss-dac-pcm512x-audio");
    CHECK(st.onboardAudio && *st.onboardAudio);
    CHECK(st.i2cCodec == "1-004d: pcm5122");
    CHECK(st.modules.size() == 3);
    CHECK(st.card->hwParams.find("S32_LE") != std::string::npos);
    CHECK(formatReport(st).find("RESULT       : OK") != std::string::npos);
}

TEST(hat_recommends_overlay_when_missing) {
    FakeRoot root(false, false);
    HatStatus st = detectHat();
    CHECK(!st.detected);
    CHECK(!st.recommendations.empty() && st.recommendations[0].find("dtoverlay=allo-boss-dac-pcm512x-audio") != std::string::npos);
}

TEST(hat_overlay_but_no_card_hints_pi5_slave) {
    FakeRoot root(true, false);
    HatStatus st = detectHat();
    CHECK(!st.detected);
    bool slave = false, other = false;
    for (const auto& r : st.recommendations) {
        slave |= r.find(",slave") != std::string::npos;
        other |= r.find("hifiberry-dacplus") != std::string::npos;
    }
    CHECK(slave && other);
}

TEST(hat_not_a_pi) {
    FakeRoot root(false, false, false, false);
    HatStatus st = detectHat();
    CHECK(!st.detected && st.piModel.empty());
    CHECK(st.issues.size() == 1 && st.issues[0].find("Not running on a Raspberry Pi") != std::string::npos);
}

TEST(hat_parse_config_variants) {
    std::vector<std::string> ov;
    std::optional<bool> audio;
    parseConfigTxt("dtparam=i2c_arm=on,audio=off\n#dtoverlay=nope\n  dtoverlay = hifiberry-dacplus,slave # c\n", ov, audio);
    CHECK(ov.size() == 1 && ov[0] == "hifiberry-dacplus,slave");
    CHECK(audio && !*audio);
}

TEST(hat_parse_amixer_and_db_mapping) {
    auto c = parseAmixerContents(AMIXER);
    CHECK(c.size() == 4);
    const MixerControl* vol = nullptr;
    for (const auto& x : c)
        if (x.name == "Digital Playback Volume") vol = &x;
    CHECK(vol != nullptr);
    if (!vol) return;
    CHECK(vol->vmax == 255 && vol->ints() == std::vector<int>({207, 207}));
    CHECK_NEAR(*vol->rawToDb(207), 0.0, 1e-9);
    CHECK(std::isinf(*vol->rawToDb(0)));
    CHECK(vol->dbToRaw(-12) == 183);
    CHECK(c[0].items.size() == 5 && c[0].values[0] == "FIR interpolation with de-emphasis");
}

TEST(hat_mixer_uses_amixer_and_caps_at_0db) {
    char tmpl[] = "/tmp/pifx-amixer-XXXXXX";
    std::string dir = mkdtemp(tmpl);
    std::string state = dir + "/contents", log = dir + "/log", stub = dir + "/amixer";
    writeFile(state, AMIXER);
    // stub: `contents` prints the state, `cset numid=2 V,V` rewrites the volume line
    writeFile(stub, "#!/bin/sh\necho \"$@\" >> " + log + "\n"
                    "if [ \"$3\" = contents ]; then cat " + state + "; exit 0; fi\n"
                    "if [ \"$3\" = cset ] && [ \"$4\" = numid=2 ]; then\n"
                    "  awk -v v=\"$5\" 'BEGIN{n=0} /name=.Digital Playback Volume./{n=1} n==1 && /: values=/{sub(/values=.*/, \"values=\" v); n=0} {print}' " +
                    state + " > " + state + ".t && mv " + state + ".t " + state + "\n"
                    "fi\nexit 0\n");
    chmod(stub.c_str(), 0755);
    setenv("PIFX_AMIXER", stub.c_str(), 1);
    AmixerMixer mx(2);
    CHECK(mx.ok());
    CHECK_NEAR(*mx.volumeDb(), 0.0, 1e-9);
    mx.setVolumeDb(+6.0);                    // capped to 0 dB -> raw 207
    std::string l;
    readFile(log, l);
    CHECK(l.find("-c 2 cset numid=2 207,207") != std::string::npos);
    mx.setVolumeDb(-20.0);
    CHECK_NEAR(*mx.volumeDb(), -20.0, 1e-9);
    CHECK(mx.dspPrograms().size() == 5);
    unsetenv("PIFX_AMIXER");
    runCapture("rm -rf " + shellQuote(dir));
}

TEST(hat_null_mixer) {
    NullMixer m;
    CHECK(!m.ok());
    m.setVolumeDb(5);
    CHECK(*m.volumeDb() == 0.0);
    m.setMute(true);
    CHECK(*m.muted());
}
