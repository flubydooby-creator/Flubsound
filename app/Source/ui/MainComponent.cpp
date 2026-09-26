#include "MainComponent.h"

#include "flub/common/Math.h"
#include "flub/engine/Parameters.h"
#include "platform/PlatformBridge.h"

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <cmath>

namespace flub::app::ui
{
using namespace flub::param;

namespace
{
namespace Palette
{
const juce::Colour background { 0xff0f1115 };
const juce::Colour panel { 0xff171a21 };
const juce::Colour panelEdge { 0xff262b36 };
const juce::Colour accent { 0xff22d3c5 };
const juce::Colour accentDeep { 0xff0e7c74 };
const juce::Colour blue { 0xff3b82f6 };
const juce::Colour text { 0xffe5e7eb };
const juce::Colour dim { 0xff8b93a1 };
const juce::Colour warn { 0xfff59e0b };
const juce::Colour danger { 0xffef4444 };
} // namespace Palette

juce::Font makeFont (float height, bool bold = false)
{
    auto options = juce::FontOptions (height);
    return juce::Font (bold ? options.withStyle ("Bold") : options);
}

float unitFromDb (float db, float minDb, float maxDb)
{
    return juce::jlimit (0.0f, 1.0f, (db - minDb) / (maxDb - minDb));
}

juce::String formatDb (float db, const char* unit = " dB")
{
    return db <= -99.0f ? juce::String ("-inf") + unit : juce::String (db, 1) + unit;
}

void drawPanel (juce::Graphics& g, juce::Rectangle<float> r)
{
    g.setColour (Palette::panel);
    g.fillRoundedRectangle (r, 10.0f);
    g.setColour (Palette::panelEdge);
    g.drawRoundedRectangle (r.reduced (0.5f), 10.0f, 1.0f);
}
} // namespace

// =============================================================================
// Meters
// =============================================================================
class MainComponent::MeterPanel final : public juce::Component
{
public:
    void update (const flub::MeterBus& bus, float masterGrDb, bool active)
    {
        auto follow = [active] (float& shown, float value, float fallPerTick)
        {
            if (! active)
                value = -100.0f;
            shown = std::max (value, shown - fallPerTick);
        };

        for (size_t c = 0; c < 2; ++c)
        {
            follow (inPeak[c], bus.inPeakDb[c].load (std::memory_order_relaxed), 1.5f);
            follow (inRms[c], bus.inRmsDb[c].load (std::memory_order_relaxed), 1.0f);
            follow (outPeak[c], bus.outPeakDb[c].load (std::memory_order_relaxed), 1.5f);
            follow (outRms[c], bus.outRmsDb[c].load (std::memory_order_relaxed), 1.0f);
        }

        // Gain reductions are <= 0; show them "falling back" towards 0.
        auto followGr = [active] (float& shown, float value)
        {
            if (! active)
                value = 0.0f;
            shown = std::min (value, shown + 0.6f);
        };
        followGr (compGr, bus.compGainReductionDb.load (std::memory_order_relaxed));
        followGr (maxGr, bus.maxGainReductionDb.load (std::memory_order_relaxed));
        followGr (masterGr, masterGrDb);

        momentary = bus.momentaryLufs.load (std::memory_order_relaxed);
        shortTerm = bus.shortTermLufs.load (std::memory_order_relaxed);
        integrated = bus.integratedLufs.load (std::memory_order_relaxed);
        range = bus.loudnessRangeLu.load (std::memory_order_relaxed);
        truePeakMax = bus.outTruePeakMaxDb.load (std::memory_order_relaxed);
        correlation = bus.correlation.load (std::memory_order_relaxed);
        width = bus.effectiveWidth.load (std::memory_order_relaxed);
        governor = bus.governorScale.load (std::memory_order_relaxed);
        autoLevel = bus.autoLevelGainDb.load (std::memory_order_relaxed);
        latencyMs = bus.latencyMs.load (std::memory_order_relaxed);
        isActive = active;
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().toFloat();
        drawPanel (g, bounds);
        auto r = bounds.reduced (16.0f, 12.0f);

        g.setColour (Palette::dim);
        g.setFont (makeFont (12.0f, true));
        g.drawText (isActive ? "LEVELS" : "LEVELS  (no signal on this strip)", r.removeFromTop (18.0f), juce::Justification::centredLeft);
        r.removeFromTop (6.0f);

        auto bars = r.removeFromLeft (250.0f);
        const auto scaleArea = bars.removeFromLeft (26.0f);
        const auto labelArea = bars.removeFromBottom (18.0f);
        const float barWidth = 22.0f, gap = 8.0f;

        // dB scale (-60 .. +3 dBFS)
        g.setFont (makeFont (10.0f));
        for (const float db : { 0.0f, -6.0f, -12.0f, -24.0f, -36.0f, -48.0f, -60.0f })
        {
            const float y = bars.getBottom() - unitFromDb (db, -60.0f, 3.0f) * bars.getHeight();
            g.setColour (Palette::dim.withAlpha (0.8f));
            g.drawText (juce::String (juce::roundToInt (db)), scaleArea.withY (y - 6.0f).withHeight (12.0f), juce::Justification::centredRight);
            g.setColour (Palette::panelEdge);
            g.drawHorizontalLine (juce::roundToInt (y), bars.getX(), bars.getX() + 6.0f * (barWidth + gap));
        }

        float x = bars.getX() + 4.0f;
        auto drawLevel = [&] (float peakDb, float rmsDb, const juce::String& label)
        {
            const auto bar = juce::Rectangle<float> (x, bars.getY(), barWidth, bars.getHeight());
            g.setColour (Palette::background);
            g.fillRoundedRectangle (bar, 3.0f);
            const float rmsH = unitFromDb (rmsDb, -60.0f, 3.0f) * bar.getHeight();
            g.setGradientFill (juce::ColourGradient (Palette::accent, bar.getX(), bar.getY(), Palette::accentDeep, bar.getX(), bar.getBottom(), false));
            g.fillRoundedRectangle (bar.withTop (bar.getBottom() - rmsH), 3.0f);
            const float peakY = bar.getBottom() - unitFromDb (peakDb, -60.0f, 3.0f) * bar.getHeight();
            g.setColour (peakDb > -0.5f ? Palette::danger : Palette::text);
            g.fillRect (bar.getX(), peakY - 1.0f, bar.getWidth(), 2.0f);
            g.setColour (Palette::dim);
            g.drawText (label, labelArea.withX (x - 6.0f).withWidth (barWidth + 12.0f), juce::Justification::centred);
            x += barWidth + gap;
        };
        drawLevel (inPeak[0], inRms[0], "IN L");
        drawLevel (inPeak[1], inRms[1], "IN R");
        x += gap * 2.0f;
        drawLevel (outPeak[0], outRms[0], "OUT L");
        drawLevel (outPeak[1], outRms[1], "OUT R");

        // Gain reduction bars (0 .. -20 dB, drawn downwards)
        r.removeFromLeft (10.0f);
        auto grArea = r.removeFromLeft (170.0f);
        g.setColour (Palette::dim);
        g.setFont (makeFont (12.0f, true));
        g.drawText ("GAIN REDUCTION", grArea.removeFromTop (16.0f), juce::Justification::centredLeft);
        grArea.removeFromTop (4.0f);
        auto drawGr = [&] (float grDb, const juce::String& label)
        {
            auto row = grArea.removeFromTop (30.0f);
            g.setColour (Palette::dim);
            g.setFont (makeFont (11.0f));
            g.drawText (label, row.removeFromTop (13.0f), juce::Justification::centredLeft);
            const auto bar = row.removeFromTop (12.0f);
            g.setColour (Palette::background);
            g.fillRoundedRectangle (bar, 3.0f);
            const float w = unitFromDb (-grDb, 0.0f, 20.0f) * bar.getWidth();
            g.setColour (Palette::warn);
            g.fillRoundedRectangle (bar.withWidth (w), 3.0f);
            g.setColour (Palette::text);
            g.drawText (juce::String (grDb, 1), bar.reduced (4.0f, 0.0f), juce::Justification::centredRight);
            grArea.removeFromTop (4.0f);
        };
        drawGr (compGr, "Compressor");
        drawGr (maxGr, "Maximizer");
        drawGr (masterGr, "Master safety limiter");
        drawGr ((governor - 1.0f) * 20.0f, "Safety governor (" + juce::String (juce::roundToInt (governor * 100.0f)) + "%)");

        // Readouts
        r.removeFromLeft (16.0f);
        g.setColour (Palette::dim);
        g.setFont (makeFont (12.0f, true));
        g.drawText ("LOUDNESS & IMAGE", r.removeFromTop (16.0f), juce::Justification::centredLeft);
        r.removeFromTop (4.0f);
        auto readout = [&] (const juce::String& name, const juce::String& value)
        {
            auto row = r.removeFromTop (22.0f);
            g.setColour (Palette::dim);
            g.setFont (makeFont (13.0f));
            g.drawText (name, row.removeFromLeft (130.0f), juce::Justification::centredLeft);
            g.setColour (Palette::text);
            g.setFont (makeFont (14.0f, true));
            g.drawText (value, row, juce::Justification::centredLeft);
        };
        readout ("Momentary", formatDb (momentary, " LUFS"));
        readout ("Short-term", formatDb (shortTerm, " LUFS"));
        readout ("Integrated", formatDb (integrated, " LUFS"));
        readout ("Loudness range", juce::String (range, 1) + " LU");
        readout ("True peak (max)", formatDb (truePeakMax, " dBTP"));
        readout ("Correlation", juce::String (correlation, 2));
        readout ("Width", juce::String (juce::roundToInt (width * 100.0f)) + " %");
        readout ("Auto level", juce::String (autoLevel, 1) + " dB");
        readout ("Strip latency", juce::String (latencyMs, 1) + " ms");
    }

private:
    std::array<float, 2> inPeak { -100.0f, -100.0f }, inRms { -100.0f, -100.0f }, outPeak { -100.0f, -100.0f }, outRms { -100.0f, -100.0f };
    float compGr = 0.0f, maxGr = 0.0f, masterGr = 0.0f;
    float momentary = -160.0f, shortTerm = -160.0f, integrated = -160.0f, range = 0.0f, truePeakMax = -160.0f;
    float correlation = 1.0f, width = 1.0f, governor = 1.0f, autoLevel = 0.0f, latencyMs = 0.0f;
    bool isActive = false;
};

// =============================================================================
// Spectrum analyser (consumes the chain's pre / post analyser taps)
// =============================================================================
class MainComponent::SpectrumView final : public juce::Component
{
public:
    static constexpr int kOrder = 11;
    static constexpr int kSize = 1 << kOrder;

