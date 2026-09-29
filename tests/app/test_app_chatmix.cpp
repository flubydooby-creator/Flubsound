// App-level tests: the voice-chat controls (docs/11 E22). ChatMix from the
// tray flyout (ui::QuickControls), the Chat strip's row (ui::RoutingPanel)
// and the ChatMix hotkeys all move the one MixEngine balance: measured at
// the device output (the host's callback driven with a fake 14-input device,
// a tone per strip, the chains bypassed), only Game and Chat move, in
// opposite directions, and the strip gains never do. The "Duck game under
// voice chat" switch and its depth persist in AppSettings and reach the
// MixEngine of a restarted app. The voice dot (flyout and Chat row) follows
// a speech-like input on the Chat strip and stays dark for a tone.
#include "AppTestSupport.h"
#include "TestSignals.h"

#include "engine/EngineController.h"
#include "settings/AppSettings.h"
#include "shell/HotkeyManager.h"
#include "ui/QuickControls.h"
#include "ui/RoutingPanel.h"

#include "flub/analysis/LoudnessMeter.h"
#include "flub/engine/Parameters.h"

#include <juce_audio_devices/juce_audio_devices.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <vector>

using namespace flub::app;

namespace
{
constexpr double kRate = 48000.0;
constexpr int kBlock = 256;
constexpr int kInputs = 14; // Game 0-7 (7.1), Music 8-9, Chat 10-11, two spare
constexpr double kGameHz = 300.0, kMusicHz = 700.0, kChatHz = 1700.0;

EngineController::Options options (const flubapptest::TempFolder& temp, bool persist = false)
{
    EngineController::Options o;
    o.openAudioDevice = false;
    o.restoreState = false;
    o.enableAppRouting = false;
    o.settingsFile = temp.file ("settings.xml");
    o.persistSettings = persist;
    return o;
}

class FakeDevice final : public juce::AudioIODevice
{
public:
    FakeDevice() : juce::AudioIODevice ("Fake Chat Device", "Fake") {}

    juce::StringArray getOutputChannelNames() override { return { "Left", "Right" }; }
    juce::StringArray getInputChannelNames() override
    {
        juce::StringArray names;
        for (int i = 0; i < kInputs; ++i)
            names.add ("In " + juce::String (i + 1));
        return names;
    }
    juce::Array<double> getAvailableSampleRates() override { return { kRate }; }
    juce::Array<int> getAvailableBufferSizes() override { return { kBlock }; }
    int getDefaultBufferSize() override { return kBlock; }
    juce::String open (const juce::BigInteger&, const juce::BigInteger&, double, int) override { return {}; }
    void close() override {}
    bool isOpen() override { return true; }
    void start (juce::AudioIODeviceCallback*) override {}
    void stop() override {}
    bool isPlaying() override { return true; }
    juce::String getLastError() override { return {}; }
    int getCurrentBufferSizeSamples() override { return kBlock; }
    double getCurrentSampleRate() override { return kRate; }
    int getCurrentBitDepth() override { return 32; }
    juce::BigInteger getActiveOutputChannels() const override { return juce::BigInteger (0x3); }
    juce::BigInteger getActiveInputChannels() const override { return juce::BigInteger ((1 << kInputs) - 1); }
    int getOutputLatencyInSamples() override { return kBlock; }
    int getInputLatencyInSamples() override { return kBlock; }
};

/** Accepts every chord; the test presses them through HotkeyManager::perform. */
class NullHotkeys final : public flub::platform::GlobalHotkeys
{
public:
    using GlobalHotkeys::registerHotkey;
    bool isSupported() const override { return true; }
    bool registerHotkey (int id, const flub::platform::KeyChord&, const std::string&, std::function<void()>) override
    {
        reportBinding (id, BindingResult::Status::Registered);
        return true;
    }
    void unregisterHotkey (int) override {}
    void unregisterAll() override {}
};

/** The controller's host with the fake device started; strips fed from the
    device inputs and their chains bypassed (the fold only). */
struct Rig
{
    explicit Rig (EngineController& c) : controller (c)
    {
        auto& host = controller.getHost();
        host.audioDeviceAboutToStart (&device); // message thread: configures synchronously
        game = controller.findStrip ("Game");
        music = controller.findStrip ("Music");
        chat = controller.findStrip ("Chat");
        std::array<int, AudioEngineHost::kMaxStrips> map {};
        map.fill (-1);
        map[static_cast<size_t> (game)] = 0;
        map[static_cast<size_t> (music)] = 8;
        map[static_cast<size_t> (chat)] = 10;
        host.setDeviceInputMap (map);
        for (const int s : { game, music, chat })
        {
            auto& p = host.getMixEngine().params (s);
            p.set (flub::param::BypassAll, 1.0f);
            p.set (flub::param::LoudnessMatchBypass, 0.0f);
            p.set (flub::param::VirtualizerOn, 0.0f);
            // The fold as a 7.1 input (Auto would switch a front-only input to stereo after a while).
            p.set (flub::param::VirtInputMode, static_cast<float> (flub::param::InputModeValue::ForceSurround));
        }
        for (auto& ch : in)
            ch.assign (kBlock, 0.0f);
        for (auto& ch : out)
            ch.assign (kBlock, 0.0f);
    }

