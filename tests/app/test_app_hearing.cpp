// App-level tests: Settings > Hearing (docs/11 E32 (c), E33) and Smart macros
// (docs/11 E34) in the app.
// * The hearing guard's settings: the listener's own headset sensitivity per
//   output endpoint (over the device profile's figure), the listening-level
//   cap and the daily dose, persisted per day and summed over the last 7
//   days; with no sensitivity nothing is estimated or applied and the page
//   says "unknown"; the system volume reaches the guard's estimate.
// * The per-ear profile (PersonalProfile): its own file next to the
//   settings, handed to every strip's chain on start and on each edit, and
//   heard in the sound (a mono tone through a strip comes out with the
//   profile's interaural difference).
// * The page itself (every control reachable, driving the controller), the
//   LoudnessPanel's dose row (only with a sensitivity) and the screenshot
//   states.
#include "AppTestSupport.h"

#include "engine/EngineController.h"
#include "shell/ScreenshotDriver.h"
#include "ui/HearingPage.h"
#include "ui/LoudnessPanel.h"
#include "ui/PersonalProfileEditor.h"
#include "ui/SettingsDialog.h"

#include "flub/engine/HearingGuard.h"
#include "flub/engine/PersonalProfile.h"

#include <cmath>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using namespace flub::app;
using flub::HearingGuard;
using flub::PersonalProfile;
using flub::platform::EndpointVolume;

namespace
{
constexpr const char* kHeadset = "Headset Earphone (Stealth 700 Gen 2 MAX)";

/** The system volume the fake endpoint reports (thread-safe). */
struct FakeVolume
{
    void set (float db)
    {
        const std::scoped_lock hold (lock);
        volumeDb = db;
    }
    EndpointVolume read()
    {
        const std::scoped_lock hold (lock);
        ++reads;
        return { true, volumeDb, false, {} };
    }
    int getReads()
    {
        const std::scoped_lock hold (lock);
        return reads;
    }

private:
    std::mutex lock;
    float volumeDb = 0.0f;
    int reads = 0;
};

/** A settable calendar (Options::clock). */
struct FakeClock
{
    juce::Time now { 2026, 8, 30, 15, 0 }; // 30 September 2026, 15:00 local
};

EngineController::Options optionsWith (const flubapptest::TempFolder& temp, const std::shared_ptr<FakeVolume>& volume,
                                       const std::shared_ptr<FakeClock>& clock = nullptr, bool persist = false)
{
    EngineController::Options o;
    o.openAudioDevice = false;
    o.restoreState = false;
    o.enableAppRouting = false;
    o.settingsFile = temp.file ("settings.xml");
    o.persistSettings = persist;
    o.foregroundAppFactory = [] { return std::unique_ptr<flub::platform::ForegroundApp>(); };
    o.endpointVolumeReader = [volume] (const std::string&) { return volume->read(); };
    if (clock != nullptr)
        o.clock = [clock] { return clock->now; };
    return o;
}

template <typename T>
T* findByTitle (juce::Component& root, const juce::String& title)
{
    for (auto* child : root.getChildren())
    {
        if (auto* t = dynamic_cast<T*> (child); t != nullptr && t->getTitle() == title)
            return t;
        if (auto* found = findByTitle<T> (*child, title))
            return found;
    }
    return nullptr;
}

/** `c` and every parent up to `root` are visible (no peer needed). */
bool visibleIn (juce::Component& root, juce::Component& c)
{
    for (auto* p = &c; p != nullptr && p != &root; p = p->getParentComponent())
        if (! p->isVisible())
            return false;
    return true;
}

juce::TextButton* findButton (juce::Component& root, const juce::String& text)
{
    for (auto* child : root.getChildren())
    {
        if (auto* b = dynamic_cast<juce::TextButton*> (child); b != nullptr && b->getButtonText() == text)
            return b;
        if (auto* found = findButton (*child, text))
            return found;
    }
    return nullptr;
}

/** A steady mono sine into one strip (every channel alike), the others silent. */
class SineSource final : public StripSignalSource
{
public:
    SineSource (int stripIndex, double freq, double sampleRate, float amplitude)
        : target (stripIndex), step (2.0 * juce::MathConstants<double>::pi * freq / sampleRate), amp (amplitude)
    {
    }

