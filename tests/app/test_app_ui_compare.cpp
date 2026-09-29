// App-level tests: loudness-matched comparisons (docs/11 E37 Phases 2 and 4).
//
// * The rule: only the louder side is turned down (matchTrims); the values
//   a comparison estimates leave the master bypass out; identical banks get
//   no trim.
// * The header's A/B (BankComparison): two banks 4+ LU apart are matched
//   within 1 LU 1 s after a flip, with no re-prepare (Done-when row); after
//   the short-term window the match is refined from the strip's own meters
//   and stays within 1 LU; a preset load releases the trim, a copy makes the
//   banks equal, switching the match off releases it; the header's line and
//   tooltip say what is trimmed.
// * A module's ear (ListenMatch): holding the Loudness Maximizer's ear on
//   Club Loud is matched within 1 LU of the sound with it; after the session
//   the trim returns to 0 dB at 2 dB/s. The virtualiser's ear on the 7.1 Game
//   strip is estimated on the 7.1 game scene.
// * The blind A/B/X test: hidden banks from the seed, X plays the hidden
//   bank, answers are scored, the binomial p-value, the bank is put back;
//   A = Boost 0 against B = Boost 100 plays within 1 LU (the level-matched
//   half of the ABX Done-when row); the panel covers the header and closes
//   on Escape.
//
// The programmes are not stationary, so a matched flip is judged against a
// run that did not flip, over the same stretch of the same deterministic
// programme, each run on a fresh engine (as tests/app/test_app_ui_preset_browser.cpp).
#include "AppTestSupport.h"

#include "engine/EngineController.h"
#include "engine/TestSignalGenerator.h"
#include "ui/AbxPanel.h"
#include "ui/Comparison.h"
#include "ui/HeaderBar.h"
#include "ui/MainComponent.h"

#include "flub/analysis/LoudnessMeter.h"
#include "flub/engine/Parameters.h"

#include <array>
#include <cmath>
#include <functional>
#include <iostream>
#include <memory>
#include <vector>

using namespace flub::app;
using namespace flub::param;

