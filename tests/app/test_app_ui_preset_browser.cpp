// App-level tests: the preset browser (docs/11 E40) and its loudness-matched
// preview (docs/11 E37, the "current or previewed preset" comparison).
//
// * The model: search terms, ranking ("late night quiet" finds Late Night Low
//   Volume first), scope / mode / category / tag filters, recommendations
//   for the output, common tags, the loudness wording.
// * AppSettings: favourites and recents (order, limit, damaged values).
// * PresetAudition through the browser: a preview writes the preset's sound
//   into the ACTIVE bank only (app state, the latency profile and the other
//   bank untouched, no re-prepare), Cancel puts the bank back bit for bit
//   (except a value someone else moved meanwhile), Load goes through the
//   controller (current preset, recents), a preset loaded by someone else
//   ends the preview without restoring.
// * Matching: two presets 4.5 LU apart on the estimator's reference are
//   within 1 LU of each other 1 s after the flip on another programme, and
//   going back to the louder current sound turns it down to the preview.
// * The header: its preset box opens the browser over the window, below the
//   header and inside it at 1100 x 700; Cancel / a click outside close it.
#include "AppTestSupport.h"

#include "engine/EngineController.h"
#include "engine/TestSignalGenerator.h"
#include "shell/ScreenshotDriver.h"
#include "ui/MainComponent.h"
#include "ui/PresetBrowser.h"

#include "flub/analysis/LoudnessMeter.h"
#include "flub/engine/Parameters.h"
#include "flub/io/PresetIO.h"

#include <array>
#include <cmath>
#include <functional>
#include <iostream>
#include <memory>
#include <vector>

using namespace flub::app;
using namespace flub::param;
using Browser = ui::PresetBrowser;

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

std::vector<float> bankValues (const ParameterStore& store, Bank bank)
{
    std::vector<float> v (static_cast<size_t> (kNumParams));
    for (int i = 0; i < kNumParams; ++i)
        v[static_cast<size_t> (i)] = store.get (bank, i);
    return v;
}

juce::StringArray names (const std::vector<PresetInfo>& presets, const std::vector<int>& indices)
{
    juce::StringArray out;
    for (const int i : indices)
        out.add (presets[static_cast<size_t> (i)].name);
    return out;
}

/** Renders `seconds` of the master output (2 channels) and, from `measureFrom`
    seconds on, measures its integrated loudness. */
float renderAndMeasure (EngineController& c, TestSignalGenerator& source, double seconds, double measureFrom)
{
    const double sr = c.getHost().getSampleRate();
    flub::LoudnessMeter meter;
    meter.prepare (sr, 2);
    std::vector<float> l (512), r (512);
    std::array<float*, 2> out { l.data(), r.data() };
    const int total = static_cast<int> (seconds * sr), from = static_cast<int> (measureFrom * sr);
    for (int done = 0; done < total; done += 512)
    {
        const int n = juce::jmin (512, total - done);
        c.getHost().renderOffline (source, n, out.data(), 2);
        if (done >= from)
            meter.process (flub::AudioBlock (out.data(), 2, n));
    }
    return meter.getIntegratedLufs();
}

bool waitForEstimates (Browser& browser, const std::vector<const std::vector<float>*>& values)
{
    for (const auto* v : values)
        browser.getEstimator().request (*v, browser.getLevelLufs(), true); // ahead of the rest of the list
    return flubapptest::pumpMessagesUntil (
        [&]
        {
            for (const auto* v : values)
                if (! browser.getEstimator().find (*v, browser.getLevelLufs()).has_value())
                    return false;
            return true;
        },
        20000);
}
} // namespace