    bool renderStrip (int strip, const flub::AudioBlock& block) override
    {
        if (strip != target)
            return false;
        for (int i = 0; i < block.numSamples; ++i)
        {
            const float v = amp * static_cast<float> (std::sin (phase));
            for (int c = 0; c < block.numChannels; ++c)
                block.channel (c)[i] = v;
            phase += step;
        }
        return true;
    }

private:
    int target;
    double step, phase = 0.0;
    float amp;
};

/** Renders `seconds` of a sine into `strip`; returns the left and right
    output amplitudes (dB re the input) at the tone over the last half. */
std::pair<double, double> renderEarsDb (EngineController& c, int strip, double freq, double seconds, float amplitude)
{
    auto& host = c.getHost();
    const int block = host.getBlockSize();
    const int total = static_cast<int> (seconds * host.getSampleRate());
    std::vector<float> left (static_cast<size_t> (block)), right (static_cast<size_t> (block)), tailL, tailR;
    float* outs[] = { left.data(), right.data() };
    SineSource source (strip, freq, host.getSampleRate(), amplitude);
    for (int done = 0; done < total; done += block)
    {
        host.renderOffline (source, block, outs, 2);
        if (done >= total / 2)
        {
            tailL.insert (tailL.end(), left.begin(), left.end());
            tailR.insert (tailR.end(), right.begin(), right.end());
        }
    }
    const double w = 2.0 * juce::MathConstants<double>::pi * freq / host.getSampleRate();
    const auto amplitudeOf = [w, amplitude] (const std::vector<float>& x)
    {
        double re = 0.0, im = 0.0;
        for (size_t i = 0; i < x.size(); ++i)
        {
            re += x[i] * std::cos (w * static_cast<double> (i));
            im -= x[i] * std::sin (w * static_cast<double> (i));
        }
        const double a = 2.0 * std::sqrt (re * re + im * im) / static_cast<double> (x.size());
        return 20.0 * std::log10 (std::max (1.0e-12, a / amplitude));
    };
    return { amplitudeOf (tailL), amplitudeOf (tailR) };
}

/** Renders `seconds` of a sine into `strip` (no capture). */
void render (EngineController& c, int strip, double freq, double seconds, float amplitude)
{
    auto& host = c.getHost();
    SineSource source (strip, freq, host.getSampleRate(), amplitude);
    const int block = host.getBlockSize();
    const int total = static_cast<int> (seconds * host.getSampleRate());
    for (int done = 0; done < total; done += block)
        c.renderOffline (source, block);
}

std::unique_ptr<ui::SettingsDialog> makeDialog (EngineController& c)
{
    ui::HotkeyHooks hooks;
    hooks.isSupported = [] { return false; };
    hooks.getFailures = [] { return juce::StringArray(); };
    hooks.reRegister = [] {};
    auto dialog = std::make_unique<ui::SettingsDialog> (c, hooks, [] (ui::MeterPalette) {}, ui::MeterPalette::Standard);
    dialog->setSize (900, 700);
    return dialog;
}
} // namespace

// =============================================================================
// Settings
// =============================================================================
TEST_CASE ("App settings: the hearing keys persist - sensitivity per endpoint, the cap, 7 days of doses, Smart per strip (E32 (c) / E34)")
{
    const flubapptest::TempFolder temp;
    const auto file = temp.file ("settings.xml");
    flub::platform::OutputEndpointIdentity headset, speakers;
    headset.id = "{0.0.0.00000000}.{aaaa}";
    headset.name = kHeadset;
    speakers.name = "Speakers (Realtek(R) Audio)";
    {
        AppSettings settings (file, true);
        CHECK (! settings.findHearingSensitivity (headset).has_value());
        CHECK (! settings.getHearingCapEnabled());
        CHECK (settings.getHearingCapDbA() == 85.0f);
        CHECK (settings.getDailyDoses().empty());
        CHECK (! settings.getSmartMacros ("Music"));

        settings.setHearingSensitivity (headset, 104.5f);
        settings.setHearingSensitivity (speakers, 400.0f); // clamped to 150
        settings.setHearingCapEnabled (true);
        settings.setHearingCapDbA (75.0f);
        settings.setDailyDose ("2026-09-20", 0.5); // dropped: more than 6 days before the 30th
        settings.setDailyDose ("2026-09-28", 0.25);
        settings.setDailyDose ("2026-09-30", 0.125);
        settings.setSmartMacros ("Music", true);
        settings.save();
    }
    AppSettings settings (file, true);
    REQUIRE (settings.findHearingSensitivity (headset).has_value());
    CHECK (*settings.findHearingSensitivity (headset) == 104.5f);
    CHECK (*settings.findHearingSensitivity (speakers) == HearingGuard::kMaxSensitivityDbSpl);
    // The same endpoint by its id under another name (docs/11 E51 matching).
    auto renamed = headset;
    renamed.name = "Headphones (Stealth 700 Gen 2 MAX)";
    CHECK (settings.findHearingSensitivity (renamed).value_or (0.0f) == 104.5f);
    CHECK (settings.getHearingCapEnabled());
    CHECK (settings.getHearingCapDbA() == 75.0f);
    settings.setHearingCapDbA (20.0f);
    CHECK (settings.getHearingCapDbA() == HearingGuard::kCapMinDbA);
    const auto doses = settings.getDailyDoses();
    REQUIRE (doses.size() == 2);
    CHECK (doses[0].day == "2026-09-30"); // newest first
    CHECK (doses[0].fraction == 0.125);
    CHECK (doses[1].day == "2026-09-28");
    CHECK (settings.getSmartMacros ("Music"));
    CHECK (! settings.getSmartMacros ("Game"));

    settings.setHearingSensitivity (headset, std::nullopt); // back to the profile's
    CHECK (! settings.findHearingSensitivity (headset).has_value());
    CHECK (settings.findHearingSensitivity (speakers).has_value());
}