namespace
{
EngineController::Options headlessOptions (const flubapptest::TempFolder& temp)
{
    EngineController::Options o;
    o.openAudioDevice = false;
    o.restoreState = false;
    o.enableAppRouting = false;
    o.settingsFile = temp.file ("settings.xml");
    o.persistSettings = false;
    o.foregroundAppFactory = [] { return std::unique_ptr<flub::platform::ForegroundApp>(); };
    return o;
}

const PresetInfo& preset (EngineController& c, const char* name)
{
    const auto* p = c.getPresetManager().findByName (name);
    REQUIRE (p != nullptr);
    return *p;
}

/** One strip (a quicker render) named `name`, with `channels` channels. */
int singleStrip (EngineController& c, const char* name, int channels = 2)
{
    flub::StripConfig strip;
    strip.name = name;
    strip.inputChannels = channels;
    c.setStripLayout ({ strip });
    c.setSelectedStrip (0);
    return 0;
}

/** Loads `a` into bank A and `b` into bank B of `strip`; A plays. */
void loadBanks (EngineController& c, int strip, const char* a, const char* b)
{
    juce::String error;
    c.setActiveBank (Bank::B, strip);
    REQUIRE (c.loadPreset (preset (c, b), strip, error));
    c.setActiveBank (Bank::A, strip);
    REQUIRE (c.loadPreset (preset (c, a), strip, error));
}

/** Renders `seconds` of the master output in 512-sample blocks, advancing
    `clock` (seconds) and calling `tick` every 0.1 s of audio; from
    `measureFrom` on it measures the output's integrated loudness. */
float render (EngineController& c, TestSignalGenerator& source, double seconds, double measureFrom, double& clock,
              const std::function<void()>& tick = {})
{
    const double sr = c.getHost().getSampleRate();
    flub::LoudnessMeter meter;
    meter.prepare (sr, 2);
    std::vector<float> l (512), r (512);
    std::array<float*, 2> out { l.data(), r.data() };
    const int total = static_cast<int> (seconds * sr), from = static_cast<int> (measureFrom * sr), tickEvery = static_cast<int> (0.1 * sr);
    int sinceTick = 0;
    for (int done = 0; done < total; done += 512)
    {
        const int n = juce::jmin (512, total - done);
        c.getHost().renderOffline (source, n, out.data(), 2);
        clock += static_cast<double> (n) / sr;
        if (done >= from)
            meter.process (flub::AudioBlock (out.data(), 2, n));
        sinceTick += n;
        if (sinceTick >= tickEvery && tick != nullptr)
        {
            sinceTick = 0;
            tick();
        }
    }
    return meter.getIntegratedLufs();
}

bool waitFor (const std::function<bool()>& done)
{
    return flubapptest::pumpMessagesUntil (done, 20000);
}

// ---- The matched A/B -----------------------------------------------------------------------
struct AbRun
{
    float levelDb = 0.0f;  // output loudness over the measured stretch
    float trimDb = 0.0f;   // the comparison's trim then
    bool live = false;     // refined from the strip's meters
};

/** Plays A (Lo-Fi Chill) for 6 s with B = Club Loud on the game scene, then
    flips to B when `flip` is set and measures from `measureFrom` to
    `measureTo` seconds after that moment. */
AbRun abRun (bool flip, double measureFrom, double measureTo, const std::shared_ptr<ui::PresetLoudnessEstimator>& shared)
{
    const flubapptest::TempFolder temp;
    EngineController c (headlessOptions (temp));
    const int strip = singleStrip (c, "Music");
    loadBanks (c, strip, "Lo-Fi Chill", "Club Loud");
    TestSignalGenerator source (c.getHost().getSampleRate());
    source.setProgramme (strip, TestSignalGenerator::Programme::Game71, -3.0f);

    double clock = 0.0;
    ui::BankComparison comparison (c, [shared] { return shared; });
    comparison.setClock ([&clock] { return clock; });
    render (c, source, 6.0, 6.0, clock, [&] { comparison.poll(); });
    // Both banks estimated at the strip's level (requested by the poll).
    REQUIRE (waitFor ([&] { return comparison.gainOf (strip, Bank::A).has_value() && comparison.gainOf (strip, Bank::B).has_value(); }));

    if (flip)
        c.setActiveBank (Bank::B, strip); // Change::Parameters: the comparison applies the trim at once
    AbRun r;
    render (c, source, measureFrom, measureFrom, clock, [&] { comparison.poll(); });
    r.trimDb = c.getComparisonTrimDb (strip);
    r.levelDb = render (c, source, measureTo - measureFrom, 0.0, clock, [&] { comparison.poll(); });
    r.live = comparison.getStatus (strip).live;
    CHECK (! c.getHost().needsReprepare());
    return r;
}
} // namespace