// =============================================================================
// The model
// =============================================================================
TEST_CASE ("App UI: preset search ranks by words found, then field; filters by scope, mode, category and tags (E40)")
{
    CHECK (Browser::searchTerms ("The late-night, QUIET 7.1 for me") == juce::StringArray ({ "late", "night", "quiet", "7", "1" }));
    CHECK (Browser::searchTerms ("  ").isEmpty());

    const flubapptest::TempFolder temp;
    EngineController c (headlessOptions (temp));
    c.getPresetManager().setUserPresetFolder (temp.file ("Presets"));
    const auto& all = c.getPresetManager().getPresets();
    REQUIRE (all.size() == 30);

    Browser::Filter f;
    const Browser::Context none;
    CHECK (Browser::filterPresets (all, f, none).size() == all.size()); // no filter: everything, list order

    // The Done-when search: a late-night quiet listening preset, first.
    f.query = "late night quiet listening";
    auto found = names (all, Browser::filterPresets (all, f, none));
    REQUIRE (found.size() >= 2);
    CHECK (found[0] == "Late Night Low Volume");
    CHECK (found.contains ("Night Mode Gaming"));
    f.query = "footstep";
    found = names (all, Browser::filterPresets (all, f, none));
    CHECK (found.contains ("Competitive FPS"));
    CHECK (found.contains ("Battle Royale"));
    CHECK (! found.contains ("Club Loud"));
    f.query = "zzzz";
    CHECK (Browser::filterPresets (all, f, none).empty());

    // Match weights: a name hit outranks a description hit.
    const auto& signature = preset (c, "Flubsound Signature");
    const auto m = Browser::match (signature, Browser::searchTerms ("signature"));
    CHECK (m.terms == 1);
    CHECK (m.score == 6);

    f = {};
    f.mode = "Gaming";
    const auto gaming = Browser::filterPresets (all, f, none);
    CHECK (gaming.size() == 9);
    for (const int i : gaming)
        CHECK (all[static_cast<size_t> (i)].mode == "Gaming");
    f = {};
    f.category = "Device";
    CHECK (names (all, Browser::filterPresets (all, f, none)) == juce::StringArray ({ "Bluetooth Headphones", "Earbuds", "Laptop Speakers" }));
    f = {};
    f.tags = { "night" };
    CHECK (names (all, Browser::filterPresets (all, f, none)) == juce::StringArray ({ "Night Mode Gaming", "Late Night Low Volume" }));
    f.tags = { "night", "startle-guard" };
    CHECK (names (all, Browser::filterPresets (all, f, none)) == juce::StringArray ({ "Night Mode Gaming" }));

    // Scopes: favourites in list order, recents newest first.
    Browser::Context ctx;
    ctx.favourites = { preset (c, "Wide Stage").id, preset (c, "Racing").id };
    ctx.recent = { preset (c, "Punchy Pop").id, preset (c, "Earbuds").id, "no-such-id" };
    f = {};
    f.scope = Browser::Scope::Favourites;
    CHECK (names (all, Browser::filterPresets (all, f, ctx)) == juce::StringArray ({ "Racing", "Wide Stage" }));
    f.scope = Browser::Scope::Recent;
    CHECK (names (all, Browser::filterPresets (all, f, ctx)) == juce::StringArray ({ "Punchy Pop", "Earbuds" }));
    f.query = "earbud";
    CHECK (names (all, Browser::filterPresets (all, f, ctx)) == juce::StringArray ({ "Earbuds" }));

    // For the output: the profile's suggestion, plus the Bluetooth presets on a Bluetooth link.
    auto rec = Browser::recommendedFor (all, "competitive fps", flub::device::Connection::Usb);
    CHECK (rec == juce::StringArray ({ preset (c, "Competitive FPS").id }));
    rec = Browser::recommendedFor (all, "Competitive FPS", flub::device::Connection::Bluetooth);
    CHECK (rec.size() == 2);
    CHECK (rec.contains (preset (c, "Bluetooth Headphones").id));
    CHECK (Browser::recommendedFor (all, "No Such Preset", flub::device::Connection::Analog).isEmpty());

    const auto tags = Browser::commonTags (all, 10);
    CHECK (tags.size() == 10);
    CHECK (tags[0] == "headphones"); // 6 presets
    for (const auto& t : tags)
        CHECK (t == t.toLowerCase());

    CHECK (Browser::describeLoudness (-1.0f, 1.1f) == "2.1 LU louder than your current sound");
    CHECK (Browser::describeLoudness (0.0f, -1.44f) == "1.4 LU quieter than your current sound");
    CHECK (Browser::describeLoudness (0.0f, 0.3f) == "About as loud as your current sound");
    CHECK (Browser::describeLoudness (0.0f, std::nanf ("")) == "Loudness could not be measured");
}

TEST_CASE ("App settings: favourite and recent presets are kept by id, recents newest first and at most 8 (E40)")
{
    const flubapptest::TempFolder temp;
    {
        AppSettings s (temp.file ("s.settings"), true);
        CHECK (s.getFavouritePresets().isEmpty());
        CHECK (s.getRecentPresets().isEmpty());
        CHECK (s.getPresetPreview());
        CHECK (s.getPresetPreviewMatched());
        s.setFavouritePreset ("aaaa", true);
        s.setFavouritePreset ("bbbb", true);
        s.setFavouritePreset ("aaaa", true); // once
        s.setFavouritePreset ("bad,id", true); // ignored
        CHECK (s.getFavouritePresets() == juce::StringArray ({ "bbbb", "aaaa" }));
        s.setFavouritePreset ("bbbb", false);
        CHECK (s.isFavouritePreset ("aaaa"));
        CHECK (! s.isFavouritePreset ("bbbb"));
        for (int i = 0; i < 10; ++i)
            s.addRecentPreset ("id" + juce::String (i));
        s.addRecentPreset ("id5"); // again: to the front
        const auto recent = s.getRecentPresets();
        CHECK (recent.size() == AppSettings::kMaxRecentPresets);
        CHECK (recent[0] == "id5");
        CHECK (recent[1] == "id9");
        CHECK (! recent.contains ("id0"));
        s.setPresetPreviewMatched (false);
        s.save();
    }
    {
        AppSettings s (temp.file ("s.settings"), true);
        CHECK (s.getFavouritePresets() == juce::StringArray ({ "aaaa" }));
        CHECK (s.getRecentPresets()[0] == "id5");
        CHECK (! s.getPresetPreviewMatched());
        s.getPropertiesFile().setValue ("presets.favourites", " ,x,, x ,y"); // damaged: cleaned
        CHECK (s.getFavouritePresets() == juce::StringArray ({ "x", "y" }));
    }
}