    SpectrumView()
        : fft (kOrder), window (static_cast<size_t> (kSize), juce::dsp::WindowingFunction<float>::hann, false)
    {
        for (auto& h : history)
            h.assign (static_cast<size_t> (kSize), 0.0f);
        for (auto& s : spectrumDb)
            s.assign (static_cast<size_t> (kSize / 2), -120.0f);
        fftData.assign (static_cast<size_t> (2 * kSize), 0.0f);
        scratch.assign (4096, 0.0f);
    }

    void reset()
    {
        for (auto& h : history)
            std::fill (h.begin(), h.end(), 0.0f);
        for (auto& s : spectrumDb)
            std::fill (s.begin(), s.end(), -120.0f);
        writePos.fill (0);
        fresh.fill (0);
    }

    /** The UI is the single consumer of both rings. */
    void pull (flub::AnalyzerTaps& taps)
    {
        drain (taps.pre, 0);
        drain (taps.post, 1);
    }

    void compute (double sampleRate)
    {
        sr = sampleRate > 0.0 ? sampleRate : 48000.0;
        for (size_t k = 0; k < 2; ++k)
        {
            if (fresh[k] >= kSize / 4)
            {
                analyse (k);
                fresh[k] = 0;
            }
        }
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().toFloat();
        drawPanel (g, bounds);
        auto header = bounds.reduced (16.0f, 10.0f).removeFromTop (18.0f);
        g.setColour (Palette::dim);
        g.setFont (makeFont (12.0f, true));
        g.drawText ("SPECTRUM", header, juce::Justification::centredLeft);
        g.setFont (makeFont (11.0f));
        g.drawText ("grey: input   colour: output   (+3 dB/oct tilt)", header, juce::Justification::centredRight);

        plot = bounds.reduced (16.0f, 10.0f).withTrimmedTop (24.0f).withTrimmedBottom (14.0f);

        g.setFont (makeFont (10.0f));
        for (const double f : { 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0 })
        {
            const float x = xForFrequency (f);
            g.setColour (Palette::panelEdge);
            g.drawVerticalLine (juce::roundToInt (x), plot.getY(), plot.getBottom());
            g.setColour (Palette::dim);
            g.drawText (f >= 1000.0 ? juce::String (f / 1000.0, 0) + "k" : juce::String (juce::roundToInt (f)),
                        juce::Rectangle<float> (x - 20.0f, plot.getBottom() + 1.0f, 40.0f, 12.0f), juce::Justification::centred);
        }
        for (float db = -12.0f; db > kMinDb; db -= 12.0f)
        {
            g.setColour (Palette::panelEdge);
            g.drawHorizontalLine (juce::roundToInt (yForDb (db)), plot.getX(), plot.getRight());
        }

        const auto pre = buildPath (0);
        const auto post = buildPath (1);
        g.setColour (Palette::dim.withAlpha (0.55f));
        g.strokePath (pre, juce::PathStrokeType (1.0f));

        auto filled = post;
        if (! filled.isEmpty())
        {
            filled.lineTo (plot.getRight(), plot.getBottom());
            filled.lineTo (plot.getX(), plot.getBottom());
            filled.closeSubPath();
            g.setGradientFill (juce::ColourGradient (Palette::accent.withAlpha (0.35f), 0.0f, plot.getY(), Palette::blue.withAlpha (0.05f), 0.0f,
                                                     plot.getBottom(), false));
            g.fillPath (filled);
        }
        g.setColour (Palette::accent);
        g.strokePath (post, juce::PathStrokeType (1.6f));
    }

private:
    static constexpr float kMinDb = -96.0f, kMaxDb = 6.0f;