// =============================================================================
// The rule
// =============================================================================
TEST_CASE ("App UI: comparisons turn only the louder side down; their values leave the master bypass out (E37)")
{
    auto t = ui::matchTrims (-1.0f, 3.0f);
    CHECK (t.first == 0.0f);
    CHECK (t.second == -4.0f);
    t = ui::matchTrims (2.0f, -0.5f);
    CHECK (t.first == -2.5f);
    CHECK (t.second == 0.0f);
    CHECK (ui::matchTrims (0.0f, 40.0f).second == -EngineController::kMaxComparisonTrimDb);
    t = ui::matchTrims (std::nanf (""), 1.0f);
    CHECK (t.first == 0.0f);
    CHECK (t.second == 0.0f);

    ParameterStore store;
    store.set (Bank::A, BypassAll, 1.0f);
    store.set (Bank::A, LoudnessMatchBypass, 1.0f);
    const auto v = ui::comparisonValues (store, Bank::A);
    CHECK (v[static_cast<size_t> (BypassAll)] == 0.0f);
    CHECK (v[static_cast<size_t> (LoudnessMatchBypass)] == 0.0f);
    CHECK (v == ui::comparisonValues (store, Bank::B));

    // The estimator's variants key apart; the default keeps the stereo keys.
    using E = ui::PresetLoudnessEstimator;
    const std::vector<float> values (4, 0.5f);
    E::Variant surround;
    surround.channels = 8;
    E::Variant listen;
    listen.listenBypassId = MaximizerOn;
    CHECK (E::keyOf (values, -20.0f) == E::keyOf (values, -20.0f, {}));
    CHECK (E::keyOf (values, -20.0f, surround) != E::keyOf (values, -20.0f));
    CHECK (E::keyOf (values, -20.0f, listen) != E::keyOf (values, -20.0f));

    // The controller's two slots add up and never go above 0 dB.
    const flubapptest::TempFolder temp;
    EngineController c (headlessOptions (temp));
    c.setComparisonTrimDb (0, -3.0f);
    c.setComparisonTrimDb (0, -2.0f, EngineController::ComparisonSlot::Listen);
    CHECK (c.getTotalComparisonTrimDb (0) == -5.0f);
    CHECK (c.getHost().getStripGainDb (0) == -5.0f);
    c.setComparisonTrimDb (0, 4.0f);
    CHECK (c.getComparisonTrimDb (0) == 0.0f);
    c.setComparisonTrimDb (0, 0.0f, EngineController::ComparisonSlot::Listen);
    CHECK (c.getHost().getStripGainDb (0) == 0.0f);
}

// =============================================================================
// The header's A/B (Phase 2)
// =============================================================================
TEST_CASE ("App UI: two banks 4+ LU apart are matched within 1 LU 1 s after an A/B flip, with no re-prepare (E37)")
{
    const auto shared = std::make_shared<ui::PresetLoudnessEstimator> (48000.0);
    const auto stay = abRun (false, 1.0, 2.5, shared);
    const auto flipped = abRun (true, 1.0, 2.5, shared);
    const float unmatched = flipped.levelDb - flipped.trimDb - stay.levelDb;
    std::cerr << "    A Lo-Fi Chill -> B Club Loud on the game scene, 1-2.5 s after the flip: unmatched " << unmatched << " LU, matched "
              << (flipped.levelDb - stay.levelDb) << " LU (trim " << flipped.trimDb << " dB)\n";
    CHECK (unmatched > 4.0f);
    CHECK (flipped.trimDb < -4.0f);
    CHECK (std::abs (flipped.levelDb - stay.levelDb) <= 1.0f);
}

TEST_CASE ("App UI: the A/B match is refined from the strip's own meters after the short-term window and stays within 1 LU (E37)")
{
    const auto shared = std::make_shared<ui::PresetLoudnessEstimator> (48000.0);
    const auto stay = abRun (false, 4.0, 5.5, shared);
    const auto flipped = abRun (true, 4.0, 5.5, shared);
    std::cerr << "    4-5.5 s after the flip: matched " << (flipped.levelDb - stay.levelDb) << " LU (trim " << flipped.trimDb << " dB, "
              << (flipped.live ? "refined from the meters" : "estimate") << ")\n";
    CHECK (flipped.live);
    CHECK (flipped.trimDb < -4.0f);
    CHECK (std::abs (flipped.levelDb - stay.levelDb) <= 1.0f);
}