// =============================================================================
// Preview, Cancel, Load
// =============================================================================
TEST_CASE ("App UI: a preview plays in the active bank only; Cancel restores it bit for bit; Load goes through the controller (E40)")
{
    const flubapptest::TempFolder temp;
    EngineController c (headlessOptions (temp));
    c.getPresetManager().setUserPresetFolder (temp.file ("Presets"));
    const int music = c.findStrip ("Music");
    REQUIRE (music >= 0);
    c.setSelectedStrip (music);
    c.setLatencyProfile (LatencyProfileValue::Quality);
    c.getHost().reconfigure();
    REQUIRE (! c.getHost().needsReprepare());
    juce::String error;
    REQUIRE (c.loadPreset (preset (c, "Flubsound Signature"), music, error));
    auto& store = c.getParams (music);
    // B differs from A, so a write into the wrong bank would show.
    store.copyBank (Bank::A, Bank::B);
    store.set (Bank::B, BoostIntensity, 0.2f);
    const auto aBefore = bankValues (store, Bank::A), bBefore = bankValues (store, Bank::B);
    const float gainBefore = c.getHost().getStripGainDb (music);
    const auto shared = std::make_shared<ui::PresetLoudnessEstimator> (c.getHost().getSampleRate());

    {
        Browser browser (c, shared);
        CHECK (browser.isCurrentSoundSelected());
        CHECK (browser.getShownPresets().size() == 30);
        const auto& club = preset (c, "Club Loud");
        REQUIRE (browser.selectPreset (club.id));
        CHECK (browser.getAudition().getPreviewId() == club.id);

        flub::preset::Preset clubValues;
        REQUIRE (c.getPresetManager().readPreset (club, clubValues, error));
        const auto aNow = bankValues (store, Bank::A);
        int wrong = 0;
        for (int i = 0; i < kNumParams; ++i)
        {
            const auto k = static_cast<size_t> (i);
            const float expected = flub::preset::isAppState (i) ? aBefore[k] : clubValues.values[k];
            wrong += aNow[k] != expected ? 1 : 0;
        }
        CHECK (wrong == 0);
        CHECK (bankValues (store, Bank::B) == bBefore);
        CHECK (store.get (Bank::A, LatencyProfile) == static_cast<float> (LatencyProfileValue::Quality));
        CHECK (! c.getHost().needsReprepare());
        CHECK (c.getCurrentPresetId (music) == preset (c, "Flubsound Signature").id); // a preview is not a load
        CHECK (c.takePresetWarnings().empty());

        // Back to the current-sound row: Signature again; then another preview.
        REQUIRE (browser.selectPreset ({}));
        CHECK (bankValues (store, Bank::A) == aBefore);
        REQUIRE (browser.selectPreset (preset (c, "Warm Vinyl").id));
        // A value moved by someone else meanwhile (a Boost hotkey) keeps its change.
        store.set (Bank::A, BoostIntensity, 0.91f);
        browser.cancel();
        CHECK (! browser.getAudition().isActive());
        auto aAfter = bankValues (store, Bank::A);
        CHECK (aAfter[static_cast<size_t> (BoostIntensity)] == 0.91f);
        aAfter[static_cast<size_t> (BoostIntensity)] = aBefore[static_cast<size_t> (BoostIntensity)];
        CHECK (aAfter == aBefore);
        CHECK (bankValues (store, Bank::B) == bBefore);
        CHECK (c.getHost().getStripGainDb (music) == gainBefore);
    }
    store.set (Bank::A, BoostIntensity, aBefore[static_cast<size_t> (BoostIntensity)]);

    {
        // Preview off: selecting plays nothing.
        Browser browser (c, shared);
        browser.setPreviewEnabled (false);
        REQUIRE (browser.selectPreset (preset (c, "Bass Head").id));
        CHECK (bankValues (store, Bank::A) == aBefore);
        CHECK (! c.getSettings().getPresetPreview());
        browser.setPreviewEnabled (true);
        CHECK (browser.getAudition().getPreviewId() == preset (c, "Bass Head").id);

        // Load: the controller's load (current preset, recent), the trim dropped.
        bool closed = false;
        browser.onClose = [&closed] { closed = true; };
        browser.toggleFavourite (preset (c, "Bass Head").id);
        CHECK (c.getSettings().isFavouritePreset (preset (c, "Bass Head").id));
        CHECK (browser.loadSelected());
        CHECK (closed);
        CHECK (! browser.getAudition().isActive());
        CHECK (c.getCurrentPresetId (music) == preset (c, "Bass Head").id);
        CHECK (c.getSettings().getRecentPresets()[0] == preset (c, "Bass Head").id);
        CHECK (c.getHost().getStripGainDb (music) == gainBefore);
        CHECK (store.get (Bank::A, LatencyProfile) == static_cast<float> (LatencyProfileValue::Quality));
        CHECK (bankValues (store, Bank::B) == bBefore);
        CHECK (! c.isPresetModified (music));
    }

    {
        // A preset loaded by someone else (an automatic profile) during a
        // preview stays; the preview ends without restoring.
        Browser browser (c, shared);
        REQUIRE (browser.selectPreset (preset (c, "Punchy Pop").id));
        REQUIRE (c.loadPreset (preset (c, "Crystal Clarity"), music, error));
        CHECK (! browser.getAudition().isActive());
        const auto loaded = bankValues (store, Bank::A);
        browser.cancel();
        CHECK (bankValues (store, Bank::A) == loaded);
        CHECK (c.getCurrentPresetId (music) == preset (c, "Crystal Clarity").id);
    }
}

