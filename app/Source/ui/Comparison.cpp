#include "Comparison.h"

#include "Theme.h"

#include <algorithm>
#include <cmath>

namespace flub::app::ui
{
using namespace flub::param;
using Slot = EngineController::ComparisonSlot;

namespace
{
/** Values unchanged this long before their estimate is requested (a knob
    being dragged would otherwise queue a render per step). */
constexpr double kRequestAfterSeconds = 1.0;

Bank otherBank (Bank b) noexcept
{
    return b == Bank::A ? Bank::B : Bank::A;
}

size_t index (Bank b) noexcept
{
    return b == Bank::A ? 0u : 1u;
}

juce::String bankName (Bank b)
{
    return b == Bank::A ? "A" : "B";
}

PresetLoudnessEstimator::Variant variantFor (EngineController& controller, int strip, int listenBypassId = -1)
{
    PresetLoudnessEstimator::Variant v;
    v.channels = controller.getStripChannels (strip) == 8 ? 8 : 2;
    v.listenBypassId = listenBypassId;
    return v;
}

double wallSeconds()
{
    return juce::Time::getMillisecondCounterHiRes() * 0.001;
}
} // namespace

MatchTrims matchTrims (float firstGainLu, float secondGainLu) noexcept
{
    if (! (std::isfinite (firstGainLu) && std::isfinite (secondGainLu)))
        return {};
    const float quieter = std::min (firstGainLu, secondGainLu);
    const float limit = EngineController::kMaxComparisonTrimDb;
    return { std::max (-limit, quieter - firstGainLu), std::max (-limit, quieter - secondGainLu) };
}

std::vector<float> comparisonValues (const ParameterStore& store, Bank bank)
{
    std::vector<float> v (static_cast<size_t> (kNumParams));
    for (int i = 0; i < kNumParams; ++i)
        v[static_cast<size_t> (i)] = store.get (bank, i);
    v[static_cast<size_t> (BypassAll)] = 0.0f;
    v[static_cast<size_t> (LoudnessMatchBypass)] = 0.0f;
    return v;
}

std::optional<float> programmeLevel (EngineController& controller, int strip)
{
    if (strip < 0 || strip >= controller.getNumStrips())
        return {};
    auto& meters = controller.getChain (strip).meters();
    const float in = meters.inShortTermLufs.load (std::memory_order_relaxed);
    if (! std::isfinite (in) || in <= -60.0f)
        return {};
    const float inputGain = controller.getParams (strip).get (InputGainDb);
    return in - inputGain - meters.autoLevelGainDb.load (std::memory_order_relaxed);
}

// =============================================================================
// BankComparison
// =============================================================================
BankComparison::BankComparison (EngineController& c, EstimatorProvider provider)
    : controller (c), estimatorProvider (std::move (provider))
{
    clock = [] { return wallSeconds(); };
    enabled = controller.getSettings().getComparisonMatched();
    generation = controller.getEngineGeneration();
    controller.addListener (this);
    startTimerHz (kPollHz);
}

BankComparison::~BankComparison()
{
    stopTimer();
    controller.removeListener (this);
    if (listened != nullptr)
        listened->removeListener (listenerToken);
}

void BankComparison::setClock (std::function<double()> c)
{
    clock = c != nullptr ? std::move (c) : std::function<double()> ([] { return wallSeconds(); });
}

double BankComparison::now() const
{
    return clock();
}

std::shared_ptr<PresetLoudnessEstimator> BankComparison::estimator()
{
    auto e = estimatorProvider != nullptr ? estimatorProvider() : nullptr;
    if (e != listened)
    {
        if (listened != nullptr)
            listened->removeListener (listenerToken);
        listened = e;
        if (listened != nullptr)
            listenerToken = listened->addListener ([this]
                                                   {
                                                       bool changed = false;
                                                       for (int s = 0; s < controller.getNumStrips(); ++s)
                                                           if (strips[static_cast<size_t> (s)].pending)
                                                           {
                                                               applyMatch (s, true);
                                                               changed = true;
                                                           }
                                                       if (changed && onStatusChanged != nullptr)
                                                           onStatusChanged();
                                                   });
    }
    return e;
}

void BankComparison::setEnabled (bool shouldMatch)
{
    enabled = shouldMatch;
    if (! enabled)
        releaseAll();
    else
        for (int s = 0; s < controller.getNumStrips(); ++s)
            if (strips[static_cast<size_t> (s)].comparing)
                applyMatch (s, true);
    if (onStatusChanged != nullptr)
        onStatusChanged();
}

void BankComparison::engineControllerChanged (EngineController::Change change)
{
    if (change == EngineController::Change::Engine && controller.getEngineGeneration() != generation)
    {
        // Rebuilt chains (a device restart keeps the strips; a layout change
        // clears the trims itself): the live readings start over.
        generation = controller.getEngineGeneration();
        for (auto& st : strips)
        {
            st.liveGain = {};
            st.known = false;
        }
    }
    if (change == EngineController::Change::Preset || change == EngineController::Change::Parameters)
        poll(); // a preset load ends the comparison, a flip applies the trim, at once
}

void BankComparison::poll()
{
    const double t = now();
    bool changed = false;
    auto est = enabled ? estimator() : nullptr;

    for (int s = 0; s < controller.getNumStrips(); ++s)
    {
        auto& st = strips[static_cast<size_t> (s)];
        auto& store = controller.getParams (s);
        const auto bank = store.getActiveBank();
        const auto presetId = controller.getCurrentPresetId (s);
        const auto level = programmeLevel (controller, s);
        if (level.has_value() && (! st.levelKnown || std::abs (*level - st.levelLufs) >= kLiveLevelToleranceLu))
        {
            st.levelLufs = *level;
            st.levelKnown = true;
        }

        std::array<std::vector<float>, 2> values { comparisonValues (store, Bank::A), comparisonValues (store, Bank::B) };
        const auto variant = variantFor (controller, s);
        for (const auto b : { Bank::A, Bank::B })
        {
            // The values' own key (not the level's): a level that moves does not change a bank.
            const auto k = PresetLoudnessEstimator::keyOf (values[index (b)], PresetLoudnessEstimator::kDefaultLevelLufs, variant);
            if (k != st.key[index (b)])
            {
                st.key[index (b)] = k;
                st.unchangedSince[index (b)] = t;
            }
        }

        if (! st.known)
        {
            st.known = true;
            st.playing = bank;
            st.presetId = presetId;
            st.flippedAt = t;
            continue;
        }

        // A preset loaded into the strip: it plays at its own level.
        if (presetId != st.presetId)
        {
            st.presetId = presetId;
            if (st.comparing || controller.getComparisonTrimDb (s) != 0.0f)
            {
                release (s);
                changed = true;
            }
        }

        if (bank != st.playing)
        {
            flipped (s, bank);
            changed = true;
        }

        // Identical banks (copied): nothing to match.
        if (st.key[0] == st.key[1] && controller.getComparisonTrimDb (s) != 0.0f)
        {
            controller.setComparisonTrimDb (s, 0.0f);
            st.gapLu = 0.0f;
            st.gapKnown = true;
            changed = true;
        }

        // Estimates ahead of the next flip, once a bank's values have settled.
        if (est != nullptr && st.key[0] != st.key[1])
            for (const auto b : { Bank::A, Bank::B })
                if (t - st.unchangedSince[index (b)] >= kRequestAfterSeconds)
                    est->request (values[index (b)], st.levelLufs, false, variant);

        // The playing bank's gain on the real programme, once it has played
        // unchanged for the short-term window.
        const auto p = index (st.playing);
        if (level.has_value() && controller.isEnabled() && ! controller.isStripBypassed (s)
            && t - std::max (st.unchangedSince[p], st.flippedAt) >= kLiveSettleSeconds)
        {
            auto& meters = controller.getChain (s).meters();
            const float in = meters.inShortTermLufs.load (std::memory_order_relaxed);
            const float out = meters.shortTermLufs.load (std::memory_order_relaxed);
            if (std::isfinite (out) && out > -70.0f && std::isfinite (in))
            {
                // Out minus the programme's own level (the estimates' input),
                // averaged over the readings since the bank settled, so a
                // single loud or quiet stretch of the programme weighs less.
                auto& live = st.liveGain[p];
                if (! live.valid || live.key != st.key[p])
                {
                    live = {};
                    live.key = st.key[p];
                }
                ++live.readings;
                live.gainSum += static_cast<double> (out - *level);
                live.levelSum += static_cast<double> (*level);
                live.gainLu = static_cast<float> (live.gainSum / live.readings);
                live.levelLufs = static_cast<float> (live.levelSum / live.readings);
                live.valid = true;
            }
        }

        // Refined once from the live readings, then frozen until the next flip.
        if (enabled && st.comparing && ! st.refined && t - st.flippedAt >= kLiveSettleSeconds)
        {
            st.refined = true;
            const float before = controller.getComparisonTrimDb (s);
            applyMatch (s, true);
            changed = changed || before != controller.getComparisonTrimDb (s);
        }
    }
    if (changed && onStatusChanged != nullptr)
        onStatusChanged();
}

void BankComparison::flipped (int strip, Bank bank)
{
    auto& st = strips[static_cast<size_t> (strip)];
    st.playing = bank;
    st.flippedAt = now();
    st.comparing = true;
    st.refined = false;
    applyMatch (strip, true);
}

std::optional<float> BankComparison::gainOf (int strip, Bank bank, bool* live) const
{
    if (strip < 0 || strip >= controller.getNumStrips())
        return {};
    const auto& st = strips[static_cast<size_t> (strip)];
    const auto& l = st.liveGain[index (bank)];
    const auto& o = st.liveGain[index (otherBank (bank))];
    const bool bothLive = l.valid && o.valid && l.key == st.key[index (bank)] && o.key == st.key[index (otherBank (bank))]
                          && std::abs (l.levelLufs - o.levelLufs) <= kLiveLevelToleranceLu;
    if (live != nullptr)
        *live = bothLive;
    if (bothLive)
        return l.gainLu;
    if (listened == nullptr)
        return {};
    const auto values = comparisonValues (controller.getParams (strip), bank);
    return listened->find (values, st.levelLufs, variantFor (controller, strip));
}

void BankComparison::applyMatch (int strip, bool allowLive)
{
    auto& st = strips[static_cast<size_t> (strip)];
    st.pending = false;
    if (! enabled || ! st.comparing)
        return;
    if (st.key[0] == st.key[1])
    {
        controller.setComparisonTrimDb (strip, 0.0f);
        st.gapLu = 0.0f;
        st.gapKnown = true;
        st.live = false;
        return;
    }
    auto est = estimator();
    bool liveP = false;
    const auto p = st.playing, o = otherBank (p);
    std::optional<float> gp, go;
    if (allowLive)
        gp = gainOf (strip, p, &liveP);
    if (allowLive && liveP)
    {
        go = gainOf (strip, o);
    }
    else if (est != nullptr)
    {
        auto& store = controller.getParams (strip);
        const auto variant = variantFor (controller, strip);
        const auto vp = comparisonValues (store, p), vo = comparisonValues (store, o);
        gp = est->find (vp, st.levelLufs, variant);
        go = est->find (vo, st.levelLufs, variant);
        if (! gp.has_value() || ! go.has_value())
        {
            // Not estimated yet (about 0.2 s each): unmatched until they arrive.
            est->request (vp, st.levelLufs, true, variant);
            est->request (vo, st.levelLufs, true, variant);
            st.pending = true;
            st.gapKnown = false;
            controller.setComparisonTrimDb (strip, 0.0f);
            return;
        }
    }
    if (! gp.has_value() || ! go.has_value() || ! std::isfinite (*gp) || ! std::isfinite (*go))
    {
        controller.setComparisonTrimDb (strip, 0.0f);
        st.gapKnown = false;
        return;
    }
    st.live = liveP;
    st.gapLu = *gp - *go;
    st.gapKnown = true;
    controller.setComparisonTrimDb (strip, matchTrims (*gp, *go).first);
}

void BankComparison::release (int strip)
{
    auto& st = strips[static_cast<size_t> (strip)];
    st.comparing = false;
    st.pending = false;
    st.refined = false;
    st.live = false;
    controller.setComparisonTrimDb (strip, 0.0f);
}

void BankComparison::releaseAll()
{
    for (int s = 0; s < controller.getNumStrips(); ++s)
    {
        strips[static_cast<size_t> (s)].pending = false;
        controller.setComparisonTrimDb (s, 0.0f);
    }
}

BankComparison::Status BankComparison::getStatus (int strip) const
{
    Status r;
    if (strip < 0 || strip >= controller.getNumStrips())
        return r;
    const auto& st = strips[static_cast<size_t> (strip)];
    r.banksDiffer = st.key[0] != st.key[1];
    r.comparing = enabled && st.comparing;
    r.pending = enabled && st.pending;
    r.live = st.live;
    r.trimDb = controller.getComparisonTrimDb (strip);
    r.gapLu = st.gapLu;
    r.gapKnown = st.gapKnown && st.comparing;
    r.playing = st.playing;
    return r;
}

juce::String BankComparison::describe (const Status& s)
{
    const auto p = bankName (s.playing), o = bankName (otherBank (s.playing));
    if (! s.banksDiffer)
        return "A and B hold the same sound.";
    if (! s.comparing)
        return "Loudness-matched A/B: the louder of A and B is turned down to the quieter while you compare (from the first switch).";
    if (s.pending)
        return "Loudness-matched A/B: estimating how loud " + p + " and " + o + " are...";
    if (! s.gapKnown)
        return "Loudness-matched A/B: the loudness of " + p + " and " + o + " cannot be measured now (silence?).";
    const auto source = s.live ? juce::String ("measured on what is playing") : juce::String ("estimated");
    const auto gap = juce::String (std::abs (s.gapLu), 1);
    if (s.trimDb < -0.05f)
        return p + " plays " + juce::String (-s.trimDb, 1) + " dB down: it is " + gap + " LU louder than " + o + " (" + source
               + "). Only the louder side is ever turned down.";
    return p + " is the quieter (" + gap + " LU under " + o + ", " + source + "): it plays at its own level, " + o
           + " is turned down to it.";
}

juce::String BankComparison::shortText (const Status& s)
{
    if (! s.banksDiffer || ! s.comparing)
        return {};
    if (s.pending)
        return "matching...";
    if (s.trimDb < -0.05f)
        return bankName (s.playing) + " " + Theme::formatSignedDb (s.trimDb, 1) + " dB";
    return s.gapKnown ? juce::String ("matched") : juce::String();
}

// =============================================================================
// ListenMatch
// =============================================================================
ListenMatch::ListenMatch (EngineController& c, EstimatorProvider provider)
    : controller (c), estimatorProvider (std::move (provider))
{
    clock = [] { return wallSeconds(); };
    enabled = controller.getSettings().getComparisonMatched();
}

ListenMatch::~ListenMatch()
{
    stopTimer();
    if (listened != nullptr)
        listened->removeListener (listenerToken);
    reset();
}

void ListenMatch::setClock (std::function<double()> c)
{
    clock = c != nullptr ? std::move (c) : std::function<double()> ([] { return wallSeconds(); });
}

double ListenMatch::now() const
{
    return clock();
}

std::shared_ptr<PresetLoudnessEstimator> ListenMatch::estimator()
{
    auto e = estimatorProvider != nullptr ? estimatorProvider() : nullptr;
    if (e != listened)
    {
        if (listened != nullptr)
            listened->removeListener (listenerToken);
        listened = e;
        if (listened != nullptr)
            listenerToken = listened->addListener ([this]
                                                   {
                                                       if (active)
                                                           update();
                                                   });
    }
    return e;
}

void ListenMatch::setEnabled (bool shouldMatch)
{
    enabled = shouldMatch;
    if (! enabled)
        reset();
}

void ListenMatch::prepare (int s, int enableParamId)
{
    if (! enabled || s < 0 || s >= controller.getNumStrips())
        return;
    if (auto est = estimator())
    {
        auto& store = controller.getParams (s);
        const auto values = comparisonValues (store, store.getActiveBank());
        preparedLevel = programmeLevel (controller, s).value_or (PresetLoudnessEstimator::kDefaultLevelLufs);
        est->request (values, preparedLevel, true, variantFor (controller, s));
        est->request (values, preparedLevel, true, variantFor (controller, s, enableParamId));
    }
}

void ListenMatch::listen (int s, int enableParamId, bool isHeld)
{
    if (! enabled)
        return;
    if (isHeld)
    {
        if (active && (s != strip || enableParamId != moduleId))
            reset(); // another module (or strip): a new session
        const bool sameSession = active && s == strip && enableParamId == moduleId;
        strip = s;
        moduleId = enableParamId;
        active = true;
        held = true;
        // The level of the session is the one at its first hold (or at the
        // hover before it): the estimates stay put while the programme moves.
        if (! sameSession)
        {
            const float hovered = preparedLevel;
            prepare (s, enableParamId);
            if (auto est = estimator(); est != nullptr)
            {
                auto& store = controller.getParams (s);
                const auto values = comparisonValues (store, store.getActiveBank());
                if (est->find (values, hovered, variantFor (controller, s)).has_value()
                    && est->find (values, hovered, variantFor (controller, s, enableParamId)).has_value())
                    preparedLevel = hovered;
            }
            sessionLevel = preparedLevel;
        }
    }
    else
    {
        if (! active || s != strip)
            return;
        held = false;
        releasedAt = now();
    }
    update();
    if (active && ! isTimerRunning())
        startTimerHz (10);
}

void ListenMatch::update()
{
    if (! active || strip < 0 || strip >= controller.getNumStrips())
        return;
    auto est = estimator();
    if (est == nullptr)
        return;
    auto& store = controller.getParams (strip);
    const auto values = comparisonValues (store, store.getActiveBank());
    const float level = sessionLevel;
    const auto with = est->find (values, level, variantFor (controller, strip));
    const auto without = est->find (values, level, variantFor (controller, strip, moduleId));
    if (! with.has_value() || ! without.has_value())
    {
        est->request (values, level, true, variantFor (controller, strip));
        est->request (values, level, true, variantFor (controller, strip, moduleId));
        return; // the trim follows when they arrive
    }
    if (! std::isfinite (*with) || ! std::isfinite (*without))
    {
        gap.reset();
        return;
    }
    gap = *without - *with;
    const auto trims = matchTrims (*with, *without);
    appliedTrim = held ? trims.second : trims.first;
    controller.setComparisonTrimDb (strip, appliedTrim, Slot::Listen);
}

void ListenMatch::poll()
{
    if (! active)
    {
        stopTimer();
        return;
    }
    if (held || now() - releasedAt < kSessionEndSeconds)
        return;
    // The session is over: back to 0 dB, gently.
    appliedTrim = std::min (0.0f, appliedTrim + kReleaseDbPerSec * 0.1f);
    if (appliedTrim >= -0.01f)
    {
        reset();
        return;
    }
    controller.setComparisonTrimDb (strip, appliedTrim, Slot::Listen);
}

void ListenMatch::reset()
{
    if (strip >= 0 && strip < controller.getNumStrips())
        controller.setComparisonTrimDb (strip, 0.0f, Slot::Listen);
    appliedTrim = 0.0f;
    active = held = false;
    gap.reset();
    strip = moduleId = -1;
    stopTimer();
}

// =============================================================================
// AbxTest
// =============================================================================
AbxTest::AbxTest (EngineController& c, int s, int numTrials, int64_t seed)
    : controller (c), strip (s), trials (juce::jmax (1, numTrials))
{
    bankBefore = controller.getActiveBank (strip);
    juce::Random random (seed);
    for (int i = 0; i < trials; ++i)
        hidden.push_back (random.nextBool() ? Bank::A : Bank::B);
    play (Choice::X);
}

AbxTest::~AbxTest()
{
    if (strip < controller.getNumStrips())
        controller.setActiveBank (bankBefore, strip);
}

Bank AbxTest::hiddenBank (int trial) const
{
    return hidden[static_cast<size_t> (juce::jlimit (0, trials - 1, trial))];
}

void AbxTest::play (Choice choice)
{
    playing = choice;
    const auto bank = choice == Choice::A ? Bank::A : choice == Choice::B ? Bank::B : hiddenBank (getTrial());
    if (strip < controller.getNumStrips())
        controller.setActiveBank (bank, strip);
}

void AbxTest::answer (bool xIsA)
{
    if (isFinished())
        return;
    if ((hiddenBank (getTrial()) == Bank::A) == xIsA)
        ++correct;
    answers.push_back (xIsA);
    if (! isFinished())
        play (Choice::X); // the next trial starts on its X
}

double AbxTest::pValue (int k, int n)
{
    // P(at least k right out of n by guessing) = sum_{i >= k} C(n, i) / 2^n.
    if (n <= 0)
        return 1.0;
    k = juce::jlimit (0, n, k);
    double sum = 0.0, c = 1.0; // C(n, 0)
    for (int i = 0; i <= n; ++i)
    {
        if (i >= k)
            sum += c;
        c = c * static_cast<double> (n - i) / static_cast<double> (i + 1);
    }
    return sum / std::pow (2.0, n);
}

juce::String AbxTest::describeResult() const
{
    const double p = pValue (correct, getTrial());
    juce::String t;
    t << correct << " of " << getTrial() << " right: p = " << juce::String (p, p < 0.01 ? 3 : 2) << ". ";
    if (getTrial() < trials)
        return t + "Not finished.";
    if (p < 0.05)
        t << "You can reliably tell A from B at the same loudness.";
    else
        t << "No reliable difference: you might as well have guessed (at matched loudness, a louder setting cannot win).";
    return t;
}
} // namespace flub::app::ui