// =============================================================================
// Hearing guard wiring (docs/11 E32 (c))
// =============================================================================
TEST_CASE ("App: with no sensitivity nothing is estimated or applied, and Settings > Hearing says unknown (E32 (c))")
{
    const flubapptest::TempFolder temp;
    auto volume = std::make_shared<FakeVolume>();
    EngineController c (optionsWith (temp, volume));
    c.simulateOutputDevice (kHeadset, 48000.0, 2);

    // No shipped profile carries a sensitivity: unknown, even with the cap on.
    c.setHearingCap (true, 70.0f);
    const auto info = c.getHearing();
    CHECK (info.output == kHeadset);
    CHECK (! info.known);
    CHECK (info.source == HearingGuard::SensitivitySource::Unknown);
    CHECK (std::isnan (info.profileDbSpl));
    auto& guard = c.getHost().getMixEngine().getHearingGuard();
    CHECK (std::isnan (guard.getSensitivityDbSpl()));
    CHECK (volume->getReads() == 0); // no volume read for an estimate that does not exist

    const int music = c.findStrip ("Music");
    REQUIRE (music >= 0);
    const auto ears = renderEarsDb (c, music, 1000.0, 0.4, 0.5f); // loud: a cap at 70 would act if it could
    render (c, music, 1000.0, 0.1, 0.5f);
    CHECK (! guard.meters().known.load());
    CHECK (guard.meters().capGainDb.load() == 0.0f);
    CHECK (! guard.meters().capActive.load());
    c.pollHearingDose();
    CHECK (c.getHearing().doseToday == 0.0);
    CHECK (c.getSettings().getDailyDoses().empty()); // nothing stored for a day nothing was estimated on
    CHECK (std::abs (ears.first - ears.second) < 0.01);

    // The page: "unknown" everywhere, the cap's line says it does nothing.
    CHECK (ui::HearingPage::describeSensitivity (info).startsWith ("Unknown: the Turtle Beach"));
    CHECK (ui::HearingPage::describeSensitivity (info).contains ("nothing is estimated or applied"));
    CHECK (ui::HearingPage::describeEstimate (info).startsWith ("Unknown"));
    CHECK (ui::HearingPage::describeDose (info).startsWith ("Unknown"));
    CHECK (ui::HearingPage::describeCap (info).contains ("without one it does nothing"));

    // No output at all.
    EngineController::HearingInfo none;
    CHECK (ui::HearingPage::describeSensitivity (none).startsWith ("Unknown: no output device"));
}

