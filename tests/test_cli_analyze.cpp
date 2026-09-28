// `flubsound-cli analyze --events / --bands --events / --glitches` and the
// `soak` options (docs/11 E60 stage 1, E53): the scene events and octave
// band tracks of a user capture (flub::analyseSceneEvents, the E60 scene
// tests' detector), and the discontinuity detector over a file. One seeded
// 12 s programme with known events; each case takes well under a second.
#include "TestFramework.h"
#include "TestSignals.h"

#include "Analysis.h"
#include "CliOptions.h"

#include "flub/common/Math.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <string>
#include <vector>

using namespace flub;
using namespace flub::cli;
using namespace flubtest;

namespace
{
constexpr double kFs = 48000.0;

int at (double seconds) { return static_cast<int> (std::lround (seconds * kFs)); }

/** 12 s, stereo: a pink bed (10 Hz high-passed) at -35 dBFS RMS with five
    30 ms 3.2 kHz steps at -29 dBFS peak (0.5, 1.1, 1.7, 2.3, 2.9 s) and a
    300 ms noise burst at -6 dBFS peak at 4.0 s; from 7.0 s the bed 15 dB
    quieter (a level change); digital silence from 10.5 s. Every edge is a
    2 ms raised-cosine ramp. */
std::vector<std::vector<float>> programme()
{
    const int n = at (12.0);
    auto bed = pinkNoise (n, 1.0f, 99);
    double x1 = 0.0, y1 = 0.0;
    const double k = std::exp (-kTwoPi * 10.0 / kFs);
    for (auto& v : bed)
    {
        const double y = k * (y1 + v - x1);
        x1 = v;
        y1 = y;
        v = static_cast<float> (y);
    }
    const auto burst = whiteNoise (at (0.3), 0.5f, 7);
    const auto ramp = [] (double t, double from, double to) {
        const double r = 0.002;
        if (t < from || t > to)
            return 0.0;
        const double a = std::min (1.0, (t - from) / r), b = std::min (1.0, (to - t) / r);
        return (0.5 - 0.5 * std::cos (kPi * a)) * (0.5 - 0.5 * std::cos (kPi * b));
    };
    std::vector<float> mono (static_cast<size_t> (n));
    for (int i = 0; i < n; ++i)
    {
        const double t = i / kFs;
        double v = bed[static_cast<size_t> (i)] * (0.0178 * ramp (t, -1.0, 7.0) + 0.00316 * ramp (t, 7.0, 10.5));
        for (double s : { 0.5, 1.1, 1.7, 2.3, 2.9 })
            v += 0.035 * ramp (t, s, s + 0.03) * std::sin (kTwoPi * 3200.0 * (t - s));
        if (t >= 4.0 && t < 4.3)
            v += burst[static_cast<size_t> (i - at (4.0))] * ramp (t, 4.0, 4.3);
        mono[static_cast<size_t> (i)] = static_cast<float> (v);
    }
    return { mono, mono };
}

std::vector<SceneEvent> ofType (const std::vector<SceneEvent>& events, SceneEventType type)
{
    std::vector<SceneEvent> r;
    std::copy_if (events.begin(), events.end(), std::back_inserter (r), [type] (const SceneEvent& e) { return e.type == type; });
    return r;
}
} // namespace

