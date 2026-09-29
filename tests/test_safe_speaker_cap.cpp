// The safe speaker profile's bass cap (docs/11 E51): while the app plays an
// unplanned fallback to speakers, ProcessingChain::setSafeSpeakerBassCapDb
// scales the bass lift - the bass engine's boost plus the parametric EQ's
// positive low shelves and low bells - down together to the cap, instead of
// bypassing the bass engine (which left a preset's EQ low shelf in place).
// Within the cap nothing moves; the store is never written; the change is
// smoothed by the modules; a swapped-in chain keeps it.
#include "TestFramework.h"
#include "TestSignals.h"

#include "flub/common/Denormals.h"
#include "flub/engine/ProcessingChain.h"
#include "flub/io/PresetIO.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace flub;
using namespace flub::param;
using namespace flubtest;

namespace
{
constexpr double kFs = 48000.0;
constexpr int kBlock = 256;
constexpr float kToneAmp = 0.003f; // -50 dBFS per tone: below the bass engine's protection with 19 dB of lift

/** A linear Music strip: only the EQ and the bass engine can shape the tones. */
void linearStrip (ParameterStore& store)
{
    store.set (Mode, static_cast<float> (ModeValue::Music));
    store.set (BoostIntensity, 0.0f);
    for (int id : { Macro1, Macro2, Macro3, Macro4, Macro5 })
        store.set (id, 0.0f);
    for (int id : { AutoLevelOn, AutoPreampOn, GateOn, DynEqOn, ClarityOn, SaturationOn, SpatialOn, VirtualizerOn, CompressorOn, MaximizerOn })
        store.set (id, 0.0f);
    store.set (EqOn, 1.0f);
    store.set (BassOn, 1.0f);
}

/** The headset voicing: bass.boost +9 dB at 70 Hz, an EQ low shelf +6 dB at
    100 Hz and a +4 dB bell at 60 Hz (19 dB of lift towards DC). `flat`: the
    same strip with every one of those gains at 0. */
void bassyVoicing (ParameterStore& store, bool flat)
{
    linearStrip (store);
    store.set (BassBoostDb, flat ? 0.0f : 9.0f);
    store.set (BassBoostFreq, 70.0f);
    store.set (eq (1, EqFieldOn), 1.0f);
    store.set (eq (1, EqFieldType), 1.0f); // Low Shelf
    store.set (eq (1, EqFieldFreq), 100.0f);
    store.set (eq (1, EqFieldGain), flat ? 0.0f : 6.0f);
    store.set (eq (2, EqFieldOn), 1.0f);
    store.set (eq (2, EqFieldType), 0.0f); // Bell
    store.set (eq (2, EqFieldFreq), 60.0f);
    store.set (eq (2, EqFieldGain), flat ? 0.0f : 4.0f);
    store.set (eq (2, EqFieldQ), 0.7f);
}

void run (ProcessingChain& chain, Planar& buf)
{
    ScopedNoDenormals noDenormals;
    const int n = buf.numSamples();
    for (int pos = 0; pos < n; pos += kBlock)
        chain.process (buf.block (pos, std::min (kBlock, n - pos)));
}

struct ToneGains
{
    double lowDb = 0.0, midDb = 0.0; // 40 Hz and 1 kHz, output re input
};

/** 40 Hz + 1 kHz at kToneAmp for 1.5 s; the gains over the last 0.5 s. */
ToneGains measure (ParameterStore& store, float capDb)
{
    const int n = static_cast<int> (kFs * 1.5);
    Planar buf (2, n);
    for (int i = 0; i < n; ++i)
    {
        const double t = i / kFs;
        const auto v = static_cast<float> (kToneAmp * (std::sin (kTwoPi * 40.0 * t) + std::sin (kTwoPi * 1000.0 * t)));
        buf.ch[0][static_cast<size_t> (i)] = buf.ch[1][static_cast<size_t> (i)] = v;
    }
    ProcessingChain chain (store);
    chain.setSafeSpeakerBassCapDb (capDb);
    chain.prepare ({ kFs, kBlock, 2 });
    run (chain, buf);
    const int tail = static_cast<int> (kFs * 0.5);
    const float* x = buf.ch[0].data() + (n - tail);
    return { toDb (toneAmplitude (x, tail, 40.0, kFs) / kToneAmp), toDb (toneAmplitude (x, tail, 1000.0, kFs) / kToneAmp) };
}

/** The bass lift at 40 Hz re 1 kHz, against the same strip with the lifts at 0. */
double liftDb (float capDb)
{
    ParameterStore voiced, flat;
    bassyVoicing (voiced, false);
    bassyVoicing (flat, true);
    const auto v = measure (voiced, capDb), f = measure (flat, capDb);
    return (v.lowDb - v.midDb) - (f.lowDb - f.midDb);
}
} // namespace