TEST_CASE ("App UI: a crash during a preview cannot save the previewed sound - the autosave keeps the strip as Cancel would leave it (E40)")
{
    const flubapptest::TempFolder temp;
    auto options = headlessOptions (temp);
    options.persistSettings = true;
    EngineController c (options);
    const int music = c.findStrip ("Music");
    REQUIRE (music >= 0);
    juce::String error;
    REQUIRE (c.loadPreset (preset (c, "Flubsound Signature"), music, error));
    auto& store = c.getParams (music);
    store.set (Bank::B, BoostIntensity, 0.2f);
    const auto saved = c.getPersistedStripState (music);

    ui::PresetAudition audition (c);
    audition.begin (music);
    REQUIRE (audition.preview (preset (c, "Club Loud"), error));
    REQUIRE (bankValues (store, Bank::A) != audition.getOriginalValues()); // Club Loud plays
    CHECK (c.getPersistedStripState (music) == saved);

    // The periodic autosave and a save on the way down both write the sound
    // before the preview; the file on disk has it (read as a restart would).
    c.saveState();
    CHECK (c.getSettings().getStripState ("Music") == saved);
    CHECK (AppSettings (temp.file ("settings.xml"), false).getStripState ("Music") == saved);

    // A value someone else moved meanwhile is saved as it is, like Cancel keeps it.
    REQUIRE (audition.preview (preset (c, "Bass Head"), error));
    store.set (Bank::A, BoostIntensity, 0.91f);
    const auto during = c.getPersistedStripState (music);
    CHECK (during != saved);
    audition.cancel();
    CHECK (c.getPersistedStripState (music) == during);
    CHECK (store.get (Bank::A, BoostIntensity) == 0.91f);

    // After the session the autosave writes the store again: a load is saved.
    REQUIRE (audition.preview (preset (c, "Warm Vinyl"), error));
    REQUIRE (audition.commit (preset (c, "Warm Vinyl"), error));
    c.saveState();
    flub::preset::Preset warm;
    REQUIRE (c.getPresetManager().readPreset (preset (c, "Warm Vinyl"), warm, error));
    CHECK (store.get (Bank::A, MaxDriveDb) == warm.values[static_cast<size_t> (MaxDriveDb)]);
    CHECK (c.getSettings().getStripState ("Music") == c.getPersistedStripState (music));
    CHECK (c.getSettings().getStripState ("Music") != saved);
}