TEST_CASE ("CLI analyze: --events / --event-band / --glitches / --bands parse for analyze only; soak takes the chain options and its own")
{
    CliOptions o;
    std::string error;
    REQUIRE (parseCommandLine ({ "analyze", "-i", "a.wav", "--bands", "--events", "--glitches" }, o, error));
    CHECK (o.bands && o.events && o.glitches && o.eventBandHz == 0.0);
    REQUIRE (parseCommandLine ({ "analyze", "a.wav", "--event-band", "3.2k" }, o, error));
    CHECK (o.events && o.eventBandHz == 3200.0); // --event-band implies --events
    REQUIRE (parseCommandLine ({ "analyze", "a.wav", "--event-band=800Hz" }, o, error));
    CHECK (o.eventBandHz == 800.0);
    CHECK (! parseCommandLine ({ "analyze", "a.wav", "--event-band", "5" }, o, error));
    CHECK (! parseCommandLine ({ "process", "-i", "a.wav", "-o", "b.wav", "--events" }, o, error));
    CHECK (error.find ("--events") != std::string::npos);

    REQUIRE (parseCommandLine ({ "soak" }, o, error));
    CHECK (o.command == Command::Soak && o.soakSeconds == 600.0 && o.seed == 1u && o.automation == "user" && o.intervalMs == 250.0);
    REQUIRE (parseCommandLine ({ "soak", "--minutes", "2", "--seed", "7", "--automation", "all", "--interval", "100ms", "--rate", "44100",
                                 "--preset", "Late Night", "--block", "256", "--protection", "normal", "--json" },
                               o, error));
    CHECK (o.soakSeconds == 120.0 && o.seed == 7u && o.automation == "all" && o.intervalMs == 100.0 && o.rate == 44100.0);
    CHECK (o.render.presetSpec == "Late Night" && o.render.blockSize == 256 && o.json);
    REQUIRE (parseCommandLine ({ "soak", "--seconds", "30" }, o, error));
    CHECK (o.soakSeconds == 30.0);
    CHECK (! parseCommandLine ({ "soak", "--automation", "sometimes" }, o, error));
    CHECK (! parseCommandLine ({ "soak", "--seed", "0" }, o, error));
    CHECK (! parseCommandLine ({ "soak", "--seconds", "0" }, o, error));
    CHECK (! parseCommandLine ({ "soak", "-i", "a.wav" }, o, error));
    CHECK (! parseCommandLine ({ "soak", "--target-lufs", "-14" }, o, error));
    CHECK (! parseCommandLine ({ "quality", "--seed", "3" }, o, error));
}

TEST_CASE ("CLI analyze --events: the burst, the level change and the silence of a known programme in the full band, its steps in the 3.2 kHz band")
{
    const auto ch = programme();
    const auto full = sceneEvents (ch, kFs);
    CHECK (full.bandHz == 0.0);
    CHECK_NEAR (full.frameSeconds, 0.01, 1.0e-9);

    const auto loud = ofType (full.events, SceneEventType::Loud);
    REQUIRE (loud.size() == 1);
    CHECK_NEAR (loud[0].startSeconds, 4.0, 0.02);
    CHECK_NEAR (loud[0].endSeconds, 4.3, 0.02);
    const auto silence = ofType (full.events, SceneEventType::Silence);
    REQUIRE (silence.size() == 1);
    CHECK_NEAR (silence[0].startSeconds, 10.5, 0.02);
    CHECK_NEAR (silence[0].endSeconds, 12.0, 0.02);
    const auto change = ofType (full.events, SceneEventType::LevelChange);
    CHECK (std::any_of (change.begin(), change.end(), [] (const SceneEvent& e) { return std::abs (e.startSeconds - 7.0) < 0.3 && e.overBackgroundDb < -10.0f; }));
    CHECK (full.counts[static_cast<size_t> (SceneEventType::Loud)] == 1);
    CHECK (full.counts[static_cast<size_t> (SceneEventType::Silence)] == 1);

    // In the step band the five steps are onsets, each within 20 ms.
    const auto band = sceneEvents (ch, kFs, 3200.0);
    CHECK (band.bandHz == 3200.0);
    const auto onsets = ofType (band.events, SceneEventType::Onset);
    for (double s : { 0.5, 1.1, 1.7, 2.3, 2.9 })
        CHECK (std::any_of (onsets.begin(), onsets.end(), [s] (const SceneEvent& e) { return std::abs (e.startSeconds - s) <= 0.02; }));
    CHECK (onsets.size() == 5);

    // JSON: counts and the full list; text: one line per event.
    const auto j = eventsToJson (band);
    CHECK (j["bandHz"].asNumber() == 3200.0);
    CHECK (j["counts"]["onset"].asNumber() == 5.0);
    CHECK (j["list"].asArray().size() == band.events.size());
    CHECK (j["list"].asArray()[0]["type"].asString() == "onset");
    CHECK (eventsToJson (full)["bandHz"].isNull());
    const auto text = formatEvents (band, 3);
    CHECK (text.find ("5 onset") != std::string::npos);
    CHECK (text.find ("3.2k Hz band") != std::string::npos);
    CHECK (text.find ("more; --json lists all") != std::string::npos);
}