TEST_CASE ("App UI: a preset load releases the A/B trim, a copy makes the banks equal, the switch turns matching off; the header says what is trimmed (E37)")
{
    const flubapptest::TempFolder temp;
    EngineController c (headlessOptions (temp));
    const int strip = singleStrip (c, "Music");
    loadBanks (c, strip, "Lo-Fi Chill", "Club Loud");

    ui::HeaderBar header (c);
    header.setSize (1280, 56);
    auto& comparison = header.getComparison();
    double clock = 0.0;
    comparison.setClock ([&clock] { return clock; });
    comparison.poll();
    clock += 1.5;
    comparison.poll(); // requests both estimates (default level: nothing plays)
    REQUIRE (waitFor ([&] { return comparison.gainOf (strip, Bank::A).has_value() && comparison.gainOf (strip, Bank::B).has_value(); }));

    // Flip to the louder B through the header's button.
    header.getBankButton (Bank::B).triggerClick();
    REQUIRE (waitFor ([&] { return c.getActiveBank (strip) == Bank::B; }));
    const auto status = comparison.getStatus (strip);
    CHECK (status.comparing);
    CHECK (status.banksDiffer);
    CHECK (status.trimDb < -3.0f);
    CHECK (status.gapKnown);
    CHECK (std::abs (status.gapLu + status.trimDb) < 0.01f); // B is trimmed by exactly its gap over A
    CHECK (header.getAbCaption().startsWith ("B -"));
    CHECK (header.getBankButton (Bank::B).getTooltip().contains ("B plays"));
    CHECK (ui::BankComparison::describe (status).contains ("LU louder than A (estimated)"));

    // Back to the quieter A: no trim (the quieter side is never raised).
    header.getBankButton (Bank::A).triggerClick();
    REQUIRE (waitFor ([&] { return c.getActiveBank (strip) == Bank::A; }));
    CHECK (c.getComparisonTrimDb (strip) == 0.0f);
    CHECK (header.getAbCaption() == "matched");
    CHECK (ui::BankComparison::describe (comparison.getStatus (strip)).contains ("A is the quieter"));

    // B again, then a preset load: the loaded preset plays at its own level.
    c.setActiveBank (Bank::B, strip);
    CHECK (c.getComparisonTrimDb (strip) < -3.0f);
    juce::String error;
    REQUIRE (c.loadPreset (preset (c, "Club Loud"), strip, error));
    CHECK (c.getComparisonTrimDb (strip) == 0.0f);
    CHECK (! comparison.getStatus (strip).comparing);
    CHECK (header.getAbCaption().isEmpty());

    // A copy: identical banks, nothing to match.
    c.setActiveBank (Bank::A, strip);
    c.setActiveBank (Bank::B, strip);
    CHECK (c.getComparisonTrimDb (strip) < -3.0f);
    c.copyActiveToOtherBank (strip);
    comparison.poll();
    CHECK (c.getComparisonTrimDb (strip) == 0.0f);
    CHECK (! comparison.getStatus (strip).banksDiffer);

    // The switch: off releases, on (persisted) matches the next flip.
    REQUIRE (c.loadPreset (preset (c, "Lo-Fi Chill"), strip, error)); // B: Lo-Fi Chill, A: Club Loud
    c.setActiveBank (Bank::A, strip);
    CHECK (c.getComparisonTrimDb (strip) < -3.0f);
    header.setComparisonMatched (false);
    CHECK (c.getComparisonTrimDb (strip) == 0.0f);
    CHECK (! c.getSettings().getComparisonMatched());
    c.setActiveBank (Bank::B, strip);
    c.setActiveBank (Bank::A, strip);
    CHECK (c.getComparisonTrimDb (strip) == 0.0f);
    CHECK (header.getBankButton (Bank::A).getTooltip().contains ("not loudness matched"));
    header.setComparisonMatched (true);
    CHECK (c.getSettings().getComparisonMatched());
    CHECK (c.getComparisonTrimDb (strip) < -3.0f);
}