TEST_CASE ("SafeSpeakerCap: bass engine boost + EQ low shelf + low bell (19 dB) are held to +3 dB at 40 Hz; the engine keeps running (E51)")
{
    const double open = liftDb (ProcessingChain::kNoBassCap);
    const double capped = liftDb (3.0f);
    std::cout << "    bass lift at 40 Hz re 1 kHz: headset voicing " << open << " dB, safe speaker cap " << capped << " dB\n";
    CHECK (open > 15.0);
    CHECK (capped <= 3.05);
    CHECK (capped > 2.0); // capped, not removed

    // What the chain applies: the gains scaled together, the modules on, the store untouched.
    ParameterStore store;
    bassyVoicing (store, false);
    ProcessingChain chain (store);
    chain.setSafeSpeakerBassCapDb (3.0f);
    CHECK (chain.getSafeSpeakerBassCapDb() == 3.0f);
    chain.prepare ({ kFs, kBlock, 2 });
    Planar buf (2, kBlock);
    run (chain, buf);
    const float sum = chain.effectiveValue (BassBoostDb) + chain.effectiveValue (eq (1, EqFieldGain)) + chain.effectiveValue (eq (2, EqFieldGain));
    CHECK (std::abs (sum - 3.0f) < 1.0e-4f);
    CHECK (std::abs (chain.effectiveValue (BassBoostDb) - 9.0f * 3.0f / 19.0f) < 1.0e-4f);
    CHECK (chain.effectiveValue (BassOn) == 1.0f);
    CHECK (store.get (BassBoostDb) == 9.0f);
    CHECK (store.get (eq (1, EqFieldGain)) == 6.0f);

    // A bell above 150 Hz, a cut and a high shelf are not bass lift.
    store.set (eq (3, EqFieldOn), 1.0f);
    store.set (eq (3, EqFieldType), 0.0f);
    store.set (eq (3, EqFieldFreq), 400.0f);
    store.set (eq (3, EqFieldGain), 5.0f);
    store.set (eq (4, EqFieldOn), 1.0f);
    store.set (eq (4, EqFieldType), 1.0f);
    store.set (eq (4, EqFieldFreq), 80.0f);
    store.set (eq (4, EqFieldGain), -4.0f);
    run (chain, buf);
    CHECK (chain.effectiveValue (eq (3, EqFieldGain)) == 5.0f);
    CHECK (chain.effectiveValue (eq (4, EqFieldGain)) == -4.0f);

    // Negative or non-finite requests: 0 dB, and no cap.
    chain.setSafeSpeakerBassCapDb (-2.0f);
    CHECK (chain.getSafeSpeakerBassCapDb() == 0.0f);
    chain.setSafeSpeakerBassCapDb (std::nanf (""));
    CHECK (chain.getSafeSpeakerBassCapDb() == ProcessingChain::kNoBassCap);
}