    /** Runs `seconds`; `fill` writes each block's inputs (default: the three
        tones). The output's left channel is kept. `afterBlock` runs after
        every block. */
    void run (double seconds, const std::function<void (int64_t)>& fill = {}, const std::function<void()>& afterBlock = {})
    {
        std::array<const float*, kInputs> inPtr {};
        std::array<float*, 2> outPtr { out[0].data(), out[1].data() };
        for (size_t c = 0; c < in.size(); ++c)
            inPtr[c] = in[c].data();
        const auto blocks = static_cast<int> (std::lround (seconds * kRate / kBlock));
        left.clear();
        for (int b = 0; b < blocks; ++b, sample += kBlock)
        {
            if (fill != nullptr)
                fill (sample);
            else
                fillTones();
            controller.getHost().audioDeviceIOCallbackWithContext (inPtr.data(), kInputs, outPtr.data(), 2, kBlock, {});
            left.insert (left.end(), out[0].begin(), out[0].end());
            if (afterBlock != nullptr)
                afterBlock();
        }
    }

    void fillTones()
    {
        for (auto& ch : in)
            std::fill (ch.begin(), ch.end(), 0.0f);
        for (int k = 0; k < kBlock; ++k)
        {
            const double t = static_cast<double> (sample + k) / kRate;
            const auto tone = [t] (double hz) { return 0.05f * static_cast<float> (std::sin (flub::kTwoPi * hz * t)); };
            in[0][static_cast<size_t> (k)] = tone (kGameHz); // Game FL
            in[8][static_cast<size_t> (k)] = in[9][static_cast<size_t> (k)] = tone (kMusicHz);
            in[10][static_cast<size_t> (k)] = in[11][static_cast<size_t> (k)] = tone (kChatHz);
        }
    }

    /** Level (dB) of `hz` in the last 0.1 s of the left output (a whole
        number of cycles of every tone). */
    double levelDb (double hz) const
    {
        const auto n = static_cast<size_t> (0.1 * kRate);
        double re = 0.0, im = 0.0;
        const size_t from = left.size() - n;
        for (size_t i = 0; i < n; ++i)
        {
            const double w = flub::kTwoPi * hz * static_cast<double> (i) / kRate;
            re += left[from + i] * std::cos (w);
            im += left[from + i] * std::sin (w);
        }
        return 20.0 * std::log10 (std::max (2.0 * std::hypot (re, im) / static_cast<double> (n), 1.0e-12));
    }

    struct Levels
    {
        double game, music, chat;
    };
    /** After 0.3 s (ChatMix glides over 50 ms). */
    Levels settle()
    {
        run (0.3);
        return { levelDb (kGameHz), levelDb (kMusicHz), levelDb (kChatHz) };
    }