TEST_CASE ("App UI: the bypass line reads the processed loudness over the input from before the bypass (E37)")
{
    CHECK (ui::HeaderBar::formatProcessedDelta (2.94f) == "+2.9 LU");
    CHECK (ui::HeaderBar::formatProcessedDelta (-1.0f) == "-1.0 LU");

    const flubapptest::TempFolder temp;
    EngineController c (headlessOptions (temp));
    const int strip = singleStrip (c, "Music");
    juce::String error;
    REQUIRE (c.loadPreset (preset (c, "Club Loud"), strip, error));
    TestSignalGenerator source (c.getHost().getSampleRate());
    source.setProgramme (strip, TestSignalGenerator::Programme::Music, -12.0f);
    ui::HeaderBar header (c);
    header.setSize (1280, 56);
    double clock = 0.0;
    render (c, source, 3.5, 3.5, clock);
    header.updateStatus();
    CHECK (header.getBypassCaption().isEmpty()); // not bypassed
    c.setEnabled (false);
    header.updateStatus();
    const auto caption = header.getBypassCaption();
    std::cerr << "    Club Loud on music at -12 dB, bypassed: \"" << caption << "\"\n";
    CHECK (caption.startsWith ("proc. +"));
    CHECK (header.getBypassCaption() == caption); // frozen while bypassed
    render (c, source, 1.0, 1.0, clock);
    header.updateStatus();
    CHECK (header.getBypassCaption() == caption);
}

// =============================================================================
// Module and virtualiser listen
// =============================================================================
namespace
{
struct ListenRun
{
    float levelDb = 0.0f, trimDb = 0.0f;
    std::optional<float> gap;
};

/** Club Loud on the Music strip (music at -12 dB): holds the maximizer's ear
    from 4 s on when `hold` is set, measures 0.5-2 s into the hold. */
ListenRun listenRun (bool hold, const std::shared_ptr<ui::PresetLoudnessEstimator>& shared)
{
    const flubapptest::TempFolder temp;
    EngineController c (headlessOptions (temp));
    const int strip = singleStrip (c, "Music");
    juce::String error;
    REQUIRE (c.loadPreset (preset (c, "Club Loud"), strip, error));
    TestSignalGenerator source (c.getHost().getSampleRate());
    source.setProgramme (strip, TestSignalGenerator::Programme::Music, -12.0f);
    double clock = 0.0;
    ui::ListenMatch match (c, [shared] { return shared; });
    match.setClock ([&clock] { return clock; });
    render (c, source, 4.0, 4.0, clock);

    // The pointer over the ear: both sides estimated before the hold.
    match.prepare (strip, MaximizerOn);
    const auto values = ui::comparisonValues (c.getParams (strip), Bank::A);
    const float level = ui::programmeLevel (c, strip).value_or (-18.0f);
    ui::PresetLoudnessEstimator::Variant without;
    without.listenBypassId = MaximizerOn;
    REQUIRE (waitFor ([&] { return shared->find (values, level).has_value() && shared->find (values, level, without).has_value(); }));

    ListenRun r;
    if (hold)
    {
        c.setAuditionBypass (strip, MaximizerOn, true);
        match.listen (strip, MaximizerOn, true);
    }
    r.trimDb = c.getComparisonTrimDb (strip, EngineController::ComparisonSlot::Listen);
    r.gap = match.getGapLu();
    r.levelDb = render (c, source, 2.0, 0.5, clock);
    if (hold)
    {
        // Released: the louder "with" side is trimmed for the session, then back to 0 dB at 2 dB/s.
        c.setAuditionBypass (strip, MaximizerOn, false);
        match.listen (strip, MaximizerOn, false);
        const float withTrim = c.getComparisonTrimDb (strip, EngineController::ComparisonSlot::Listen);
        CHECK (withTrim == match.getTrimDb());
        if (r.gap.has_value() && *r.gap < -0.5f)
            CHECK (withTrim < -0.4f);
        clock += 5.0;
        match.poll();
        CHECK (c.getComparisonTrimDb (strip, EngineController::ComparisonSlot::Listen) == withTrim); // still in the session
        clock += 5.5;
        match.poll();
        if (withTrim < -0.3f)
            CHECK (c.getComparisonTrimDb (strip, EngineController::ComparisonSlot::Listen) > withTrim); // releasing
        for (int i = 0; i < 200; ++i)
            match.poll();
        CHECK (c.getComparisonTrimDb (strip, EngineController::ComparisonSlot::Listen) == 0.0f);
    }
    return r;
}
} // namespace

