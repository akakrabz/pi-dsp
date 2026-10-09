// PCM5122 DAC HAT detection and hardware volume (ALSA mixer via amixer).
//
// Targets the InnoMaker "HiFi DAC HAT" (PCM5122, dual oscillators, RCA + 3.5 mm) but
// works with any pcm512x board (HiFiBerry DAC+, Allo Boss, IQaudIO DAC+...). Detection
// only reads files under /proc, /sys and /boot, resolved through $PIFX_SYSROOT so the
// tests can point it at a fake tree.
#pragma once
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace pifx {

struct SoundCard {
    int index = -1;
    std::string id, driver, name;
    bool pcm512x = false;
    std::string hwParams;    // currently running params, if any
};

struct HatStatus {
    std::string piModel;               // empty = not a Pi
    bool isPi5 = false;
    std::vector<std::pair<std::string, std::string>> eeprom;
    std::string configPath;
    std::vector<std::string> overlays;
    std::string audioOverlay;          // the pcm512x overlay in use
    std::optional<bool> onboardAudio;  // dtparam=audio=...
    std::vector<std::string> modules;
    std::string i2cCodec;
    std::vector<SoundCard> cards;
    std::optional<SoundCard> card;     // the detected HAT
    bool detected = false;
    bool pipewire = false, pulseaudio = false;
    std::vector<std::string> issues, recommendations;

    std::string summary() const;
};

extern const char* const kRecommendedOverlay;   // allo-boss-dac-pcm512x-audio
extern const char* const kFallbackOverlay;      // hifiberry-dacplus

HatStatus detectHat();
std::string formatReport(const HatStatus& st);

// pieces (exposed for tests)
void parseConfigTxt(const std::string& text, std::vector<std::string>& overlays, std::optional<bool>& audio);
std::vector<SoundCard> parseAsoundCards(const std::string& text);

// ---------------------------------------------------------------- mixer
struct MixerControl {
    std::string name, kind;        // INTEGER / BOOLEAN / ENUMERATED
    int numid = 0;
    std::vector<std::string> values;   // raw strings ("207", "on", item name for enums)
    int vmin = 0, vmax = 0;
    std::optional<double> dbMin, dbStep;
    bool dbMuteAtMin = false;
    std::vector<std::string> items;

    std::optional<double> rawToDb(int raw) const;   // -inf when muted at min
    int dbToRaw(double db) const;
    std::vector<int> ints() const;
};
std::vector<MixerControl> parseAmixerContents(const std::string& text);

class Mixer {
public:
    static constexpr const char* kVolume = "Digital Playback Volume";
    static constexpr const char* kMute = "Digital Playback Switch";
    static constexpr const char* kAnalog = "Analogue Playback Volume";
    static constexpr const char* kDsp = "DSP Program";
    // Never push the PCM5122 past 0 dB: above raw 207 it applies digital gain and clips.
    static constexpr double kMaxDb = 0.0;

    virtual ~Mixer() = default;
    virtual bool ok() const = 0;
    virtual std::string error() const = 0;
    virtual int card() const = 0;
    virtual void refresh() {}
    virtual std::optional<double> volumeDb() const = 0;
    virtual std::optional<double> volumeDbMin() const = 0;
    virtual bool setVolumeDb(double db) = 0;
    virtual std::optional<bool> muted() const = 0;
    virtual bool setMute(bool m) = 0;
    virtual std::optional<double> analogDb() const = 0;
    virtual bool setAnalogDb(double db) = 0;
    virtual std::vector<std::string> dspPrograms() const = 0;
    virtual std::string dspProgram() const = 0;
    virtual bool setDspProgram(int index) = 0;
    virtual std::vector<std::string> controlNames() const = 0;
};

// Talks to `amixer -c N` (set PIFX_AMIXER to use another binary, e.g. a test stub).
class AmixerMixer : public Mixer {
public:
    explicit AmixerMixer(int card);
    bool ok() const override;
    std::string error() const override { return error_; }
    int card() const override { return card_; }
    void refresh() override;
    std::optional<double> volumeDb() const override;
    std::optional<double> volumeDbMin() const override;
    bool setVolumeDb(double db) override;
    std::optional<bool> muted() const override;
    bool setMute(bool m) override;
    std::optional<double> analogDb() const override;
    bool setAnalogDb(double db) override;
    std::vector<std::string> dspPrograms() const override;
    std::string dspProgram() const override;
    bool setDspProgram(int index) override;
    std::vector<std::string> controlNames() const override;
    const MixerControl* get(const std::string& name) const;
    bool setRaw(const std::string& name, const std::string& value);

private:
    std::string run(const std::string& args, int* status) const;
    int card_;
    std::string amixer_, error_;
    std::vector<MixerControl> controls_;
};

// Stand-in without a HAT (laptop, other DACs): remembers what you set.
class NullMixer : public Mixer {
public:
    bool ok() const override { return false; }
    std::string error() const override { return "no PCM5122 card"; }
    int card() const override { return -1; }
    std::optional<double> volumeDb() const override { return db_; }
    std::optional<double> volumeDbMin() const override { return -103.5; }
    bool setVolumeDb(double db) override { db_ = std::min(db, kMaxDb); return true; }
    std::optional<bool> muted() const override { return mute_; }
    bool setMute(bool m) override { mute_ = m; return true; }
    std::optional<double> analogDb() const override { return 0.0; }
    bool setAnalogDb(double) override { return true; }
    std::vector<std::string> dspPrograms() const override { return {}; }
    std::string dspProgram() const override { return ""; }
    bool setDspProgram(int) override { return false; }
    std::vector<std::string> controlNames() const override { return {}; }

private:
    double db_ = -12.0;
    bool mute_ = false;
};

std::unique_ptr<Mixer> mixerFor(const HatStatus& st);

}  // namespace pifx