TEST_CASE ("CLI analyze --bands --events: octave-band level tracks in 100 ms frames with percentiles and per-band events")
{
    const auto ch = programme();
    const auto tracks = bandTracks (ch, kFs);
    REQUIRE (tracks.size() == 10); // 31.5 Hz .. 16 kHz at 48 kHz
    CHECK (tracks.front().centreHz == 31.5f && tracks.back().centreHz == 16000.0f);
    for (const auto& t : tracks)
    {
        CHECK (t.levelDb.size() == 120); // 12 s
        CHECK (t.p10Db <= t.medianDb && t.medianDb <= t.p90Db);
        // Digital silence from 10.5 s reads at the floor in every band ...
        CHECK (t.levelDb[115] <= -150.0f);
        // ... and the bed about 15 dB quieter after 7 s than at 3.5 s (one
        // 100 ms frame of noise in a band: a few dB either way).
        const float before = t.levelDb[35], after = t.levelDb[85];
        CHECK (before - after > 8.0f && before - after < 22.0f);
        CHECK (t.counts[static_cast<size_t> (SceneEventType::Silence)] == 1);
    }
    // The 4 kHz band (2.8 - 5.7 kHz) holds the 3.2 kHz steps: an onset at each.
    const auto& b4k = tracks[7];
    CHECK (b4k.centreHz == 4000.0f);
    const auto onsets = ofType (b4k.events, SceneEventType::Onset);
    for (double s : { 0.5, 1.1, 1.7, 2.3, 2.9 })
        CHECK (std::any_of (onsets.begin(), onsets.end(), [s] (const SceneEvent& e) { return std::abs (e.startSeconds - s) <= 0.02; }));

    const auto j = bandTracksToJson (tracks);
    CHECK_NEAR (j["frameSeconds"].asNumber(), 0.1, 1.0e-9);
    REQUIRE (j["bands"].asArray().size() == 10);
    const auto& jb = j["bands"].asArray()[7];
    CHECK (jb["hz"].asNumber() == 4000.0);
    CHECK (jb["levelsDb"].asArray().size() == 120);
    CHECK (jb["levelsDb"].asArray()[115].isNull()); // the floor is null, as elsewhere in the CLI's JSON
    CHECK (jb["counts"]["onset"].asNumber() == static_cast<double> (b4k.counts[static_cast<size_t> (SceneEventType::Onset)]));
    CHECK (jb["events"].asArray().size() == b4k.events.size());
    CHECK (formatBandTracks (tracks).find ("4k") != std::string::npos);

    // At 22.05 kHz the bands stop below 0.4 fs (8.82 kHz), as --bands does.
    CHECK (bandTracks (ch, 22050.0).size() == 9);
}

TEST_CASE ("CLI analyze --glitches: the discontinuity detector over a file - clean programme reads nothing, an injected skip and NaN read once each")
{
    auto ch = programme();
    auto g = detectGlitches (ch, kFs);
    CHECK (g.counts == (std::array<int64_t, kNumDiscontinuityTypes> {}));
    // A 997 Hz tone on the right channel in the silence, 10.6 - 11.8 s, with
    // a skipped sample at 11.2 s, and a NaN on the left at 11.5 s. (Under
    // the pink bed the skip would not show: noise leaves a high residual.)
    for (int i = at (10.6); i < at (11.8); ++i)
        ch[1][static_cast<size_t> (i)] += static_cast<float> (0.2 * std::sin (kTwoPi * 997.0 * (i - at (10.6)) / kFs)
                                                              * std::min ({ 1.0, (i - at (10.6)) / 480.0, (at (11.8) - i) / 480.0 }));
    ch[1].erase (ch[1].begin() + at (11.2));
    ch[1].push_back (0.0f);
    ch[0][static_cast<size_t> (at (11.5))] = std::nanf ("");
    g = detectGlitches (ch, kFs);
    CHECK (g.counts[static_cast<size_t> (DiscontinuityType::Click)] == 1);
    CHECK (g.counts[static_cast<size_t> (DiscontinuityType::NonFinite)] == 1);
    CHECK (g.counts[static_cast<size_t> (DiscontinuityType::Dropout)] == 0);
    REQUIRE (g.events.size() == 2);
    const auto j = glitchesToJson (g);
    CHECK (j["total"].asNumber() == 2.0);
    const auto& list = j["list"].asArray();
    REQUIRE (list.size() == 2);
    CHECK (list[0]["type"].asString() == "click" && list[0]["channel"].asNumber() == 1.0);
    CHECK_NEAR (list[0]["seconds"].asNumber(), 11.2, 0.001);
    CHECK (list[1]["type"].asString() == "non-finite" && list[1]["channel"].asNumber() == 0.0);
    CHECK (formatGlitches (g).find ("1 click, 0 dropout, 1 non-finite, 0 dc-step") != std::string::npos);
}