    void drain (flub::SpscRing<float>& ring, size_t k)
    {
        // Only the most recent kSize samples matter; drop anything older.
        const size_t available = ring.available();
        if (available > static_cast<size_t> (kSize))
            ring.skip (available - static_cast<size_t> (kSize));

        for (int guard = 0; guard < 16; ++guard)
        {
            const size_t n = ring.pop (scratch.data(), scratch.size());
            if (n == 0)
                break;
            auto& h = history[k];
            for (size_t i = 0; i < n; ++i)
            {
                h[static_cast<size_t> (writePos[k])] = scratch[i];
                writePos[k] = (writePos[k] + 1) & (kSize - 1);
            }
            fresh[k] += static_cast<int> (n);
        }
    }

    void analyse (size_t k)
    {
        const auto& h = history[k];
        for (int i = 0; i < kSize; ++i)
            fftData[static_cast<size_t> (i)] = h[static_cast<size_t> ((writePos[k] + i) & (kSize - 1))];
        std::fill (fftData.begin() + kSize, fftData.end(), 0.0f);
        window.multiplyWithWindowingTable (fftData.data(), static_cast<size_t> (kSize));
        fft.performFrequencyOnlyForwardTransform (fftData.data());

        const float norm = 4.0f / static_cast<float> (kSize); // Hann-windowed sine -> amplitude
        auto& s = spectrumDb[k];
        for (size_t bin = 1; bin < s.size(); ++bin)
        {
            const float db = flub::gainToDb (fftData[bin] * norm, -140.0f);
            s[bin] = std::max (db, s[bin] - 1.5f);
        }
    }

