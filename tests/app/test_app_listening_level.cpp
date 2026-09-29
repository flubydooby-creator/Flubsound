// App-level tests: the loudness contour follows the OS output volume (docs/11
// E32). EngineController reads the output endpoint's volume through a fake
// reader (Options::endpointVolumeReader, the stand-in for
// platform::AudioEndpoints::queryOutputVolume, whose Linux path
// test_app_endpoint_volume.cpp runs against a fake pactl) and hands every
// strip's chain the volume minus the user's reference volume
// (ProcessingChain::setListeningLevelDb). Off by default; the Settings >
// Processing text and the persisted keys are checked here too.
#include "AppTestSupport.h"

#include "engine/EngineController.h"
#include "ui/SettingsDialog.h"

#include "flub/engine/Parameters.h"

#include <atomic>
#include <cmath>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using namespace flub::app;
using flub::platform::EndpointVolume;

namespace
{
/** What the fake endpoint reports; thread-safe (the background poll reads it). */
struct FakeEndpoint
{
    void set (bool known, float volumeDb, bool muted = false, const std::string& error = {})
    {
        const std::scoped_lock hold (lock);
        volume = { known, volumeDb, muted, error };
    }

    EndpointVolume read (const std::string& device)
    {
        const std::scoped_lock hold (lock);
        ++reads;
        devices.push_back (device);
        return volume;
    }

    int getReads()
    {
        const std::scoped_lock hold (lock);
        return reads;
    }

    std::string lastDevice()
    {
        const std::scoped_lock hold (lock);
        return devices.empty() ? std::string() : devices.back();
    }

private:
    std::mutex lock;
    EndpointVolume volume { true, 0.0f, false, {} };
    int reads = 0;
    std::vector<std::string> devices;
};

EngineController::Options optionsWith (const flubapptest::TempFolder& temp, const std::shared_ptr<FakeEndpoint>& endpoint, bool persist = false)
{
    EngineController::Options o;
    o.openAudioDevice = false;
    o.restoreState = false;
    o.enableAppRouting = false;
    o.settingsFile = temp.file ("settings.xml");
    o.persistSettings = persist;
    o.foregroundAppFactory = [] { return std::unique_ptr<flub::platform::ForegroundApp>(); };
    o.endpointVolumeReader = [endpoint] (const std::string& device) { return endpoint->read (device); };
    return o;
}

/** The first descendant of type T whose title is `title`. */
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

/** Every strip's chain at `db` (the listening level handed over). */
bool allChainsAt (EngineController& c, float db)
{
    for (int s = 0; s < c.getNumStrips(); ++s)
        if (std::abs (c.getChain (s).getListeningLevelDb() - db) > 1.0e-4f)
            return false;
    return true;
}

/** A steady sine into one strip (the others silent). */
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

/** Output amplitude of a steady sine through the whole engine, dB re the
    input, over the last half of `seconds`. */
double renderedGainDb (EngineController& controller, int strip, double freq, double seconds = 0.6)
{
    auto& host = controller.getHost();
    const int block = host.getBlockSize();
    const int total = static_cast<int> (seconds * host.getSampleRate());
    std::vector<float> left (static_cast<size_t> (block)), right (static_cast<size_t> (block)), tail;
    float* outs[] = { left.data(), right.data() };
    constexpr float kAmplitude = 0.02f; // -34 dBFS: no limiter or protection acts
    SineSource source (strip, freq, host.getSampleRate(), kAmplitude);
    for (int done = 0; done < total; done += block)
    {
        host.renderOffline (source, block, outs, 2);
        if (done >= total / 2)
            tail.insert (tail.end(), left.begin(), left.end());
    }
    double re = 0.0, im = 0.0;
    const double w = 2.0 * juce::MathConstants<double>::pi * freq / host.getSampleRate();
    for (size_t i = 0; i < tail.size(); ++i)
    {
        re += tail[i] * std::cos (w * static_cast<double> (i));
        im -= tail[i] * std::sin (w * static_cast<double> (i));
    }
    const double amplitude = 2.0 * std::sqrt (re * re + im * im) / static_cast<double> (tail.size());
    return 20.0 * std::log10 (std::max (1.0e-12, amplitude / kAmplitude));
}
} // namespace

