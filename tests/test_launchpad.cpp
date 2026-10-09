// Port of the Python Launchpad / pad map tests, plus the MIDI byte parser.
#include <unistd.h>

#include "check.h"
#include "core/Launchpad.h"
#include "core/PadMap.h"
#include "core/Util.h"

using namespace pifx;

TEST(lp_identify_models) {
    CHECK((identifyLaunchpad("Launchpad Mini MK3:Launchpad Mini MK3 LPMiniMK3 MIDI 20:1") == LpModel{"mk3", 0x0D}));
    CHECK((identifyLaunchpad("Launchpad X:Launchpad X LPX MIDI 24:1") == LpModel{"mk3", 0x0C}));
    CHECK((identifyLaunchpad("Launchpad Pro MK3:Launchpad Pro MK3 LPProMK3 MIDI 28:1") == LpModel{"mk3", 0x0E}));
    CHECK((identifyLaunchpad("Launchpad MK2:Launchpad MK2 MIDI 1 20:0") == LpModel{"mk2", 0x18}));
    CHECK((identifyLaunchpad("Launchpad Pro:Launchpad Pro MIDI 20:0") == LpModel{"pro1", 0x10}));
    CHECK((identifyLaunchpad("Launchpad S:Launchpad S MIDI 1 20:0") == LpModel{"legacy", -1}));
    CHECK(!identifyLaunchpad("Volt 1:Volt 1 MIDI 1 24:0"));
}

TEST(lp_pick_port_prefers_midi_over_daw) {
    std::vector<std::string> names = {"Midi Through:Midi Through Port-0 14:0", "Launchpad X:Launchpad X LPX DAW 24:0",
                                      "Launchpad X:Launchpad X LPX MIDI 24:1"};
    CHECK(pickLaunchpadPort(names) == names[2]);
    CHECK(pickLaunchpadPort({"Midi Through:Midi Through Port-0 14:0"}).empty());
    names = {"Launchpad Mini MK3:Launchpad Mini MK3 LPMiniMK3 DA 24:0", "Launchpad Mini MK3:Launchpad Mini MK3 LPMiniMK3 MI 24:1"};
    CHECK(pickLaunchpadPort(names) == names[1]);
    CHECK(pickLaunchpadPort({"Launchpad MK2:Launchpad MK2 MIDI 1 20:0"}) == "Launchpad MK2:Launchpad MK2 MIDI 1 20:0");
}

TEST(lp_mk3_layout_roundtrip) {
    LpLayout lay("mk3", 0x0D);
    CHECK(lay.enterMessages()[0] == Bytes({0xF0, 0x00, 0x20, 0x29, 0x02, 0x0D, 0x0E, 0x01, 0xF7}));
    auto d = [&](Bytes m) { return lay.decode(m); };
    auto is = [](std::optional<PadEvent> e, int x, int y, bool p) { return e && e->x == x && e->y == y && e->pressed == p; };
    CHECK(is(d({0x90, 11, 100}), 0, 0, true));
    CHECK(is(d({0x90, 88, 100}), 7, 7, true));
    CHECK(is(d({0x80, 45, 0}), 4, 3, false));
    CHECK(is(d({0x90, 45, 0}), 4, 3, false));
    CHECK(is(d({0xB0, 89, 127}), 8, 7, true));
    CHECK(is(d({0xB0, 19, 0}), 8, 0, false));
    CHECK(is(d({0xB0, 91, 127}), 0, 8, true));
    CHECK(is(d({0xB0, 98, 127}), 7, 8, true));
    CHECK(!d({0xB0, 50, 1}));
    CHECK(lay.led(0, 0, 21) == Bytes({0x90, 11, 21}));
    CHECK(lay.led(8, 7, 5) == Bytes({0xB0, 89, 5}));
    CHECK(lay.led(3, 8, 3) == Bytes({0xB0, 94, 3}));
}

TEST(lp_mk2_layout) {
    LpLayout lay("mk2", 0x18);
    auto e = lay.decode({0x90, 89, 100});
    CHECK(e && e->x == 8 && e->y == 7 && e->pressed);
    e = lay.decode({0xB0, 104, 127});
    CHECK(e && e->x == 0 && e->y == 8);
    CHECK(lay.led(8, 0, 5) == Bytes({0x90, 19, 5}));
    CHECK(lay.led(0, 8, 5) == Bytes({0xB0, 104, 5}));
}