    float xForFrequency (double f) const
    {
        return plot.getX() + static_cast<float> (std::log (f / 20.0) / std::log (1000.0)) * plot.getWidth();
    }

    float yForDb (float db) const
    {
        return plot.getBottom() - unitFromDb (db, kMinDb, kMaxDb) * plot.getHeight();
    }

    juce::Path buildPath (size_t k) const
    {
        juce::Path p;
        const auto& s = spectrumDb[k];
        float lastX = -1.0e9f, pending = -200.0f;
        bool started = false;
        for (size_t bin = 1; bin < s.size(); ++bin)
        {
            const double f = static_cast<double> (bin) * sr / static_cast<double> (kSize);
            if (f < 20.0)
                continue;
            if (f > 20000.0)
                break;
            const float tilt = 3.0f * static_cast<float> (std::log2 (f / 1000.0));
            pending = std::max (pending, s[bin] + tilt);
            const float x = xForFrequency (f);
            if (started && x - lastX < 1.5f)
                continue;
            const float y = yForDb (pending);
            if (started)
                p.lineTo (x, y);
            else
                p.startNewSubPath (x, y);
            started = true;
            lastX = x;
            pending = -200.0f;
        }
        return p;
    }

    juce::dsp::FFT fft;
    juce::dsp::WindowingFunction<float> window;
    std::array<std::vector<float>, 2> history, spectrumDb;
    std::array<int, 2> writePos {}, fresh {};
    std::vector<float> fftData, scratch;
    juce::Rectangle<float> plot;
    double sr = 48000.0;
};

// =============================================================================
// MainComponent
// =============================================================================
MainComponent::MainComponent (EngineController& c)
    : controller (c)
{
    meters = std::make_unique<MeterPanel>();
    spectrum = std::make_unique<SpectrumView>();
    addAndMakeVisible (*meters);
    addAndMakeVisible (*spectrum);

    auto styleToggle = [] (juce::TextButton& b)
    {
        b.setColour (juce::TextButton::buttonColourId, Palette::panel);
        b.setColour (juce::TextButton::buttonOnColourId, Palette::accentDeep);
        b.setColour (juce::TextButton::textColourOffId, Palette::dim);
        b.setColour (juce::TextButton::textColourOnId, Palette::text);
    };

    enableButton.onClick = [this] { controller.toggleEnabled(); };
    styleToggle (enableButton);
    addAndMakeVisible (enableButton);

    audioSettingsButton.setButtonText ("Audio Settings...");
    audioSettingsButton.onClick = [this] { openAudioSettings(); };
    styleToggle (audioSettingsButton);
    addAndMakeVisible (audioSettingsButton);

    for (auto* b : { &musicButton, &gamingButton })
    {
        styleToggle (*b);
        addAndMakeVisible (*b);
    }
    musicButton.onClick = [this] { controller.setMode (ModeValue::Music); };
    gamingButton.onClick = [this] { controller.setMode (ModeValue::Gaming); };

    auto percent = [] (double v) { return juce::String (juce::roundToInt (v * 100.0)) + "%"; };
    auto fromPercent = [] (const juce::String& t) { return t.retainCharacters ("0123456789.").getDoubleValue() / 100.0; };

    boostSlider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    boostSlider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 80, 24);
    boostSlider.setRange (0.0, 1.0, 0.01);
    boostSlider.textFromValueFunction = percent;
    boostSlider.valueFromTextFunction = fromPercent;
    boostSlider.setColour (juce::Slider::rotarySliderFillColourId, Palette::accent);
    boostSlider.setColour (juce::Slider::rotarySliderOutlineColourId, Palette::panelEdge);
    boostSlider.setColour (juce::Slider::thumbColourId, Palette::text);
    boostSlider.onValueChange = [this] { controller.setBoost (static_cast<float> (boostSlider.getValue())); };
    addAndMakeVisible (boostSlider);