TEST_CASE ("App UI: holding a module's ear is loudness matched within 1 LU; after the session the trim returns to 0 dB (E37)")
{
    const auto shared = std::make_shared<ui::PresetLoudnessEstimator> (48000.0);
    const auto with = listenRun (false, shared);
    const auto held = listenRun (true, shared);
    REQUIRE (held.gap.has_value());
    const float unmatched = held.levelDb - held.trimDb - with.levelDb;
    std::cerr << "    Club Loud, the Loudness Maximizer's ear held: unmatched " << unmatched << " LU (estimated gap " << *held.gap
              << " LU), matched " << (held.levelDb - with.levelDb) << " LU (trim while held " << held.trimDb << " dB)\n";
    CHECK (std::abs (unmatched) > 1.5f); // the maximizer changes the loudness: there is something to match
    CHECK (std::abs (*held.gap - unmatched) <= 1.0f);
    // The quieter side is never raised: a held "without" that is quieter plays as it is,
    // and the level difference then shows up on the "with" side after the release.
    if (*held.gap < 0.0f)
        CHECK (held.trimDb == 0.0f);
    else
        CHECK (std::abs (held.levelDb - with.levelDb) <= 1.0f);
}

TEST_CASE ("App UI: the virtualiser's ear on the 7.1 Game strip is estimated on the 7.1 game scene (E37)")
{
    const flubapptest::TempFolder temp;
    EngineController c (headlessOptions (temp));
    const int strip = singleStrip (c, "Game", 8);
    REQUIRE (c.getStripChannels (strip) == 8);
    juce::String error;
    REQUIRE (c.loadPreset (preset (c, "Competitive FPS"), strip, error));
    const auto shared = std::make_shared<ui::PresetLoudnessEstimator> (48000.0);
    ui::ListenMatch match (c, [shared] { return shared; });
    match.prepare (strip, VirtualizerOn);
    const auto values = ui::comparisonValues (c.getParams (strip), c.getActiveBank (strip));
    ui::PresetLoudnessEstimator::Variant with, without;
    with.channels = without.channels = 8;
    without.listenBypassId = VirtualizerOn;
    const float level = ui::PresetLoudnessEstimator::kDefaultLevelLufs;
    REQUIRE (waitFor ([&] { return shared->find (values, level, with).has_value() && shared->find (values, level, without).has_value(); }));
    const float gWith = *shared->find (values, level, with), gWithout = *shared->find (values, level, without);
    std::cerr << "    Competitive FPS on the 7.1 scene: with the virtualiser " << gWith << " LU, without " << gWithout << " LU\n";
    CHECK (std::isfinite (gWith));
    CHECK (std::isfinite (gWithout));
    CHECK (gWith != gWithout);
    // The stereo estimate is another one (the 7.1 scene takes the fold and the virtualiser).
    REQUIRE (waitFor ([&]
                      {
                          shared->request (values, level);
                          return shared->find (values, level).has_value();
                      }));
    CHECK (*shared->find (values, level) != gWith);
    match.listen (strip, VirtualizerOn, true);
    REQUIRE (match.getGapLu().has_value());
    CHECK (std::abs (*match.getGapLu() - (gWithout - gWith)) < 1.0e-4f);
    match.reset();
    CHECK (c.getTotalComparisonTrimDb (strip) == 0.0f);
}