TEST_CASE ("App: the contour follows the system volume only when switched on, relative to the reference volume (E32)")
{
    const flubapptest::TempFolder temp;
    auto endpoint = std::make_shared<FakeEndpoint>();
    endpoint->set (true, -20.0f);
    EngineController c (optionsWith (temp, endpoint));

    // Off by default: nothing is read, every chain plays at 0 dB.
    CHECK (! c.getContourFollowsVolume());
    CHECK (! c.getSettings().getContourReferenceVolumeDb().has_value());
    CHECK (endpoint->getReads() == 0);
    c.pollEndpointVolume(); // a read while off changes nothing
    CHECK (allChainsAt (c, 0.0f));
    CHECK (c.getListeningLevel().levelDb == 0.0f);

    // On: the volume now becomes the reference, so switching it on changes nothing.
    c.setContourFollowsVolume (true);
    CHECK (c.getContourFollowsVolume());
    REQUIRE (c.getSettings().getContourReferenceVolumeDb().has_value());
    CHECK (*c.getSettings().getContourReferenceVolumeDb() == -20.0f);
    CHECK (allChainsAt (c, 0.0f));

    // Turned down 15 dB: every strip's contour plays 15 dB below the reference.
    endpoint->set (true, -35.0f);
    c.pollEndpointVolume();
    CHECK (allChainsAt (c, -15.0f));
    auto level = c.getListeningLevel();
    CHECK (level.following);
    CHECK (level.known);
    CHECK (level.volumeDb == -35.0f);
    CHECK (level.levelDb == -15.0f);

    // A reference set by hand, then "use current volume".
    c.setContourReferenceVolumeDb (-30.0f);
    CHECK (allChainsAt (c, -5.0f));
    endpoint->set (true, -42.0f);
    CHECK (c.useCurrentVolumeAsReference());
    CHECK (*c.getSettings().getContourReferenceVolumeDb() == -42.0f);
    CHECK (allChainsAt (c, 0.0f));
    endpoint->set (true, -52.0f);
    c.pollEndpointVolume();
    CHECK (allChainsAt (c, -10.0f));

    // A muted endpoint reads its volume as usual; a failed read holds the
    // last volume of the same output and says why.
    endpoint->set (true, -52.0f, true);
    c.pollEndpointVolume();
    CHECK (c.getListeningLevel().muted);
    CHECK (allChainsAt (c, -10.0f));
    endpoint->set (false, 0.0f, false, "Connection failure: Connection refused");
    c.pollEndpointVolume();
    level = c.getListeningLevel();
    CHECK (! level.known);
    CHECK (level.error == "Connection failure: Connection refused");
    CHECK (allChainsAt (c, -10.0f));
    CHECK (! c.useCurrentVolumeAsReference()); // nothing to take: the reference stays
    CHECK (*c.getSettings().getContourReferenceVolumeDb() == -42.0f);

    // Off: 0 dB again; the reference is kept for the next time.
    c.setContourFollowsVolume (false);
    CHECK (allChainsAt (c, 0.0f));
    CHECK (*c.getSettings().getContourReferenceVolumeDb() == -42.0f);
}

