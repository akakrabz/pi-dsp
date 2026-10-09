// The Rig: single source of truth for everything the UI and the Launchpad control.
//
// Lives on the UI thread. Every change goes through a method here, which updates the
// mirror state the UI draws from and posts a Command to the engine. The audio thread
// never sees this object. update() is called once per UI frame (or per tick in
// headless mode): it drains the scope tap into the History, handles Launchpad pads,
// pushes LED colours, frees retired sources and notices unplugged devices.
#pragma once
#include <array>
#include <chrono>
#include <deque>
#include <future>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "core/Analysis.h"
#include "core/Devices.h"
#include "core/Engine.h"
#include "core/Hat.h"
#include "core/Hijack.h"
#include "core/Launchpad.h"
#include "core/PadMap.h"

namespace pifx {

struct FxState {
    bool enabled = false;
    std::vector<float> values;
};

class Rig {
public:
    struct Options {
        std::string dataDir, mediaDir;
        int sampleRate = 48000;
        int block = 256;
        std::string backend = "auto";
        std::vector<std::string> outputs;   // device keys/names; "none" = scope only
        bool outputsGiven = false;
        std::string input;                  // capture device for the "input" source
        std::string source;                 // startup source: tone | shape | file[:name] | capture[:dev] | silence
        std::string preset;                 // load this preset after start
        bool hijack = false;                // route system audio through pifx from the start
        bool launchpad = true;
        std::string midiPort;
        bool audio = true;                  // false: timer clock only (tests)
    };

    explicit Rig(Options o);
    ~Rig();
    void start();
    void update();
    void shutdown();                        // stop audio, restore hijack, save settings

    // components
    Engine& engine() { return *engine_; }
    AudioIO& io() { return *io_; }
    History& history() { return history_; }
    Mixer& mixer() { return *mixer_; }
    const HatStatus& hat() const { return hat_; }
    LaunchpadManager* launchpad() { return lp_.get(); }
    const Options& options() const { return opt_; }

    // ---------------------------------------------------------------- effects
    const std::vector<FxState>& fx() const { return fx_; }
    void setParam(int fx, int param, float value);
    bool setParam(const std::string& fx, const std::string& param, const json& value);
    void setEnabled(int fx, bool on);
    bool bypassAll() const { return bypassAll_; }
    void setBypassAll(bool on);
    void allOff();
    void panic();
    void cycle(const std::string& fx, const std::string& param);
    float masterDb() const { return masterDb_; }
    void setMasterDb(float db);

    // ---------------------------------------------------------------- tempo
    void tap();
    float tempo() const { return tempo_; }
    void setTempo(float bpm);
    void delayDiv(double div);
    bool tapFlash() const;

    // ---------------------------------------------------------------- sources
    SourceKind sourceKind() const;
    const ToneParams& tone() const { return tone_; }
    void setTone(const ToneParams& p);      // also switches to the tone source
    void useSilence();
    std::vector<std::string> mediaFiles() const;
    const std::string& mediaDir() const { return opt_.mediaDir; }
    void playFile(const std::string& pathOrName);   // async decode; see loading()
    bool loading() const { return loading_.valid(); }
    const std::string& loadingName() const { return loadingName_; }
    void stepFile(int delta);
    FileSource* file();
    bool useCapture(const std::string& key, std::string* err);
    CaptureSource* capture();
    const std::string& captureKey() const { return captureKey_; }

    // ---------------------------------------------------------------- hijack / routing
    Hijack& hijack() { return hijack_; }
    bool startHijack(std::string* err);
    void stopHijack();
    bool hijackOnStart() const { return settings_.value("hijack_on_start", false); }
    void setHijackOnStart(bool on);
    // True for pifx's own virtual outputs (pi-dsp, pifx-hijack): playing to them would feed back.
    bool isVirtualOutput(const std::string& keyOrName) const;
    bool setOutputs(const std::vector<std::string>& keys, std::string* err);
    bool setBackend(const std::string& name, std::string* err);
    float outputGainDb() const { return outGainDb_; }
    void setOutputGainAll(float db);

    // ---------------------------------------------------------------- DAC / output level
    bool hasHat() const { return mixer_->ok(); }
    double volumeDb() const;
    void setVolumeDb(double db);
    void stepVolume(double delta);
    bool muted() const;
    void setMute(bool on);
    void rescanHat();

    // ---------------------------------------------------------------- presets
    std::vector<std::string> presets() const;
    bool savePreset(const std::string& name);
    bool loadPreset(const std::string& name);
    void deletePreset(const std::string& name);
    int currentPreset() const { return preset_; }
    json stateJson() const;                 // what a preset stores
    void applyStateJson(const json& j);

    // ---------------------------------------------------------------- pads
    const json& padMap() const { return padMap_; }
    std::array<int, 81> padColors() const;
    void padEvent(int x, int y, bool pressed);
    bool padHeld(int x, int y) const { return held_.count({x, y}) > 0; }
    std::string padLabel(int x, int y) const;

    // view requests coming from pads ("view" / "timebase" actions), consumed by the UI
    std::string view = "heat";
    double timebaseFactor = 1.0;            // UI multiplies its window by this, then resets to 1

    // ---------------------------------------------------------------- settings & messages
    json& settings() { return settings_; }
    void saveSettings();
    void notify(const std::string& msg);
    std::deque<std::pair<std::string, double>>& messages() { return messages_; }

private:
    void postFx(int fx, int param, float v);
    void setSourceKindMirror();
    void pressPad(int x, int y, const json& a);
    void releasePad(int x, int y, const json& a);
    std::string presetPath(const std::string& name) const;
    void chooseInitialRouting();

    Options opt_;
    std::unique_ptr<Engine> engine_;
    std::unique_ptr<AudioIO> io_;
    History history_;
    HatStatus hat_;
    std::unique_ptr<Mixer> mixer_;
    std::unique_ptr<LaunchpadManager> lp_;
    Hijack hijack_;
    json padMap_, settings_;

    std::vector<FxState> fx_;
    bool bypassAll_ = false;
    float masterDb_ = -6.0f;
    ToneParams tone_;
    float tempo_ = 120.0f;
    std::vector<double> taps_;
    int preset_ = 0;
    int fileIndex_ = 0;
    std::string captureKey_;
    float outGainDb_ = 0.0f;
    bool outMuted_ = false;

    std::future<std::shared_ptr<AudioClip>> loading_;
    std::string loadingName_, loadingErr_;
    std::shared_ptr<std::string> loadErr_;

    std::map<std::pair<int, int>, json> held_;
    std::vector<float> tapBuf_;
    std::deque<std::pair<std::string, double>> messages_;
    bool started_ = false;
};

double nowSeconds();

}  // namespace pifx