    boostLabel.setText ("BOOST INTENSITY", juce::dontSendNotification);
    boostLabel.setJustificationType (juce::Justification::centred);
    boostLabel.setFont (makeFont (13.0f, true));
    boostLabel.setColour (juce::Label::textColourId, Palette::dim);
    addAndMakeVisible (boostLabel);

    for (size_t i = 0; i < macroSliders.size(); ++i)
    {
        auto& s = macroSliders[i];
        s.setSliderStyle (juce::Slider::LinearHorizontal);
        s.setTextBoxStyle (juce::Slider::TextBoxRight, false, 48, 20);
        s.setRange (0.0, 1.0, 0.01);
        s.textFromValueFunction = percent;
        s.valueFromTextFunction = fromPercent;
        s.setColour (juce::Slider::trackColourId, Palette::accent);
        s.setColour (juce::Slider::backgroundColourId, Palette::background);
        const int id = Macro1 + static_cast<int> (i);
        s.onValueChange = [this, id, &s] { controller.getSelectedParams().set (id, static_cast<float> (s.getValue())); };
        addAndMakeVisible (s);

        auto& l = macroLabels[i];
        l.setFont (makeFont (13.0f));
        l.setColour (juce::Label::textColourId, Palette::text);
        addAndMakeVisible (l);
    }