TEST_CASE ("App: following without a readable volume waits for the first read; another output starts afresh (E32)")
{
    const flubapptest::TempFolder temp;
    auto endpoint = std::make_shared<FakeEndpoint>();
    endpoint->set (false, 0.0f, false, "No such sink");
    EngineController c (optionsWith (temp, endpoint));

    c.setContourFollowsVolume (true);
    CHECK (endpoint->getReads() == 1); // read at once
    CHECK (! c.getSettings().getContourReferenceVolumeDb().has_value());
    CHECK (allChainsAt (c, 0.0f));

    // The first read that works sets the reference.
    endpoint->set (true, -12.0f);
    c.pollEndpointVolume();
    CHECK (*c.getSettings().getContourReferenceVolumeDb() == -12.0f);
    endpoint->set (true, -30.0f);
    c.pollEndpointVolume();
    CHECK (allChainsAt (c, -18.0f));

    // Another output: its first read failing does not hold the last one's volume.
    c.simulateOutputDevice ("USB Headset", 48000.0, 2);
    endpoint->set (false, 0.0f, false, "No such sink");
    c.pollEndpointVolume();
    CHECK (endpoint->lastDevice() == "USB Headset");
    CHECK (c.getListeningLevel().device == "USB Headset");
    CHECK (allChainsAt (c, 0.0f));
    endpoint->set (true, -24.0f);
    c.pollEndpointVolume();
    CHECK (allChainsAt (c, -12.0f));
}

TEST_CASE ("App: the background poll reads the volume off the message thread and applies it (E32)")
{
    const flubapptest::TempFolder temp;
    auto endpoint = std::make_shared<FakeEndpoint>();
    endpoint->set (true, -10.0f);
    auto options = optionsWith (temp, endpoint);
    options.pollEndpointVolumeHeadless = true;
    EngineController c (options);

    // No poll while off.
    flubapptest::pumpMessagesUntil ([] { return false; }, 2 * EngineController::kEndpointVolumePollMs);
    CHECK (endpoint->getReads() == 0);

    c.setContourFollowsVolume (true); // reference -10 dB
    endpoint->set (true, -26.0f);
    REQUIRE (flubapptest::pumpMessagesUntil ([&] { return allChainsAt (c, -16.0f); }, 5000));
    const int reads = endpoint->getReads();
    CHECK (reads >= 2);

    // Off stops it.
    c.setContourFollowsVolume (false);
    CHECK (allChainsAt (c, 0.0f));
    const int afterOff = endpoint->getReads();
    flubapptest::pumpMessagesUntil ([] { return false; }, 3 * EngineController::kEndpointVolumePollMs);
    CHECK (endpoint->getReads() == afterOff);
    CHECK (allChainsAt (c, 0.0f));
}

TEST_CASE ("App: the listening level reaches the contour - a strip with contour.on lifts 50 Hz as the volume goes down (E32)")
{
    using namespace flub::param;
    const flubapptest::TempFolder temp;
    auto endpoint = std::make_shared<FakeEndpoint>();
    endpoint->set (true, -6.0f);
    EngineController c (optionsWith (temp, endpoint));
    const int music = c.findStrip ("Music");
    REQUIRE (music >= 0);
    auto& store = c.getParams (music);
    store.set (Bank::A, ContourOn, 1.0f);
    store.set (Bank::B, ContourOn, 1.0f);

    const auto tilt = [&] { return renderedGainDb (c, music, 50.0) - renderedGainDb (c, music, 1000.0); };
    const double atReference = tilt();
    CHECK (std::abs (c.getChain (music).getContourLiftAt50HzDb()) < 0.05f);

    // 30 dB below the reference: ISO 226 asks for about +11.9 dB at 50 Hz
    // (docs/11 E32: 11.87 dB at -30 dB).
    c.setContourFollowsVolume (true); // reference -6 dB
    endpoint->set (true, -36.0f);
    c.pollEndpointVolume();
    const double quiet = tilt();
    CHECK_NEAR (c.getChain (music).getContourLiftAt50HzDb(), 11.87, 0.6);
    CHECK_NEAR (quiet - atReference, 11.87, 1.0);

    // A strip without contour.on is not touched by the level.
    const int game = c.findStrip ("Game");
    REQUIRE (game >= 0);
    CHECK (c.getChain (game).getListeningLevelDb() == -30.0f);
    CHECK (c.getChain (game).getContourLiftAt50HzDb() == 0.0f);
}