    EngineController& controller;
    FakeDevice device;
    std::array<std::vector<float>, kInputs> in;
    std::array<std::vector<float>, 2> out;
    std::vector<float> left;
    int64_t sample = 0;
    int game = -1, music = -1, chat = -1;
};

/** A speech-like mono signal (tests/test_mix_engine_sidechain.cpp's
    formantSpeech, shortened): syllables of an impulse train at 100 - 170 Hz
    through two vowel formants, 30 - 70 ms apart, from `start` to `end`
    seconds; scaled to -20 LUFS (as stereo). */
std::vector<float> formantSpeech (int n, double start, double end)
{
    static const double vowels[5][2] = { { 730, 1090 }, { 270, 2290 }, { 300, 870 }, { 530, 1840 }, { 570, 840 } };
    std::vector<float> x (static_cast<size_t> (n), 0.0f);
    flub::FastRandom rng (7);
    const auto uniform = [&rng] (double lo, double hi) { return lo + (hi - lo) * 0.5 * (static_cast<double> (rng.nextBipolar()) + 1.0); };
    const auto samplesOf = [] (double s) { return static_cast<int> (std::lround (s * kRate)); };
    struct Resonator
    {
        double a1 = 0.0, a2 = 0.0, g = 0.0, y1 = 0.0, y2 = 0.0;
        void set (double hz, double bw)
        {
            const double r = std::exp (-flub::kPi * bw / kRate);
            a1 = 2.0 * r * std::cos (flub::kTwoPi * hz / kRate);
            a2 = -r * r;
            g = 1.0 - r;
        }
        double process (double in)
        {
            const double y = g * in + a1 * y1 + a2 * y2;
            y2 = y1;
            y1 = y;
            return y;
        }
    };
    for (double t = start; t < end - 0.3;)
    {
        const double dur = uniform (0.12, 0.24), f0 = uniform (100.0, 170.0);
        const int onset = samplesOf (t), len = samplesOf (dur), edge = samplesOf (0.02);
        const auto& v = vowels[rng.nextU32() % 5u];
        Resonator r1, r2;
        r1.set (v[0], 90.0);
        r2.set (v[1], 110.0);
        double phase = 1.0;
        for (int i = 0; i < len && onset + i < n; ++i)
        {
            phase += f0 * (1.0 - 0.1 * i / len) / kRate;
            const double pulse = phase >= 1.0 ? 1.0 : 0.0;
            if (phase >= 1.0)
                phase -= 1.0;
            const double a = i < edge ? 0.5 - 0.5 * std::cos (flub::kPi * i / edge) : 1.0;
            const double b = len - i < edge ? 0.5 - 0.5 * std::cos (flub::kPi * (len - i) / edge) : 1.0;
            x[static_cast<size_t> (onset + i)] = static_cast<float> (a * b * (r1.process (pulse) + 0.6 * r2.process (pulse)));
        }
        t += dur + uniform (0.03, 0.07);
    }

    flub::LoudnessMeter meter;
    meter.prepare (kRate, 2);
    std::array<float*, 2> ptrs { x.data(), x.data() };
    for (int pos = 0; pos < n; pos += kBlock)
        meter.process (flub::AudioBlock (ptrs.data(), 2, std::min (kBlock, n - pos), pos));
    const double gain = std::pow (10.0, (-20.0 - meter.getIntegratedLufs()) / 20.0);
    for (auto& s : x)
        s = static_cast<float> (s * gain);
    return x;
}
} // namespace