// =============================================================================
// The blind A/B/X test (Phase 4)
// =============================================================================
TEST_CASE ("App UI: the A/B/X test hides a random bank behind X, scores the answers with the binomial p-value and puts the bank back (E37)")
{
    using T = ui::AbxTest;
    CHECK (std::abs (T::pValue (10, 10) - 1.0 / 1024.0) < 1.0e-12);
    CHECK (std::abs (T::pValue (7, 10) - 176.0 / 1024.0) < 1.0e-12);
    CHECK (std::abs (T::pValue (9, 12) - 299.0 / 4096.0) < 1.0e-12);
    CHECK (T::pValue (0, 10) == 1.0);
    CHECK (T::pValue (3, 0) == 1.0);

    const flubapptest::TempFolder temp;
    EngineController c (headlessOptions (temp));
    const int strip = singleStrip (c, "Music");
    loadBanks (c, strip, "Lo-Fi Chill", "Club Loud");
    {
        T a (c, strip, 12, 1234), b (c, strip, 12, 1234), other (c, strip, 12, 99);
        int differences = 0, bs = 0;
        for (int i = 0; i < 12; ++i)
        {
            CHECK (a.hiddenBank (i) == b.hiddenBank (i)); // the seed decides
            differences += a.hiddenBank (i) != other.hiddenBank (i) ? 1 : 0;
            bs += a.hiddenBank (i) == Bank::B ? 1 : 0;
        }
        CHECK (differences > 0);
        CHECK (bs > 0);
        CHECK (bs < 12);
    }
    CHECK (c.getActiveBank (strip) == Bank::A); // put back
    c.setActiveBank (Bank::B, strip);
    {
        T test (c, strip, 10, 42);
        CHECK (test.getPlaying() == T::Choice::X);
        CHECK (c.getActiveBank (strip) == test.hiddenBank (0)); // X plays the hidden bank
        test.play (T::Choice::A);
        CHECK (c.getActiveBank (strip) == Bank::A);
        test.play (T::Choice::B);
        CHECK (c.getActiveBank (strip) == Bank::B);
        // Always right: 10 of 10.
        for (int i = 0; i < 10; ++i)
        {
            test.play (T::Choice::X);
            CHECK (test.getTrial() == i);
            test.answer (c.getActiveBank (strip) == Bank::A);
        }
        CHECK (test.isFinished());
        CHECK (test.getCorrect() == 10);
        CHECK (test.describeResult().startsWith ("10 of 10 right: p = 0.001"));
        CHECK (test.describeResult().contains ("You can reliably tell A from B"));
        test.answer (true); // after the end: ignored
        CHECK (test.getTrial() == 10);
    }
    CHECK (c.getActiveBank (strip) == Bank::B);
    {
        T test (c, strip, 10, 7);
        for (int i = 0; i < 10; ++i)
            test.answer (i % 2 == 0);
        CHECK (test.describeResult().contains ("No reliable difference") == (T::pValue (test.getCorrect(), 10) >= 0.05));
    }
}

namespace
{
/** Signature with A = Boost 0 and B = Boost 100 on the Music strip (music at
    -12 dB); plays `choice` in an A/B/X test and measures 1-2.5 s later. */
AbRun abxRun (ui::AbxTest::Choice choice, const std::shared_ptr<ui::PresetLoudnessEstimator>& shared)
{
    const flubapptest::TempFolder temp;
    EngineController c (headlessOptions (temp));
    const int strip = singleStrip (c, "Music");
    loadBanks (c, strip, "Flubsound Signature", "Flubsound Signature");
    c.getParams (strip).set (Bank::A, BoostIntensity, 0.0f);
    c.getParams (strip).set (Bank::B, BoostIntensity, 1.0f);
    TestSignalGenerator source (c.getHost().getSampleRate());
    source.setProgramme (strip, TestSignalGenerator::Programme::Music, -12.0f);
    double clock = 0.0;
    ui::BankComparison comparison (c, [shared] { return shared; });
    comparison.setClock ([&clock] { return clock; });
    render (c, source, 4.0, 4.0, clock, [&] { comparison.poll(); });
    REQUIRE (waitFor ([&] { return comparison.gainOf (strip, Bank::A).has_value() && comparison.gainOf (strip, Bank::B).has_value(); }));

    ui::AbxTest test (c, strip, 10, 5);
    test.play (choice);
    AbRun r;
    render (c, source, 1.0, 1.0, clock, [&] { comparison.poll(); });
    r.trimDb = c.getComparisonTrimDb (strip);
    r.levelDb = render (c, source, 1.5, 0.0, clock, [&] { comparison.poll(); });
    CHECK (! c.getHost().needsReprepare());
    return r;
}
} // namespace