TEST_CASE ("App: the listener's sensitivity switches the estimate on for its output, with the system volume; the cap holds the level (E32 (c))")
{
    const flubapptest::TempFolder temp;
    auto volume = std::make_shared<FakeVolume>();
    volume->set (-10.0f);
    EngineController c (optionsWith (temp, volume));
    CHECK (! c.setHearingSensitivity (100.0f)); // no output: nothing stored
    c.simulateOutputDevice (kHeadset, 48000.0, 2);

    REQUIRE (c.setHearingSensitivity (100.0f));
    auto info = c.getHearing();
    CHECK (info.known);
    CHECK (info.source == HearingGuard::SensitivitySource::User);
    CHECK (info.userDbSpl.value_or (0.0f) == 100.0f);
    auto& guard = c.getHost().getMixEngine().getHearingGuard();
    CHECK (guard.getSensitivityDbSpl() == 100.0f);
    CHECK (volume->getReads() >= 1);                  // read at once for the estimate
    CHECK (guard.getEndpointVolumeDb() == -10.0f);    // and handed to the guard
    CHECK (c.getHearing().volumeKnown);

    // A -20 dBFS 1 kHz tone (A-weighting 0 dB there): about 100 - 10 - 20 = 70 dB(A).
    const int music = c.findStrip ("Music");
    REQUIRE (music >= 0);
    const auto ears = renderEarsDb (c, music, 1000.0, 0.8, 0.1f);
    info = c.getHearing();
    REQUIRE (info.levelDbA > 0.0f);
    std::cerr << "    tone out " << ears.first << " dB re in, estimate " << info.levelDbA << " dB(A), 5 s " << info.leq5sDbA << "\n";
    CHECK_NEAR (info.levelDbA, 100.0 - 10.0 - 20.0 + ears.first, 0.5);
    CHECK (ui::HearingPage::describeEstimate (info).startsWith ("Estimated now "));
    CHECK (ui::HearingPage::describeSensitivity (info).startsWith ("Your figure for this output: 100.0 dB SPL"));

    // The cap: its setting reaches the guard, which turns the level down to it.
    c.setHearingCap (true, 60.0f);
    CHECK (guard.getCapEnabled());
    CHECK (guard.getCapDbA() == 60.0f);
    render (c, music, 1000.0, 2.0, 0.1f);
    info = c.getHearing();
    std::cerr << "    cap 60 dB(A): gain " << info.capGainDb << " dB, estimate " << info.levelDbA << " dB(A), 5 s " << info.leq5sDbA << "\n";
    CHECK (info.capActive);
    CHECK (info.capGainDb < -5.0f);
    CHECK (info.levelDbA < 61.0f);
    CHECK (ui::HearingPage::describeCap (info).contains ("Holding the level down now"));
    c.setHearingCap (false, 60.0f);
    CHECK (! guard.getCapEnabled());

    // Another output has no figure of its own: unknown again, the guard off.
    c.simulateOutputDevice ("Speakers (Realtek(R) Audio)", 48000.0, 2);
    CHECK (! c.getHearing().known);
    CHECK (std::isnan (guard.getSensitivityDbSpl()));
    // ... and back: the headset's figure returns with it.
    c.simulateOutputDevice (kHeadset, 48000.0, 2);
    CHECK (guard.getSensitivityDbSpl() == 100.0f);
    REQUIRE (c.setHearingSensitivity (std::nullopt));
    CHECK (std::isnan (guard.getSensitivityDbSpl()));
}

TEST_CASE ("App: the estimated dose is kept per day, survives a restart and starts afresh after midnight (E32 (c))")
{
    const flubapptest::TempFolder temp;
    auto volume = std::make_shared<FakeVolume>();
    auto clock = std::make_shared<FakeClock>();
    double firstDose = 0.0;
    {
        EngineController c (optionsWith (temp, volume, clock, true));
        c.simulateOutputDevice (kHeadset, 48000.0, 2);
        REQUIRE (c.setHearingSensitivity (130.0f)); // loud enough to count in a second
        CHECK (c.getDoseDay() == "2026-09-30");
        const int music = c.findStrip ("Music");
        render (c, music, 1000.0, 0.6, 0.5f);
        c.pollHearingDose();
        firstDose = c.getHearing().doseToday;
        CHECK (firstDose > 0.0);
        CHECK (std::abs (c.getHost().getMixEngine().getHearingGuard().meters().doseToday.load() - firstDose) < 1.0e-6 * std::max (1.0, firstDose));
        const auto stored = c.getSettings().getDailyDoses();
        REQUIRE (stored.size() == 1);
        CHECK (stored[0].day == "2026-09-30");
        CHECK_NEAR (stored[0].fraction, firstDose, 1.0e-6 * firstDose); // as the settings file stores a double
        CHECK (ui::HearingPage::describeDose (c.getHearing()).startsWith ("Today about "));
    } // shutdown stores the day's dose

    {
        // The same day after a restart: the stored dose is the baseline.
        EngineController c (optionsWith (temp, volume, clock, true));
        c.simulateOutputDevice (kHeadset, 48000.0, 2);
        CHECK (c.getHearing().known); // the figure is stored for the endpoint
        CHECK_NEAR (c.getHearing().doseToday, firstDose, 1.0e-9);
        const int music = c.findStrip ("Music");
        render (c, music, 1000.0, 0.6, 0.5f);
        c.pollHearingDose();
        const double sameDay = c.getHearing().doseToday;
        CHECK (sameDay > firstDose * 1.5);
        CHECK_NEAR (c.getHost().getMixEngine().getHearingGuard().meters().doseToday.load(), sameDay, 1.0e-6 * sameDay);

        // Midnight: the day's dose is final, the new day starts at 0; the
        // week counts both.
        clock->now = clock->now + juce::RelativeTime::hours (10);
        c.pollHearingDose();
        auto info = c.getHearing();
        CHECK (c.getDoseDay() == "2026-10-01");
        CHECK_NEAR (info.doseToday, 0.0, 1.0e-12);
        CHECK_NEAR (info.doseWeek, sameDay, 1.0e-6 * sameDay);
        auto stored = c.getSettings().getDailyDoses();
        REQUIRE (! stored.empty());
        CHECK (stored.back().day == "2026-09-30");
        CHECK_NEAR (stored.back().fraction, sameDay, 1.0e-6 * sameDay);
        render (c, music, 1000.0, 0.3, 0.5f);
        c.pollHearingDose();
        info = c.getHearing();
        CHECK (info.doseToday > 0.0);
        CHECK_NEAR (info.doseWeek, sameDay + info.doseToday, 1.0e-6 * info.doseWeek);
        stored = c.getSettings().getDailyDoses();
        REQUIRE (stored.size() == 2);
        CHECK (stored[0].day == "2026-10-01");

        // A week later the old day no longer counts.
        clock->now = clock->now + juce::RelativeTime::days (7);
        c.pollHearingDose();
        CHECK_NEAR (c.getHearing().doseWeek, 0.0, 1.0e-12);
    }
}