TEST_CASE ("App UI: the preview is a never-saved audition bank - a preset save or an A/B copy during it takes the pre-preview sound, B stays bit-identical (E40)")
{
    const flubapptest::TempFolder temp;
    EngineController c (headlessOptions (temp));
    c.getPresetManager().setUserPresetFolder (temp.file ("Presets"));
    const int music = c.findStrip ("Music");
    REQUIRE (music >= 0);
    c.setSelectedStrip (music);
    juce::String error;
    REQUIRE (c.loadPreset (preset (c, "Flubsound Signature"), music, error));
    auto& store = c.getParams (music);
    store.copyBank (Bank::A, Bank::B);
    store.set (Bank::B, BoostIntensity, 0.2f);
    const auto aBefore = bankValues (store, Bank::A), bBefore = bankValues (store, Bank::B);
    const auto persistedBefore = c.getPersistedStripState (music);
    const auto shared = std::make_shared<ui::PresetLoudnessEstimator> (c.getHost().getSampleRate());
    const auto soundOf = [] (const std::vector<float>& values)
    {
        auto v = values;
        for (int i = 0; i < kNumParams; ++i)
            if (flub::preset::isAppState (i))
                v[static_cast<size_t> (i)] = 0.0f;
        return v;
    };
    const auto savedSound = [&] (const char* name)
    {
        flub::preset::Preset p;
        juce::String readError;
        REQUIRE (c.getPresetManager().readPreset (preset (c, name), p, readError));
        return soundOf (p.values);
    };

    Browser browser (c, shared);
    const auto& club = preset (c, "Club Loud");
    REQUIRE (browser.selectPreset (club.id));
    REQUIRE (soundOf (bankValues (store, Bank::A)) != soundOf (aBefore)); // Club Loud plays

    // Save As during the preview: the file holds the sound before it; the
    // preview plays on (the saved preset is now the session's start) and B
    // is untouched.
    const auto id = c.saveUserPreset ("Before The Preview", "Mine", "", music, error);
    REQUIRE (id.isNotEmpty());
    CHECK (savedSound ("Before The Preview") == soundOf (aBefore));
    CHECK (browser.getAudition().isActive());
    CHECK (browser.getAudition().getPreviewId() == club.id);
    CHECK (browser.getAudition().getPresetIdAtBegin() == id);
    CHECK (c.getCurrentPresetId (music) == id);
    CHECK (c.isPresetModified (music)); // Club Loud plays over the saved sound
    CHECK (bankValues (store, Bank::B) == bBefore);
    CHECK (c.getPersistedStripState (music) == persistedBefore); // the autosave, as before the preview
    {
        std::vector<float> values;
        CHECK (c.getSavedBankValues (music, Bank::A, values));
        CHECK (values == aBefore);
    }

    // Save (overwrite the current user preset, the header's Save) during another preview.
    REQUIRE (browser.selectPreset (preset (c, "Bass Head").id));
    REQUIRE (c.getPresetManager().saveCurrent (music, store, error));
    CHECK (savedSound ("Before The Preview") == soundOf (aBefore));

    // The A/B copy during the preview copies the sound before it.
    c.copyActiveToOtherBank (music);
    CHECK (bankValues (store, Bank::B) == aBefore);
    CHECK (soundOf (bankValues (store, Bank::A)) != soundOf (aBefore)); // the preview still plays

    // Cancel: A back bit for bit, the saved preset unmodified, B as the copy left it.
    browser.cancel();
    CHECK (bankValues (store, Bank::A) == aBefore);
    CHECK (bankValues (store, Bank::B) == aBefore);
    CHECK (! c.isPresetModified (music));
    CHECK (c.getCurrentPresetId (music) == id);

    // Without a save or copy, a preview and its cancel leave B bit-identical
    // (the done-when row; the browser has no hover audition, Cancel stands for mouse-out).
    store.set (Bank::B, BoostIntensity, 0.3f);
    const auto bNow = bankValues (store, Bank::B);
    {
        Browser again (c, shared);
        REQUIRE (again.selectPreset (preset (c, "Warm Vinyl").id));
        REQUIRE (again.selectPreset (preset (c, "Lo-Fi Chill").id));
        CHECK (bankValues (store, Bank::B) == bNow);
        again.cancel();
    }
    CHECK (bankValues (store, Bank::A) == aBefore);
    CHECK (bankValues (store, Bank::B) == bNow);
}