TEST_CASE ("App: ChatMix from the tray flyout, the Chat row and the hotkeys moves only Game and Chat, oppositely, at the output (E22)")
{
    const flubapptest::TempFolder temp;
    EngineController controller (options (temp));
    Rig rig (controller);
    auto& host = controller.getHost();
    ui::QuickControls flyout (controller);
    ui::RoutingPanel panel (controller);
    HotkeyManager hotkeys (controller, std::make_unique<NullHotkeys>());
    REQUIRE (panel.getChatMixSlider() != nullptr);
    CHECK (flyout.getChatMixSlider().isEnabled());

    std::vector<float> gains;
    for (int s = 0; s < controller.getNumStrips(); ++s)
        gains.push_back (host.getStripGainDb (s));
    const auto centre = rig.settle();

    // Moves towards Chat and towards Game from the three controls, and the
    // expected law: the side moved towards stays, the other falls to 1 - |b|.
    struct Step
    {
        const char* control;
        std::function<void()> move;
        float balance;
    };
    const std::vector<Step> steps {
        { "flyout", [&] { flyout.getChatMixSlider().setValue (0.5, juce::sendNotificationSync); }, 0.5f },
        { "Chat row", [&] { panel.getChatMixSlider()->setValue (-0.3, juce::sendNotificationSync); }, -0.3f },
        { "hotkey", [&] { hotkeys.perform (HotkeyAction::ChatMixToChat); }, -0.1f },
        { "hotkey x2", [&] { for (int i = 0; i < 2; ++i) hotkeys.perform (HotkeyAction::ChatMixToChat); }, 0.3f },
        { "Chat row, double-click value", [&] { panel.getChatMixSlider()->setValue (0.0, juce::sendNotificationSync); }, 0.0f },
    };
    double previousRatio = centre.chat - centre.game;
    float previousBalance = 0.0f;
    for (const auto& step : steps)
    {
        const int failuresBefore = flubtest::currentFailures();
        step.move();
        CHECK (controller.getChatMix() == step.balance);
        CHECK (host.getMixEngine().getChatMix() == step.balance);
        const auto l = rig.settle();
        const double gameDb = 20.0 * std::log10 (1.0 - std::max (0.0, static_cast<double> (step.balance)));
        const double chatDb = 20.0 * std::log10 (1.0 + std::min (0.0, static_cast<double> (step.balance)));
        CHECK (std::abs ((l.game - centre.game) - gameDb) < 0.05);
        CHECK (std::abs ((l.chat - centre.chat) - chatDb) < 0.05);
        CHECK (std::abs (l.music - centre.music) < 0.01); // no other strip moves
        // Oppositely: towards Chat raises Chat against Game, towards Game lowers it.
        const double ratio = l.chat - l.game;
        if (step.balance > previousBalance)
            CHECK (ratio > previousRatio + 0.5);
        else if (step.balance < previousBalance)
            CHECK (ratio < previousRatio - 0.5);
        if (flubtest::currentFailures() != failuresBefore)
            std::cerr << "    (ChatMix from the " << step.control << " to " << step.balance << ")\n";
        previousRatio = ratio;
        previousBalance = step.balance;
        // The strip gains (the faders, the host) never move.
        for (int s = 0; s < controller.getNumStrips(); ++s)
            CHECK (host.getStripGainDb (s) == gains[static_cast<size_t> (s)]);
        // Every control shows the balance (the flyout on Change::Parameters,
        // the Chat row on its next display frame).
        panel.updateMeters (0.02);
        CHECK (std::abs (flyout.getChatMixSlider().getValue() - step.balance) < 1.0e-4);
        CHECK (std::abs (panel.getChatMixSlider()->getValue() - step.balance) < 1.0e-4);
    }

    // Back at the centre: both 0 dB again.
    const auto back = rig.settle();
    CHECK (std::abs (back.game - centre.game) < 0.001);
    CHECK (std::abs (back.chat - centre.chat) < 0.001);

    // An engine rebuilt (a new layout keeps the strips) starts on the balance.
    controller.setChatMix (0.6f);
    host.audioDeviceAboutToStart (&rig.device);
    CHECK (host.getMixEngine().getChatMix() == 0.6f);

    // Without a Chat strip the controls grey out.
    controller.setStripLayout ({ { "Game", 8 }, { "Music", 2 } });
    flyout.refresh();
    CHECK (! flyout.getChatMixSlider().isEnabled());
    panel.rebuildStrips();
    CHECK (panel.getChatMixSlider() == nullptr);
    CHECK (host.getMixEngine().getChatMix() == 0.0f);
}