// =============================================================================
// Personal profile (docs/11 E33)
// =============================================================================
TEST_CASE ("App: an edited per-ear profile reaches every strip's chain and the sound - a fake-strip meter test (E33)")
{
    const flubapptest::TempFolder temp;
    auto volume = std::make_shared<FakeVolume>();
    EngineController c (optionsWith (temp, volume));
    const int music = c.findStrip ("Music");
    REQUIRE (music >= 0);
    constexpr float kQuiet = 0.02f; // -34 dBFS: no dynamics act

    // Nothing by default: the ears alike.
    CHECK (! c.getPersonalProfile().enabled);
    auto ears = renderEarsDb (c, music, 1000.0, 0.3, kQuiet);
    CHECK (std::abs (ears.second - ears.first) < 0.01);

    // Right ear +6 dB everywhere: a mono tone comes out 6 dB louder on the
    // right, on every strip's chain.
    PersonalProfile p;
    p.enabled = true;
    p.gainDb = { 0.0f, 6.0f };
    c.setPersonalProfile (p);
    for (int s = 0; s < c.getNumStrips(); ++s)
        CHECK (c.getChain (s).getPersonalProfile() == c.getPersonalProfile());
    ears = renderEarsDb (c, music, 1000.0, 0.3, kQuiet);
    std::cerr << "    +6 dB right: L " << ears.first << " dB, R " << ears.second << " dB; reservation "
              << c.getChain (music).getPersonalReservationDb() << " dB\n";
    CHECK_NEAR (ears.second - ears.first, 6.0, 0.2);

    // Right ear +9 dB at 4 kHz only: at 4 kHz the ears differ by 9 dB, at 250 Hz not.
    p.gainDb = { 0.0f, 0.0f };
    p.bandDb[1] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 9.0f, 0.0f, 0.0f };
    c.setPersonalProfile (p);
    ears = renderEarsDb (c, music, 4000.0, 0.3, kQuiet);
    std::cerr << "    +9 dB right at 4 kHz: R - L " << ears.second - ears.first << " dB at 4 kHz";
    CHECK_NEAR (ears.second - ears.first, 9.0, 0.3);
    ears = renderEarsDb (c, music, 250.0, 0.3, kQuiet);
    std::cerr << ", " << ears.second - ears.first << " dB at 250 Hz\n";
    CHECK_NEAR (ears.second - ears.first, 0.0, 0.3);

    // Balance: +6 turns the left ear down 6 dB (never boosts).
    p = {};
    p.enabled = true;
    p.balanceDb = 6.0f;
    c.setPersonalProfile (p);
    ears = renderEarsDb (c, music, 1000.0, 0.3, kQuiet);
    std::cerr << "    balance +6: L - R " << ears.first - ears.second << " dB\n";
    CHECK_NEAR (ears.first - ears.second, -6.0, 0.2);

    // Switched off: the ears alike again.
    p.enabled = false;
    c.setPersonalProfile (p);
    ears = renderEarsDb (c, music, 1000.0, 0.3, kQuiet);
    CHECK (std::abs (ears.second - ears.first) < 0.01);
}