    presetBox.setTextWhenNothingSelected ("Select a preset");
    presetBox.setTextWhenNoChoicesAvailable ("No presets installed");
    presetBox.onChange = [this]
    {
        const int item = presetBox.getSelectedId();
        if (item > 0 && item <= static_cast<int> (presetIdsByItem.size()))
        {
            juce::String error;
            controller.loadPreset (presetIdsByItem[static_cast<size_t> (item - 1)], -1, error);
        }
    };
    addAndMakeVisible (presetBox);

    previousPresetButton.onClick = [this] { controller.previousPreset(); };
    nextPresetButton.onClick = [this] { controller.nextPreset(); };
    abButton.onClick = [this] { controller.toggleAB(); };
    copyAbButton.onClick = [this] { controller.copyActiveToOtherBank(); };
    for (auto* b : { &previousPresetButton, &nextPresetButton, &abButton, &copyAbButton })
    {
        styleToggle (*b);
        addAndMakeVisible (*b);
    }

    rebuildStripButtons();
    refreshPresetList();
    refreshControls();
    updateStatusText();

    controller.addListener (this);
    setSize (1280, 820);
    startTimerHz (30);
}

MainComponent::~MainComponent()
{
    controller.removeListener (this);
    stopTimer();
}

void MainComponent::rebuildStripButtons()
{
    for (auto* b : stripButtons)
        removeChildComponent (b);
    stripButtons.clear();

    for (int i = 0; i < controller.getNumStrips(); ++i)
    {
        const int channels = controller.getStripChannels (i);
        auto label = controller.getStripName (i).toUpperCase();
        if (channels >= 8)
            label << "  7.1";
        else if (channels >= 6)
            label << "  5.1";

        auto* b = stripButtons.add (new juce::TextButton (label));
        b->setClickingTogglesState (false);
        b->setColour (juce::TextButton::buttonColourId, Palette::panel);
        b->setColour (juce::TextButton::buttonOnColourId, Palette::blue.withAlpha (0.55f));
        b->setColour (juce::TextButton::textColourOffId, Palette::dim);
        b->setColour (juce::TextButton::textColourOnId, Palette::text);
        b->onClick = [this, i] { controller.setSelectedStrip (i); };
        addAndMakeVisible (b);
    }
    resized();
}

void MainComponent::refreshPresetList()
{
    presetBox.clear (juce::dontSendNotification);
    presetIdsByItem.clear();

    juce::String lastHeading;
    for (const auto& p : controller.getPresetManager().getPresets())
    {
        const auto heading = (p.isFactory ? "Factory - " : "User - ") + p.category;
        if (heading != lastHeading)
        {
            presetBox.addSectionHeading (heading);
            lastHeading = heading;
        }
        presetIdsByItem.push_back (p.id);
        presetBox.addItem (p.name, static_cast<int> (presetIdsByItem.size()));
    }
}