TEST_CASE ("App settings: the contour follow switch and reference volume are persisted, off and unset by default (E32)")
{
    const flubapptest::TempFolder temp;
    const auto file = temp.file ("settings.xml");
    {
        AppSettings settings (file, true);
        CHECK (! settings.getContourFollowsVolume());
        CHECK (! settings.getContourReferenceVolumeDb().has_value());
        settings.setContourFollowsVolume (true);
        settings.setContourReferenceVolumeDb (-17.5f);
        settings.save();
    }
    AppSettings settings (file, true);
    CHECK (settings.getContourFollowsVolume());
    REQUIRE (settings.getContourReferenceVolumeDb().has_value());
    CHECK (*settings.getContourReferenceVolumeDb() == -17.5f);
    settings.setContourReferenceVolumeDb (-500.0f); // clamped to the silent floor
    CHECK (*settings.getContourReferenceVolumeDb() == EndpointVolume::kSilentDb);
    settings.setContourReferenceVolumeDb (std::nanf ("")); // ignored
    CHECK (*settings.getContourReferenceVolumeDb() == EndpointVolume::kSilentDb);
}

TEST_CASE ("App UI: Settings > Processing describes the listening level (E32)")
{
    using ui::SettingsDialog;
    EngineController::ListeningLevel level;
    CHECK (SettingsDialog::describeListeningLevel (level).startsWith ("Off:"));

    level.following = true;
    level.known = true;
    level.volumeDb = -35.0f;
    level.referenceDb = -20.0f;
    level.levelDb = -15.0f;
    CHECK (SettingsDialog::describeListeningLevel (level) == "System volume now -35.0 dB, reference -20.0 dB: the contour plays -15.0 dB re the reference.");
    level.muted = true;
    CHECK (SettingsDialog::describeListeningLevel (level).contains ("(muted)"));

    level = {};
    level.following = true;
    level.error = "pactl not found";
    CHECK (SettingsDialog::describeListeningLevel (level)
           == "The system volume cannot be read (pactl not found): the contour plays 0.0 dB re the reference.");
}

TEST_CASE ("App UI: Settings > Processing has the listening-level controls, off by default, driving the controller (E32)")
{
    const flubapptest::TempFolder temp;
    auto endpoint = std::make_shared<FakeEndpoint>();
    endpoint->set (true, -18.0f);
    EngineController c (optionsWith (temp, endpoint));
    ui::HotkeyHooks hooks;
    hooks.isSupported = [] { return false; };
    hooks.getFailures = [] { return juce::StringArray(); };
    hooks.reRegister = [] {};
    ui::SettingsDialog dialog (c, hooks, [] (ui::MeterPalette) {}, ui::MeterPalette::Standard);
    dialog.setSize (900, 700);
    dialog.showPage (ui::SettingsDialog::Page::Processing);

    auto* follow = findByTitle<juce::ToggleButton> (dialog, "Follow the system volume");
    auto* contour = findByTitle<juce::ToggleButton> (dialog, "Loudness contour");
    auto* reference = findByTitle<juce::Slider> (dialog, "Reference volume");
    REQUIRE (follow != nullptr);
    REQUIRE (contour != nullptr);
    REQUIRE (reference != nullptr);
    CHECK (! follow->getToggleState());
    CHECK (! contour->getToggleState());
    CHECK (! reference->isEnabled());

    follow->triggerClick();
    REQUIRE (flubapptest::pumpMessagesUntil ([&] { return c.getContourFollowsVolume(); }, 2000));
    CHECK (*c.getSettings().getContourReferenceVolumeDb() == -18.0f);
    CHECK (reference->isEnabled());
    CHECK (reference->getValue() == -18.0);

    reference->setValue (-24.0, juce::sendNotificationSync); // the slider sets the reference
    CHECK (*c.getSettings().getContourReferenceVolumeDb() == -24.0f);
    CHECK (allChainsAt (c, 6.0f));

    contour->triggerClick(); // the selected strip's contour.on
    REQUIRE (flubapptest::pumpMessagesUntil ([&] { return c.getSelectedParams().get (flub::param::ContourOn) >= 0.5f; }, 2000));
}