TEST_CASE ("App: 'Duck game under voice chat' persists and reaches the MixEngine; the voice dot follows speech on the Chat strip (E22)")
{
    const flubapptest::TempFolder temp;
    {
        EngineController controller (options (temp, true));
        CHECK (! controller.getChatDuck()); // off by default
        CHECK (controller.getChatDuckDepthDb() == EngineController::kDefaultChatDuckDepthDb);
        CHECK (! controller.getHost().getMixEngine().getChatDuck());

        ui::RoutingPanel panel (controller);
        auto* duck = panel.getChatDuckButton();
        auto* depth = panel.getChatDuckDepthSlider();
        REQUIRE (duck != nullptr);
        REQUIRE (depth != nullptr);
        CHECK (! duck->getToggleState());
        CHECK (! depth->isEnabled());
        duck->triggerClick();
        REQUIRE (flubapptest::pumpMessagesUntil ([&] { return controller.getChatDuck(); }, 2000));
        CHECK (depth->isEnabled());
        depth->setValue (6.0, juce::sendNotificationSync);
        CHECK (controller.getChatDuckDepthDb() == 6.0f);
        CHECK (controller.getHost().getMixEngine().getChatDuck());
        CHECK (controller.getHost().getMixEngine().getChatDuckDepthDb() == 6.0f);
        controller.getSettings().save();
    }

    // A restarted app: the setting, in the settings file and in the engine.
    EngineController controller (options (temp, true));
    CHECK (controller.getChatDuck());
    CHECK (controller.getChatDuckDepthDb() == 6.0f);
    CHECK (controller.getHost().getMixEngine().getChatDuck());
    CHECK (controller.getHost().getMixEngine().getChatDuckDepthDb() == 6.0f);

    // The voice dot: a 1 kHz tone on Chat (0.5 s) never lights it; speech
    // (1.5 s) does, within 0.25 s, and holds; 1 s of silence after it puts it out.
    Rig rig (controller);
    ui::QuickControls flyout (controller);
    ui::RoutingPanel panel (controller);
    const int n = static_cast<int> (4.0 * kRate);
    const auto speech = formantSpeech (n, 0.5, 2.0);
    std::map<int, bool> flyoutLit, rowLit; // per 50 ms
    float deepestDuck = 0.0f;
    rig.run (
        4.0,
        [&] (int64_t start)
        {
            for (auto& ch : rig.in)
                std::fill (ch.begin(), ch.end(), 0.0f);
            for (int k = 0; k < kBlock; ++k)
            {
                const auto i = static_cast<size_t> (start + k);
                const float x = start + k < static_cast<int64_t> (0.5 * kRate)
                                    ? 0.1f * static_cast<float> (std::sin (flub::kTwoPi * 1000.0 * static_cast<double> (i) / kRate))
                                    : (i < speech.size() ? speech[i] : 0.0f);
                rig.in[10][static_cast<size_t> (k)] = rig.in[11][static_cast<size_t> (k)] = x;
                rig.in[0][static_cast<size_t> (k)] = 0.05f * static_cast<float> (std::sin (flub::kTwoPi * kGameHz * static_cast<double> (i) / kRate));
            }
        },
        [&]
        {
            flyout.pollVoice();
            panel.updateMeters (static_cast<double> (kBlock) / kRate);
            const int slot = static_cast<int> (static_cast<double> (rig.sample + kBlock) / kRate / 0.05);
            flyoutLit[slot] = flyoutLit[slot] || flyout.isVoiceDotLit();
            rowLit[slot] = rowLit[slot] || panel.isVoiceDotLit();
            CHECK (flyout.isVoiceDotLit() == controller.isChatVoiceActive());
            CHECK (panel.isVoiceDotLit() == controller.isChatVoiceActive());
            deepestDuck = std::max (deepestDuck, controller.getChatDuckAmount());
        });
    const auto litBetween = [] (const std::map<int, bool>& lit, double from, double to)
    {
        int count = 0;
        for (const auto& [slot, on] : lit)
            if (slot * 0.05 >= from && slot * 0.05 < to && on)
                ++count;
        return count;
    };
    for (const auto* lit : { &flyoutLit, &rowLit })
    {
        CHECK (litBetween (*lit, 0.0, 0.5) == 0);  // the tone
        CHECK (litBetween (*lit, 0.5, 0.75) > 0);  // speech found within 0.25 s (measured: 0.10 - 0.15 s)
        CHECK (litBetween (*lit, 0.5, 2.0) >= 25); // held through it (measured: 28 of 30 slots)
        CHECK (litBetween (*lit, 3.0, 4.0) == 0);  // out after the hangover
    }
    CHECK (deepestDuck > 0.9f); // the duck (on) followed the talker
}