void MainComponent::refreshControls()
{
    const int strip = controller.getSelectedStrip();
    auto& store = controller.getParams (strip);

    for (int i = 0; i < stripButtons.size(); ++i)
        stripButtons[i]->setToggleState (i == strip, juce::dontSendNotification);

    const auto mode = controller.getMode (strip);
    musicButton.setToggleState (mode == ModeValue::Music, juce::dontSendNotification);
    gamingButton.setToggleState (mode == ModeValue::Gaming, juce::dontSendNotification);

    boostSlider.setValue (store.get (BoostIntensity), juce::dontSendNotification);
    for (size_t i = 0; i < macroSliders.size(); ++i)
    {
        macroSliders[i].setValue (store.get (Macro1 + static_cast<int> (i)), juce::dontSendNotification);
        macroLabels[i].setText (EngineController::getMacroName (mode, static_cast<int> (i)), juce::dontSendNotification);
    }

    const bool enabled = controller.isEnabled();
    enableButton.setButtonText (enabled ? "ENABLED" : "BYPASSED");
    enableButton.setToggleState (enabled, juce::dontSendNotification);

    abButton.setButtonText (store.getActiveBank() == Bank::A ? "A  |  b" : "a  |  B");

    const auto currentId = controller.getCurrentPresetId (strip);
    int item = 0;
    for (size_t i = 0; i < presetIdsByItem.size(); ++i)
        if (presetIdsByItem[i] == currentId)
            item = static_cast<int> (i) + 1;
    presetBox.setSelectedId (item, juce::dontSendNotification);

    lastStoreVersion = store.version();
    lastSelectedStrip = strip;
}

void MainComponent::updateStatusText()
{
    const auto status = controller.getStatus();
    const auto latency = controller.getLatencyInfo();
    auto& routing = controller.getRouting();

    juce::String s;
    s << (status.deviceOpen ? status.deviceName + " (" + status.deviceTypeName + ")" : juce::String ("No audio device (offline)"));
    s << "   |   " << juce::String (latency.sampleRate / 1000.0, 1) << " kHz / " << latency.blockSize << " smp";
    s << "   |   latency " << juce::String (latency.totalMs, 1) << " ms (engine " << juce::String (latency.engineMs, 1) << " ms)";
    if (status.deviceOpen)
        s << "   |   CPU " << juce::roundToInt (status.cpuLoad * 100.0) << "%";
    if (status.xruns >= 0)
        s << "   |   xruns " << status.xruns;

    const auto method = routing.getEffectiveMethod();
    s << "   |   app routing: "
      << (method == AppRouting::Method::EndpointRouting ? "endpoints"
                                                        : (method == AppRouting::Method::ProcessCapture ? "capture" : "off"));
    s << "   |   platform services: " << (platform_bridge::servicesCompiledIn() ? "yes" : "not built");
    if (controller.getLastDeviceError().isNotEmpty())
        s << "   |   " << controller.getLastDeviceError();
    statusText = s;
}

void MainComponent::openAudioSettings()
{
    auto* selector = new juce::AudioDeviceSelectorComponent (controller.getDeviceManager(), 0, 8, 1, 2, false, false, true, false);
    selector->setSize (560, 520);

    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned (selector);
    options.dialogTitle = "Audio Settings";
    options.dialogBackgroundColour = Palette::background;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = true;
    options.launchAsync();
}

void MainComponent::engineControllerChanged (EngineController::Change change)
{
    switch (change)
    {
        case EngineController::Change::Preset:
            refreshPresetList();
            refreshControls();
            break;
        case EngineController::Change::Engine:
            if (stripButtons.size() != controller.getNumStrips())
                rebuildStripButtons();
            spectrum->reset();
            refreshControls();
            break;
        case EngineController::Change::SelectedStrip:
            spectrum->reset();
            refreshControls();
            break;
        case EngineController::Change::MasterEnable:
        case EngineController::Change::Parameters:
            refreshControls();
            break;
        case EngineController::Change::Device:
        case EngineController::Change::Routing:
        case EngineController::Change::Settings:
            updateStatusText();
            repaint (footerArea);
            break;
    }
}

void MainComponent::timerCallback()
{
    const int strip = controller.getSelectedStrip();
    auto& store = controller.getParams (strip);
    if (store.version() != lastStoreVersion || strip != lastSelectedStrip)
        refreshControls();

    const auto generation = controller.getEngineGeneration();
    if (generation != lastGeneration)
    {
        spectrum->reset();
        lastGeneration = generation;
    }

    // Re-fetched every tick: the chain object changes on reconfiguration.
    auto& chain = controller.getChain (strip);
    meters->update (chain.meters(), controller.getMasterGainReductionDb(), controller.isStripActive (strip));
    spectrum->pull (chain.taps());
    spectrum->compute (controller.getHost().getSampleRate());

    if (++tick % 10 == 0)
    {
        updateStatusText();
        repaint (footerArea);
        repaint (stripArea);
    }
}