TEST(lp_legacy_layout) {
    LpLayout lay("legacy", -1);
    auto e = lay.decode({0x90, 0, 127});
    CHECK(e && e->x == 0 && e->y == 7);
    e = lay.decode({0x90, 16 * 7 + 7, 127});
    CHECK(e && e->x == 7 && e->y == 0);
    e = lay.decode({0x90, 8, 127});
    CHECK(e && e->x == 8 && e->y == 7);
    auto m = lay.led(0, 7, palette("green"));
    CHECK(m[0] == 0x90 && m[1] == 0);
    CHECK(legacyColor(0) == 12);
    CHECK(legacyColor(palette("red")) == 12 + 3);
    CHECK(legacyColor(palette("green")) == 12 + 16 * 3);
}

TEST(lp_device_press_and_leds) {
    std::vector<Bytes> sent;
    std::vector<PadEvent> presses;
    LaunchpadDevice pad("Launchpad Mini MK3:LPMiniMK3 MIDI", [&](const Bytes& b) { sent.push_back(b); },
                        [&](const PadEvent& e) { presses.push_back(e); });
    CHECK(sent[0][6] == 0x0E && sent[0][7] == 0x01);        // programmer mode
    CHECK(sent.size() == 1 + 80);                           // then every pad cleared
    pad.onMidi({0x90, 81, 100});
    pad.onMidi({0x80, 81, 0});
    CHECK(presses.size() == 2 && presses[0].x == 0 && presses[0].y == 7 && presses[0].pressed && !presses[1].pressed);
    sent.clear();
    std::array<int, 81> c{};
    c[7 * 9 + 0] = 21;
    c[7 * 9 + 1] = 23;
    pad.setLeds(c);
    CHECK(sent.size() == 2 && sent[0] == Bytes({0x90, 81, 21}) && sent[1] == Bytes({0x90, 82, 23}));
    pad.setLeds(c);                                          // unchanged: nothing sent
    CHECK(sent.size() == 2);
    pad.close();
    CHECK(sent.back()[6] == 0x0E && sent.back()[7] == 0x00); // back to live mode
}

TEST(lp_midi_parser_running_status_and_sysex) {
    MidiParser p;
    std::vector<Bytes> got;
    const uint8_t stream[] = {0x90, 11, 100, 12, 0, 0xF8, 0xF0, 0x00, 0x20, 0xF7, 0xB0, 91, 127};
    p.feed(stream, 5, [&](const Bytes& m) { got.push_back(m); });
    p.feed(stream + 5, sizeof stream - 5, [&](const Bytes& m) { got.push_back(m); });
    CHECK(got.size() == 4);
    CHECK(got[0] == Bytes({0x90, 11, 100}));
    CHECK(got[1] == Bytes({0x90, 12, 0}));                  // running status
    CHECK(got[2] == Bytes({0xF0, 0x00, 0x20, 0xF7}));       // sysex (realtime byte skipped)
    CHECK(got[3] == Bytes({0xB0, 91, 127}));
}

TEST(padmap_default_covers_grid_and_colors) {
    json m = defaultPadMap();
    CHECK(m.size() == 80);
    for (int y = 0; y < 9; y++)
        for (int x = 0; x < 9; x++)
            if (!(x == 8 && y == 8)) CHECK(m.contains(padKey(x, y)));
    PadContext ctx;
    ctx.fxOn = {{"filter", true}, {"delay", false}};
    ctx.sourceKind = "tone";
    ctx.toneMode = 0;
    ctx.wave = "sine";
    ctx.freq = 440;
    ctx.preset = 3;
    ctx.volumeDb = -12;
    CHECK(padColor(m["0,7"], 0, 7, ctx) == palette("cyan"));
    CHECK(padColor(m["4,7"], 4, 7, ctx) == dim("blue"));
    CHECK(padColor(m["0,5"], 0, 5, ctx) == palette("sky"));
    CHECK(padColor(m["1,5"], 1, 5, ctx) == dim("sky"));
    CHECK(padColor(m["2,3"], 2, 3, ctx) == palette("yellow"));
    CHECK(padColor(m["4,0"], 4, 0, ctx) == palette("green"));
    CHECK(padColor(m["5,0"], 5, 0, ctx) == dim("green"));
    CHECK(padColor(m["8,7"], 8, 7, ctx) == dim("red"));
}

TEST(padmap_file_roundtrip) {
    char tmpl[] = "/tmp/pifx-pad-XXXXXX";
    std::string dir = mkdtemp(tmpl);
    std::string p = dir + "/padmap.json";
    json m = loadPadMap(p);
    CHECK(fileExists(p) && m == defaultPadMap());
    writeFile(p, R"({"0,0": {"action": "mute"}})");
    CHECK(loadPadMap(p) == json::parse(R"({"0,0": {"action": "mute"}})"));
    runCapture("rm -rf " + shellQuote(dir));
}

TEST(rawmidi_enumeration_does_not_crash) {
    std::string why;
    RawMidi::available(&why);
    auto ports = RawMidi::list();      // usually empty in CI; must simply not crash
    CHECK(ports.size() < 1000);
}