TEST_CASE ("App: the personal profile lives in its own file, is loaded on start and survives preset loads (E33)")
{
    const flubapptest::TempFolder temp;
    auto volume = std::make_shared<FakeVolume>();
    PersonalProfile p;
    p.enabled = true;
    p.balanceDb = -2.0f;
    p.bandDb[1] = { 0.0f, 0.0f, 0.0f, 0.0f, 3.0f, 6.0f, 9.0f, 12.0f };
    {
        EngineController c (optionsWith (temp, volume, nullptr, true));
        CHECK (c.getPersonalProfileFile() == temp.file ("personal-profile.json"));
        CHECK (! c.getPersonalProfileFile().existsAsFile());
        c.setPersonalProfile (p);
        CHECK (c.savePersonalProfile());
        CHECK (c.getPersonalProfileFile().existsAsFile());
        PersonalProfile read;
        std::string error;
        REQUIRE (flub::personal::load (c.getPersonalProfileFile().getFullPathName().toStdString(), read, error));
        CHECK (read == p.sanitised());

        // A preset load never touches it (not a parameter).
        const auto factory = c.getPresetManager().getFactoryPresets();
        REQUIRE (! factory.empty());
        juce::String loadError;
        CHECK (c.loadPreset (factory.front(), c.getSelectedStrip(), loadError));
        CHECK (c.getPersonalProfile() == p.sanitised());
        CHECK (c.getChain (c.getSelectedStrip()).getPersonalProfile() == p.sanitised());
    }
    EngineController c (optionsWith (temp, volume, nullptr, true));
    CHECK (c.getPersonalProfileError().isEmpty());
    CHECK (c.getPersonalProfile() == p.sanitised());
    for (int s = 0; s < c.getNumStrips(); ++s)
        CHECK (c.getChain (s).getPersonalProfile() == p.sanitised());

    // A damaged file: the error is kept, the profile stays neutral.
    const flubapptest::TempFolder other;
    other.file ("personal-profile.json").replaceWithText ("{ \"format\": \"something else\" }");
    EngineController broken (optionsWith (other, volume));
    CHECK (broken.getPersonalProfileError().isNotEmpty());
    CHECK (broken.getPersonalProfile() == PersonalProfile {});
}