TEST_CASE ("SafeSpeakerCap: a preset within the cap is bit-identical; Bass Head is capped; a swapped-in chain keeps the cap (E51)")
{
    const int n = static_cast<int> (kFs * 0.6);
    Planar input (2, n);
    input.ch[0] = pinkNoise (n, 0.05f, 31);
    input.ch[1] = pinkNoise (n, 0.05f, 32);

    const auto render = [&] (const char* file, float capDb, float* boostOut)
    {
        ParameterStore store;
        preset::Preset p;
        std::string error;
        REQUIRE (preset::load (std::string (FLUB_PRESET_DIR) + "/" + file, p, error));
        preset::applyPresetToStore (p, store, Bank::A);
        ProcessingChain chain (store);
        chain.setSafeSpeakerBassCapDb (capDb);
        chain.prepare ({ kFs, kBlock, 2 });
        Planar buf = input;
        run (chain, buf);
        if (boostOut != nullptr)
            *boostOut = chain.effectiveValue (BassBoostDb);
        return buf;
    };

    // Bluetooth Headphones: bass.boost 2.5, no EQ lift: nothing moves.
    const Planar open = render ("device-bluetooth-headphones.json", ProcessingChain::kNoBassCap, nullptr);
    const Planar capped = render ("device-bluetooth-headphones.json", 3.0f, nullptr);
    CHECK (open.ch == capped.ch);

    float boostOpen = 0.0f, boostCapped = 0.0f;
    render ("music-bass-head.json", ProcessingChain::kNoBassCap, &boostOpen);
    render ("music-bass-head.json", 3.0f, &boostCapped);
    std::cout << "    Bass Head bass.boost as applied: " << boostOpen << " -> " << boostCapped << " dB\n";
    CHECK (boostOpen > 3.0f);
    CHECK (boostCapped <= 3.0f + 1.0e-4f);

    ParameterStore store;
    ProcessingChain previous (store), next (store);
    previous.setSafeSpeakerBassCapDb (3.0f);
    next.adoptGovernorState (previous);
    CHECK (next.getSafeSpeakerBassCapDb() == 3.0f);
}

TEST_CASE ("SafeSpeakerCap: engaging and releasing the cap mid-programme is click-free (E51)")
{
    ParameterStore store;
    bassyVoicing (store, false);
    ProcessingChain chain (store);
    chain.prepare ({ kFs, kBlock, 2 });
    const int n = static_cast<int> (kFs * 1.2);
    Planar buf (2, n);
    for (int i = 0; i < n; ++i)
    {
        const double t = i / kFs;
        buf.ch[0][static_cast<size_t> (i)] = buf.ch[1][static_cast<size_t> (i)] =
            static_cast<float> (0.02 * std::sin (kTwoPi * 50.0 * t) + 0.01 * std::sin (kTwoPi * 1000.0 * t));
    }
    const int engageAt = static_cast<int> (kFs * 0.4) / kBlock * kBlock, releaseAt = static_cast<int> (kFs * 0.8) / kBlock * kBlock;
    {
        ScopedNoDenormals noDenormals;
        for (int pos = 0; pos < n; pos += kBlock)
        {
            if (pos == engageAt)
                chain.setSafeSpeakerBassCapDb (3.0f);
            if (pos == releaseAt)
                chain.setSafeSpeakerBassCapDb (ProcessingChain::kNoBassCap);
            chain.process (buf.block (pos, std::min (kBlock, n - pos)));
        }
    }
    // The largest sample step around each change against the steady
    // uncapped programme before it (its 1 kHz tone sets the step).
    const auto maxStep = [&] (int from, int to)
    {
        double m = 0.0;
        for (int i = std::max (1, from); i < to; ++i)
            m = std::max (m, static_cast<double> (std::abs (buf.ch[0][static_cast<size_t> (i)] - buf.ch[0][static_cast<size_t> (i - 1)])));
        return m;
    };
    const double steady = maxStep (engageAt - static_cast<int> (kFs * 0.2), engageAt);
    const double atEngage = maxStep (engageAt, engageAt + static_cast<int> (kFs * 0.1));
    const double atRelease = maxStep (releaseAt, releaseAt + static_cast<int> (kFs * 0.1));
    std::cout << "    largest sample step: steady " << steady << ", engaging " << atEngage << ", releasing " << atRelease << "\n";
    CHECK (atEngage <= steady * 1.05);
    CHECK (atRelease <= steady * 1.05);
    // Released: the full lift is back.
    const int tail = static_cast<int> (kFs * 0.25);
    const double lowEnd = toneAmplitude (buf.ch[0].data() + (n - tail), tail, 50.0, kFs);
    const double lowCapped = toneAmplitude (buf.ch[0].data() + (releaseAt - tail), tail, 50.0, kFs);
    std::cout << "    50 Hz: capped " << toDb (lowCapped / 0.02) << " dB, released " << toDb (lowEnd / 0.02) << " dB\n";
    CHECK (toDb (lowEnd / lowCapped) > 10.0);
}