void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (Palette::background);

    // Header
    g.setColour (Palette::panel);
    g.fillRect (headerArea);
    g.setColour (Palette::panelEdge);
    g.drawHorizontalLine (headerArea.getBottom() - 1, 0.0f, static_cast<float> (getWidth()));

    auto title = headerArea.reduced (20, 0);
    g.setColour (Palette::accent);
    g.setFont (makeFont (24.0f, true));
    g.drawText ("FLUBSOUND PRO", title.removeFromLeft (220), juce::Justification::centredLeft);
    g.setColour (Palette::dim);
    g.setFont (makeFont (14.0f));
    g.drawText ("Music & Gaming Edition", title.removeFromLeft (200), juce::Justification::centredLeft);

    // Strip activity dots
    for (int i = 0; i < stripButtons.size(); ++i)
    {
        const auto b = stripButtons[i]->getBounds().toFloat();
        const bool active = controller.isStripActive (i);
        g.setColour (active ? Palette::accent : Palette::panelEdge);
        g.fillEllipse (b.getRight() - 14.0f, b.getCentreY() - 3.5f, 7.0f, 7.0f);
    }

    // Left control panel
    drawPanel (g, leftArea.toFloat());
    g.setColour (Palette::dim);
    g.setFont (makeFont (12.0f, true));
    g.drawText ("MODE", leftArea.reduced (16, 0).withHeight (22), juce::Justification::bottomLeft);

    // Footer
    g.setColour (Palette::panel);
    g.fillRect (footerArea);
    g.setColour (Palette::dim);
    g.setFont (makeFont (12.0f));
    g.drawText (statusText, footerArea.reduced (16, 0), juce::Justification::centredLeft, true);
}

void MainComponent::resized()
{
    auto r = getLocalBounds();

    headerArea = r.removeFromTop (64);
    {
        auto h = headerArea.reduced (16, 14);
        audioSettingsButton.setBounds (h.removeFromRight (150));
        h.removeFromRight (10);
        enableButton.setBounds (h.removeFromRight (130));
    }

    footerArea = r.removeFromBottom (28);

    stripArea = r.removeFromTop (52);
    {
        auto s = stripArea.reduced (16, 8);
        for (auto* b : stripButtons)
        {
            b->setBounds (s.removeFromLeft (150));
            s.removeFromLeft (8);
        }
    }

    r.reduce (16, 8);
    leftArea = r.removeFromLeft (360);
    {
        auto l = leftArea.reduced (16, 12);
        l.removeFromTop (14); // "MODE" caption
        auto modeRow = l.removeFromTop (34);
        musicButton.setBounds (modeRow.removeFromLeft (modeRow.getWidth() / 2 - 4));
        modeRow.removeFromLeft (8);
        gamingButton.setBounds (modeRow);

        l.removeFromTop (12);
        boostLabel.setBounds (l.removeFromTop (20));
        boostSlider.setBounds (l.removeFromTop (180).withSizeKeepingCentre (200, 180));

        l.removeFromTop (8);
        for (size_t i = 0; i < macroSliders.size(); ++i)
        {
            auto row = l.removeFromTop (30);
            macroLabels[i].setBounds (row.removeFromLeft (120));
            macroSliders[i].setBounds (row);
            l.removeFromTop (3);
        }

        l.removeFromTop (10);
        auto presetRow = l.removeFromTop (30);
        previousPresetButton.setBounds (presetRow.removeFromLeft (32));
        nextPresetButton.setBounds (presetRow.removeFromRight (32));
        presetBox.setBounds (presetRow.reduced (6, 0));

        l.removeFromTop (8);
        auto abRow = l.removeFromTop (30);
        abButton.setBounds (abRow.removeFromLeft (110));
        abRow.removeFromLeft (8);
        copyAbButton.setBounds (abRow.removeFromLeft (140));
    }

    r.removeFromLeft (12);
    meters->setBounds (r.removeFromTop (juce::jmin (300, r.getHeight() / 2)));
    r.removeFromTop (12);
    spectrum->setBounds (r);
}
} // namespace flub::app::ui