TEST_CASE ("App UI: in the A/B/X test Boost 0 and Boost 100 play within 1 LU of each other (E37)")
{
    const auto shared = std::make_shared<ui::PresetLoudnessEstimator> (48000.0);
    const auto a = abxRun (ui::AbxTest::Choice::A, shared);
    const auto b = abxRun (ui::AbxTest::Choice::B, shared);
    const float unmatched = (b.levelDb - b.trimDb) - (a.levelDb - a.trimDb);
    std::cerr << "    Signature, Boost 0 (A) against Boost 100 (B), music at -12 dB: unmatched " << unmatched << " LU, matched "
              << (b.levelDb - a.levelDb) << " LU (trims A " << a.trimDb << " dB, B " << b.trimDb << " dB)\n";
    CHECK (std::abs (unmatched) > 2.0f);
    CHECK (std::abs (b.levelDb - a.levelDb) <= 1.0f);
}

TEST_CASE ("App UI: the A/B/X panel covers the whole window, plays and answers with keys, and closing puts the bank back (E37)")
{
    const flubapptest::TempFolder temp;
    EngineController c (headlessOptions (temp));
    c.getPresetManager().setUserPresetFolder (temp.file ("Presets"));
    const int strip = c.findStrip ("Music");
    c.setSelectedStrip (strip);
    ui::MainComponent main (c);
    main.setSize (1100, 700);

    // Identical banks: nothing to test.
    c.copyActiveToOtherBank (strip);
    main.openBlindTest();
    REQUIRE (main.getBlindTest() != nullptr);
    CHECK (main.getBlindTest()->getTest() == nullptr);
    CHECK (main.getBlindTest()->getStatusText().startsWith ("A and B hold the same sound"));
    CHECK (! main.getBlindTest()->getAnswerButton (true).isEnabled());
    main.getBlindTest()->getCloseButton().triggerClick();
    REQUIRE (flubapptest::pumpMessagesUntil ([&] { return main.getBlindTest() == nullptr; }, 2000));

    c.getParams (strip).set (Bank::B, BoostIntensity, 1.0f);
    c.setActiveBank (Bank::B, strip);
    main.openBlindTest();
    auto* panel = main.getBlindTest();
    REQUIRE (panel != nullptr);
    REQUIRE (panel->getTest() != nullptr);
    CHECK (panel->getBounds() == main.getLocalBounds()); // the header's A/B and its trim line are covered
    CHECK (panel->getStatusText() == "Trial 1 of 10: which one is X?");
    CHECK (panel->keyPressed (juce::KeyPress ('a')));
    CHECK (c.getActiveBank (strip) == Bank::A);
    CHECK (panel->getPlayButton (ui::AbxTest::Choice::A).getToggleState());
    CHECK (panel->keyPressed (juce::KeyPress ('x')));
    CHECK (c.getActiveBank (strip) == panel->getTest()->hiddenBank (0));
    CHECK (panel->keyPressed (juce::KeyPress ('1')));
    CHECK (panel->getTest()->getTrial() == 1);
    CHECK (panel->getStatusText() == "Trial 2 of 10: which one is X?");
    for (int i = 1; i < 10; ++i)
        CHECK (panel->keyPressed (juce::KeyPress (i % 2 == 0 ? '1' : '2')));
    CHECK (panel->getTest()->isFinished());
    CHECK (panel->getStatusText().contains (" of 10 right: p = "));
    CHECK (! panel->getAnswerButton (true).isEnabled());
    main.setSize (800, 560);
    CHECK (panel->getBounds() == main.getLocalBounds());
    CHECK (main.getLocalBounds().contains (ui::AbxPanel::cardBounds (main.getLocalBounds())));
    CHECK (panel->keyPressed (juce::KeyPress (juce::KeyPress::escapeKey)));
    REQUIRE (flubapptest::pumpMessagesUntil ([&] { return main.getBlindTest() == nullptr; }, 2000));
    CHECK (c.getActiveBank (strip) == Bank::B); // what played before the test
}