TEST_CASE ("App UI: the browser shows a preset's description, tags, latency profile and reader warnings (E40)")
{
    const flubapptest::TempFolder temp;
    EngineController c (headlessOptions (temp));
    const auto folder = temp.file ("Presets");
    folder.createDirectory();
    folder.getChildFile ("Typo.flubpreset.json")
        .replaceWithText (R"({ "format": "flubsound-preset", "version": 2, "name": "Typo Mix", "category": "Mine", "mode": "Music",
                               "description": "Mine.", "tags": ["test"], "params": { "bost": 0.6, "bass.boost": 40 } })");
    c.getPresetManager().setUserPresetFolder (folder);
    c.setSelectedStrip (c.findStrip ("Music"));
    c.setLatencyProfile (LatencyProfileValue::Balanced);
    const auto shared = std::make_shared<ui::PresetLoudnessEstimator> (c.getHost().getSampleRate());

    Browser browser (c, shared);
    browser.setPreviewEnabled (false);
    REQUIRE (browser.selectPreset (preset (c, "Audiophile Subtle").id));
    auto lines = browser.getDetailLines();
    CHECK (lines[0] == "Audiophile Subtle");
    CHECK (lines[1].startsWith ("Factory"));
    CHECK (lines.joinIntoString ("\n").contains ("Tags: audiophile, transparent"));
    CHECK (lines.joinIntoString ("\n").contains ("Made for the Quality latency profile (the engine runs Balanced; loading never changes it)"));

    browser.setPreviewEnabled (true);
    REQUIRE (browser.selectPreset (preset (c, "Typo Mix").id));
    CHECK (browser.getAudition().getPreviewId() == preset (c, "Typo Mix").id);
    lines = browser.getDetailLines();
    CHECK (lines[1].startsWith ("User"));
    CHECK (lines.joinIntoString ("\n").contains ("Warning: unknown parameter \"bost\" ignored (did you mean \"boost\"?)"));
    CHECK (lines.joinIntoString ("\n").contains ("Warning: \"bass.boost\" = 40 is out of range [0, 15]: clamped to 15"));
    CHECK (c.takePresetWarnings().empty()); // shown in the browser, not queued as a toast

    browser.setQuery ("typo");
    CHECK (browser.getShownPresets().size() == 1);
    CHECK (browser.getStatusText().startsWith ("1 of 31 presets"));
    browser.cancel();
}

// =============================================================================
// Loudness matching (docs/11 E37)
// =============================================================================
// A flip is judged against a run that did not flip, over the same stretch of
// the same (deterministic) programme, each run on a fresh engine. The
// programme is the test signal's music at -12 dB (about -26 LUFS in): since
// docs/11 E01's app copies (Phase 3 batch 3) the test signal's stereo
// downmix of its 7.1 game scene folds the LFE at +10 dB as the chain does,
// which plays that scene about 10 LU louder and LF-heavy on a stereo strip,
// where the estimates, made on music, then leave 1.3-2.9 LU.
namespace
{
struct FlipRun
{
    float levelDb = 0.0f;     // output loudness over the measured stretch
    float trimDb = 0.0f;      // the match's trim while it was measured
    float inputLufs = 0.0f;   // the level the estimates were made at
};

/** Plays `first` for preRollSeconds on the Music strip (the music), then opens the
    browser and runs `flip` (which may select rows) and measures the output
    from 1 s to 2.5 s after it; `second`, when given, is run after another
    1.5 s and measured the same way instead. */
FlipRun flipRun (const char* first, const std::shared_ptr<ui::PresetLoudnessEstimator>& shared, bool matched,
                 const std::function<void (Browser&, EngineController&)>& flip,
                 const std::function<void (Browser&, EngineController&)>& second = {}, double preRollSeconds = 4.5)
{
    const flubapptest::TempFolder temp;
    EngineController c (headlessOptions (temp));
    c.getPresetManager().setUserPresetFolder (temp.file ("Presets"));
    flub::StripConfig strip;
    strip.name = "Music";
    c.setStripLayout ({ strip }); // one strip: a quicker render
    const int music = c.findStrip ("Music");
    REQUIRE (music == 0);
    c.setSelectedStrip (music);
    juce::String error;
    REQUIRE (c.loadPreset (preset (c, first), music, error));
    TestSignalGenerator source (c.getHost().getSampleRate());
    source.setProgramme (music, TestSignalGenerator::Programme::Music, -12.0f);
    renderAndMeasure (c, source, preRollSeconds, preRollSeconds); // the input's 3 s loudness follower settles

    Browser browser (c, shared);
    browser.setMatchEnabled (matched);
    std::vector<std::vector<float>> values { browser.getAudition().getOriginalValues() };
    for (const auto& p : c.getPresetManager().getPresets())
        if (p.name == "Lo-Fi Chill" || p.name == "Club Loud")
            values.push_back (browser.getAudition().valuesFor (p));
    std::vector<const std::vector<float>*> wanted;
    for (const auto& v : values)
        wanted.push_back (&v);
    REQUIRE (waitForEstimates (browser, wanted));

    FlipRun r;
    r.inputLufs = browser.getLevelLufs();
    flip (browser, c);
    if (second != nullptr)
    {
        renderAndMeasure (c, source, 1.5, 1.5);
        second (browser, c);
    }
    r.trimDb = browser.getAudition().getTrimDb();
    CHECK (! c.getHost().needsReprepare());
    r.levelDb = renderAndMeasure (c, source, 2.5, 1.0);
    browser.cancel();
    CHECK (c.getHost().getStripGainDb (music) == 0.0f); // the trim is gone
    return r;
}
} // namespace