// =============================================================================
// The page, the dose row and the screenshot states
// =============================================================================
TEST_CASE ("App UI: Settings > Hearing - every control reachable, driving the controller and persisted (E32 (c) / E33)")
{
    const flubapptest::TempFolder temp;
    auto volume = std::make_shared<FakeVolume>();
    EngineController c (optionsWith (temp, volume, nullptr, true));
    c.simulateOutputDevice (kHeadset, 48000.0, 2);
    auto dialog = makeDialog (c);

    auto* nav = findButton (*dialog, "Hearing");
    REQUIRE (nav != nullptr);
    nav->triggerClick();
    REQUIRE (flubapptest::pumpMessagesUntil ([&] { return nav->getToggleState(); }, 2000));

    auto* own = findByTitle<juce::ToggleButton> (*dialog, "Own sensitivity figure");
    auto* sensitivity = findByTitle<juce::Slider> (*dialog, "Headset sensitivity");
    auto* cap = findByTitle<juce::ToggleButton> (*dialog, "Listening-level cap");
    auto* capLevel = findByTitle<juce::Slider> (*dialog, "Listening-level cap level");
    auto* profileOn = findByTitle<juce::ToggleButton> (*dialog, "Personal profile");
    auto* balance = findByTitle<juce::Slider> (*dialog, "Balance");
    auto* right4k = findByTitle<juce::Slider> (*dialog, "Right 4 kHz");
    auto* leftGain = findByTitle<juce::Slider> (*dialog, "Left gain");
    auto* page = findByTitle<ui::HearingPage> (*dialog, "Hearing");
    for (auto* control : std::initializer_list<juce::Component*> { own, sensitivity, cap, capLevel, profileOn, balance, right4k, leftGain, page })
    {
        REQUIRE (control != nullptr);
        CHECK (visibleIn (*dialog, *control)); // in the page's scrolling view
    }
    CHECK (page->getHeight() > dialog->getHeight()); // the editor below the guard: the page scrolls
    // Unknown at first: the switch off, the value greyed (the starting figure).
    CHECK (! own->getToggleState());
    CHECK (! sensitivity->isEnabled());
    CHECK (page->getSensitivityText().startsWith ("Unknown"));
    CHECK (page->getEstimateText().startsWith ("Unknown"));
    CHECK (page->getDoseText().startsWith ("Unknown"));

    // Own figure: typed into the value box, switched on.
    own->setToggleState (true, juce::sendNotificationSync);
    CHECK (c.getHearing().known);
    CHECK (sensitivity->isEnabled());
    sensitivity->setValue (104.0, juce::sendNotificationSync);
    CHECK (c.getHearing().userDbSpl.value_or (0.0f) == 104.0f);
    CHECK (c.getHost().getMixEngine().getHearingGuard().getSensitivityDbSpl() == 104.0f);
    dialog->showPage (ui::SettingsDialog::Page::Hearing); // refresh
    CHECK (page->getSensitivityText().startsWith ("Your figure for this output: 104.0 dB SPL"));

    // The cap.
    CHECK (! capLevel->isEnabled());
    cap->setToggleState (true, juce::sendNotificationSync);
    CHECK (c.getSettings().getHearingCapEnabled());
    CHECK (capLevel->isEnabled());
    capLevel->setValue (78.0, juce::sendNotificationSync);
    CHECK (c.getSettings().getHearingCapDbA() == 78.0f);
    CHECK (c.getHost().getMixEngine().getHearingGuard().getCapDbA() == 78.0f);

    // The per-ear editor: manual entry through the value boxes' sliders.
    profileOn->setToggleState (true, juce::sendNotificationSync);
    right4k->setValue (7.5, juce::sendNotificationSync);
    balance->setValue (-3.0, juce::sendNotificationSync);
    leftGain->setValue (2.0, juce::sendNotificationSync);
    const auto& p = c.getPersonalProfile();
    CHECK (p.enabled);
    CHECK (p.bandDb[1][5] == 7.5f);
    CHECK (p.balanceDb == -3.0f);
    CHECK (p.gainDb[0] == 2.0f);
    CHECK (c.getChain (c.getSelectedStrip()).getPersonalProfile() == p);
    CHECK (right4k->isTextBoxEditable());
    auto* flat = findButton (*dialog, "Flat");
    REQUIRE (flat != nullptr);
    flat->triggerClick();
    REQUIRE (flubapptest::pumpMessagesUntil ([&] { return c.getPersonalProfile().bandDb[1][5] == 0.0f; }, 2000));
    CHECK (c.getPersonalProfile().enabled); // Flat keeps the switch
    CHECK (right4k->getValue() == 0.0);

    // Persisted: a new dialog on a new controller shows the same state.
    right4k->setValue (4.0, juce::sendNotificationSync);
    dialog.reset();
    c.shutdown();
    EngineController again (optionsWith (temp, volume, nullptr, true));
    again.simulateOutputDevice (kHeadset, 48000.0, 2);
    auto reopened = makeDialog (again);
    reopened->showPage (ui::SettingsDialog::Page::Hearing);
    CHECK (findByTitle<juce::ToggleButton> (*reopened, "Own sensitivity figure")->getToggleState());
    CHECK (findByTitle<juce::Slider> (*reopened, "Headset sensitivity")->getValue() == 104.0);
    CHECK (findByTitle<juce::ToggleButton> (*reopened, "Listening-level cap")->getToggleState());
    CHECK (findByTitle<juce::Slider> (*reopened, "Listening-level cap level")->getValue() == 78.0);
    CHECK (findByTitle<juce::ToggleButton> (*reopened, "Personal profile")->getToggleState());
    CHECK (findByTitle<juce::Slider> (*reopened, "Right 4 kHz")->getValue() == 4.0);
}