TEST_CASE ("App UI: a louder preview is matched within 1 LU of the current sound 1 s after the flip (E37/E40)")
{
    // Lo-Fi Chill and Club Loud: 4.5 LU apart on the estimator's music at
    // -13 LUFS; on the music at about -26 LUFS the gap is larger (Club
    // Loud's maximizer lifts a quiet input).
    const auto shared = std::make_shared<ui::PresetLoudnessEstimator> (48000.0);
    const auto stay = [] (Browser&, EngineController&) {};
    const auto toClub = [] (Browser& b, EngineController& c) { REQUIRE (b.selectPreset (preset (c, "Club Loud").id)); };
    // 6 s first: the level the estimates are made at comes from the input's
    // 3 s loudness follower, which reads 1.1 dB low after 4.5 s.
    const auto current = flipRun ("Lo-Fi Chill", shared, true, stay, {}, 6.0);
    const auto matched = flipRun ("Lo-Fi Chill", shared, true, toClub, {}, 6.0);
    // Unmatched: the same run without the trim, which is a plain gain after
    // the chain (the master limiter does not act at this level).
    const float unmatched = matched.levelDb - matched.trimDb - current.levelDb;
    std::cerr << "    Lo-Fi Chill -> Club Loud on the music (input " << matched.inputLufs << " LUFS), 1-2.5 s after the flip: unmatched "
              << unmatched << " LU, matched " << (matched.levelDb - current.levelDb) << " LU (trim " << matched.trimDb << " dB)\n";
    CHECK (matched.inputLufs < -20.0f);
    CHECK (matched.inputLufs > -28.0f);
    CHECK (unmatched > 4.0f); // the gap the match closes
    CHECK (matched.trimDb < -4.0f);
    CHECK (std::abs (matched.levelDb - current.levelDb) <= 1.0f);

    // Match off: no trim.
    const flubapptest::TempFolder temp;
    EngineController c (headlessOptions (temp));
    c.setSelectedStrip (c.findStrip ("Music"));
    Browser browser (c, shared);
    browser.setMatchEnabled (false);
    REQUIRE (browser.selectPreset (preset (c, "Club Loud").id));
    CHECK (browser.getAudition().getTrimDb() == 0.0f);
    CHECK (browser.getStatusText().contains ("not loudness matched"));
    browser.cancel();
}

TEST_CASE ("App UI: going back from a quieter preview turns the louder current sound down to it (E37/E40)")
{
    const auto shared = std::make_shared<ui::PresetLoudnessEstimator> (48000.0);
    const auto toLoFi = [] (Browser& b, EngineController& c)
    {
        REQUIRE (b.selectPreset (preset (c, "Lo-Fi Chill").id));
        CHECK (b.getAudition().getTrimDb() == 0.0f); // the quieter side is never raised
    };
    const auto back = [] (Browser& b, EngineController&)
    {
        REQUIRE (b.selectPreset ({}));
        CHECK (b.getStatusText().contains ("matched -"));
    };
    const auto stayed = flipRun ("Club Loud", shared, true, toLoFi, [] (Browser&, EngineController&) {}, 3.5);
    const auto returned = flipRun ("Club Loud", shared, true, toLoFi, back, 3.5);
    const float matchedGap = returned.levelDb - stayed.levelDb, unmatchedGap = matchedGap - returned.trimDb;
    std::cerr << "    Club Loud (current) after Lo-Fi Chill, against staying on Lo-Fi Chill: unmatched " << unmatchedGap << " LU, matched "
              << matchedGap << " LU (trim " << returned.trimDb << " dB)\n";
    CHECK (returned.trimDb < -4.0f);
    CHECK (unmatchedGap > 5.0f);
    // A first return is matched from the estimates alone (the live probes
    // that would refine it are core work, docs/11 E37 Phase 3): most of the
    // gap, not all of it (on the game scene before docs/11 E01's app copies:
    // within 2 LU; on the music now within 0.2 LU).
    CHECK (std::abs (matchedGap) <= 2.0f);
}