TEST_CASE ("App UI: the page's texts - whose sensitivity, 'manufacturer figure, not lab-verified', the dose, the profile's caps (E32 (c) / E33)")
{
    EngineController::HearingInfo info;
    info.output = kHeadset;
    info.profileName = "Turtle Beach Stealth series";
    info.profileDbSpl = 108.0f;
    info.sensitivityDbSpl = 108.0f;
    info.source = HearingGuard::SensitivitySource::Profile;
    info.known = true;
    CHECK (ui::HearingPage::describeSensitivity (info)
           == "From the device profile: Turtle Beach Stealth series: 108.0 dB SPL at full volume (manufacturer figure, not lab-verified). "
              "Switch on your own figure to correct it.");
    info.profileLabVerified = true;
    CHECK (ui::HearingPage::describeSensitivity (info).contains ("(measured in the device lab)"));
    info.profileLabVerified = false;
    info.userDbSpl = 101.0f;
    info.sensitivityDbSpl = 101.0f;
    info.source = HearingGuard::SensitivitySource::User;
    CHECK (ui::HearingPage::describeSensitivity (info).startsWith ("Your figure for this output: 101.0 dB SPL at full volume. The profile's: "));
    CHECK (ui::HearingPage::describeSensitivity (info).contains ("manufacturer figure, not lab-verified"));

    info.doseToday = 0.12;
    info.doseWeek = 1.25;
    const auto dose = ui::HearingPage::describeDose (info);
    CHECK (dose.startsWith ("Today about 12 % of the weekly allowance, the last 7 days 125 %"));
    CHECK (dose.contains ("80 dB(A) for 40 hours a week"));
    CHECK (dose.contains ("more than the reference allowance"));
    CHECK (ui::HearingPage::formatDosePercent (0.0) == "0.0 %");
    CHECK (ui::HearingPage::formatDosePercent (0.045) == "4.5 %");
    CHECK (ui::HearingPage::formatDosePercent (0.5) == "50 %");

    // The per-ear editor's line.
    PersonalProfile p;
    CHECK (ui::PersonalProfileEditor::describeProfile (p, 0.0f).startsWith ("Off:"));
    p.enabled = true;
    CHECK (ui::PersonalProfileEditor::describeProfile (p, 0.0f).startsWith ("Flat:"));
    p.gainDb = { 0.0f, 12.0f };
    p.bandDb[1] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 15.0f, 15.0f, 15.0f }; // +27 asked: capped at +15, and 12 dB from the left
    const auto text = ui::PersonalProfileEditor::describeProfile (p, -9.0f);
    CHECK (text.startsWith ("Left ear 0.0 dB; right ear +12.0 dB"));
    CHECK (text.contains ("Capped: each ear at most +15 dB, the ears at most 12 dB apart."));
    CHECK (text.contains ("Both ears are turned down 9.0 dB"));
}

TEST_CASE ("App UI: the LoudnessPanel shows the dose row only while a sensitivity is known (E32 (c))")
{
    ui::LoudnessPanel panel;
    panel.setSize (300, 620);
    juce::Image image (juce::Image::ARGB, 300, 620, true);
    {
        juce::Graphics g (image);
        panel.paint (g);
    }
    CHECK (! panel.isShowingDose());

    ui::LoudnessPanel::HearingReadout readout;
    readout.known = true;
    readout.levelDbA = 72.0f;
    readout.doseToday = 0.12;
    readout.capOn = true;
    panel.setHearing (readout);
    {
        juce::Graphics g (image);
        panel.paint (g);
    }
    CHECK (panel.isShowingDose());
    CHECK (ui::LoudnessPanel::formatDose (0.12) == "12 %");
    CHECK (ui::LoudnessPanel::formatDose (0.045) == "4.5 %");

    // From the controller: unknown without a sensitivity, known with one; the
    // source MainComponent sets feeds update().
    const flubapptest::TempFolder temp;
    auto volume = std::make_shared<FakeVolume>();
    EngineController c (optionsWith (temp, volume));
    c.simulateOutputDevice (kHeadset, 48000.0, 2);
    CHECK (! ui::LoudnessPanel::hearingReadoutOf (c).known);
    c.setHearingSensitivity (100.0f);
    CHECK (ui::LoudnessPanel::hearingReadoutOf (c).known);
    ui::LoudnessPanel fed;
    fed.setSize (300, 620);
    fed.hearingSource = [&c] { return ui::LoudnessPanel::hearingReadoutOf (c); };
    fed.update (ui::MeterSnapshot {}, 0.05);
    {
        juce::Graphics g (image);
        fed.paint (g);
    }
    CHECK (fed.isShowingDose());
}

TEST_CASE ("App: the screenshot driver knows the hearing states (E32 (c) / E33)")
{
    ScreenshotDriver::Options o;
    juce::String error;
    CHECK (ScreenshotDriver::parseCommandLine (
        juce::StringArray ({ "--screenshot", "a.png", "--state", "settings-hearing,hearing-readout,hearing-profile,module-keys" }), o, error));
    CHECK (o.states.size() == 4);
    ScreenshotDriver::Options u;
    CHECK (ScreenshotDriver::parseCommandLine (juce::StringArray ({ "--screenshot", "a.png", "--state", "settings-hearing-unknown" }), u, error));
    ScreenshotDriver::Options bad;
    CHECK (! ScreenshotDriver::parseCommandLine (juce::StringArray ({ "--screenshot", "a.png", "--state", "settings-hear" }), bad, error));
}