TEST_CASE ("App UI: matchTrims turns only the louder side down (E37)")
{
    using A = ui::PresetAudition;
    auto t = A::matchTrims (-1.0f, 3.0f);
    CHECK (t.previewDb == -4.0f);
    CHECK (t.originalDb == 0.0f);
    t = A::matchTrims (2.0f, -0.5f);
    CHECK (t.previewDb == 0.0f);
    CHECK (t.originalDb == -2.5f);
    t = A::matchTrims (0.0f, 40.0f);
    CHECK (t.previewDb == -A::kMaxTrimDb);
    t = A::matchTrims (std::nanf (""), 1.0f);
    CHECK (t.previewDb == 0.0f);
    CHECK (t.originalDb == 0.0f);
    CHECK (ui::PresetLoudnessEstimator::keyOf ({ 1.0f, 2.0f }) != ui::PresetLoudnessEstimator::keyOf ({ 2.0f, 1.0f }));
    CHECK (ui::PresetLoudnessEstimator::keyOf ({ 1.0f }, -20.0f) != ui::PresetLoudnessEstimator::keyOf ({ 1.0f }, -21.0f));
    CHECK (ui::PresetLoudnessEstimator::keyOf ({ 1.0f }, -20.2f) == ui::PresetLoudnessEstimator::keyOf ({ 1.0f }, -19.8f));
    CHECK (ui::PresetLoudnessEstimator::levelBucket (-80.0f) == -40);
    CHECK (ui::PresetLoudnessEstimator::levelBucket (std::nanf ("")) == -18);
}

// =============================================================================
// The header and the window
// =============================================================================
TEST_CASE ("App UI: the header's preset box opens the browser over the window; Cancel and a click outside close it (E40)")
{
    const flubapptest::TempFolder temp;
    EngineController c (headlessOptions (temp));
    c.getPresetManager().setUserPresetFolder (temp.file ("Presets"));
    const int music = c.findStrip ("Music");
    c.setSelectedStrip (music);
    juce::String error;
    REQUIRE (c.loadPreset (preset (c, "Flubsound Signature"), music, error));
    const auto before = bankValues (c.getParams (music), Bank::A);

    ui::MainComponent main (c);
    for (const auto size : { juce::Point<int> (1100, 700), juce::Point<int> (1280, 820) })
    {
        main.setSize (size.x, size.y);
        auto& header = main.getHeader();
        header.getPresetBox().showPopup(); // a click on the box
        auto* browser = header.getPresetBrowser();
        REQUIRE (browser != nullptr);
        auto* overlay = browser->getParentComponent();
        REQUIRE (overlay != nullptr);
        CHECK (overlay->getParentComponent() == &main);
        CHECK (overlay->getBounds() == main.getLocalBounds());
        const auto b = main.getLocalArea (browser, browser->getLocalBounds());
        CHECK (main.getLocalBounds().contains (b));
        CHECK (b.getY() >= header.getBottom());
        CHECK (b.getWidth() >= 700);
        CHECK (b.getHeight() >= 480);
        for (auto* child : browser->getChildren())
            if (child->isVisible())
                CHECK (browser->getLocalBounds().contains (child->getBounds()));
        CHECK (browser->getList().getHeight() >= 200);
        header.showPresetBrowser(); // already open: the same one
        CHECK (header.getPresetBrowser() == browser);

        REQUIRE (browser->selectPreset (preset (c, "Club Loud").id));
        CHECK (bankValues (c.getParams (music), Bank::A) != before);
        if (size.x == 1100)
        {
            browser->cancel(); // Cancel / Escape
        }
        else
        {
            // A click on the scrim, outside the browser.
            const juce::MouseEvent click (juce::Desktop::getInstance().getMainMouseSource(), { 4.0f, 700.0f }, {}, 1.0f, 0.0f, 0.0f, 0.0f,
                                          0.0f, overlay, overlay, juce::Time::getCurrentTime(), { 4.0f, 700.0f },
                                          juce::Time::getCurrentTime(), 1, false);
            overlay->mouseDown (click);
        }
        REQUIRE (flubapptest::pumpMessagesUntil ([&header] { return header.getPresetBrowser() == nullptr; }, 2000));
        CHECK (bankValues (c.getParams (music), Bank::A) == before);
    }

    // Load from the browser: closed, loaded, recorded as recent.
    main.getHeader().showPresetBrowser();
    auto* browser = main.getHeader().getPresetBrowser();
    REQUIRE (browser != nullptr);
    browser->setQuery ("late night quiet");
    REQUIRE (! browser->getShownPresets().empty());
    REQUIRE (browser->selectPreset (browser->getShownPresets().front()->id));
    CHECK (browser->loadSelected());
    REQUIRE (flubapptest::pumpMessagesUntil ([&main] { return main.getHeader().getPresetBrowser() == nullptr; }, 2000));
    CHECK (c.getCurrentPresetName (music) == "Late Night Low Volume");
    CHECK (c.getSettings().getRecentPresets()[0] == preset (c, "Late Night Low Volume").id);
}

TEST_CASE ("App UI: the screenshot driver accepts --state preset-browser (E40)")
{
    ScreenshotDriver::Options o;
    juce::String error;
    CHECK (ScreenshotDriver::parseCommandLine (juce::StringArray ({ "--screenshot", "a.png", "--state", "preset-browser,recovery" }), o, error));
    CHECK (error.isEmpty());
    CHECK (o.states.contains ("preset-browser"));
}
