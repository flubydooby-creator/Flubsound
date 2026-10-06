#include "AnalyzerPanel.h"

#include "Theme.h"
#include "vis/VisualiserRegistry.h"

#include <cmath>

namespace flub::app::ui
{
namespace
{
constexpr const char* kCaption = "SPECTRUM  +  EQ";
constexpr int kChipWidth = 46, kChipGap = 5;
constexpr int kBesideMinWidth = 640; // narrower panels replace the spectrum instead
constexpr int kMenuVisualiser = 100; // + registry index; kMenuVisualiser - 1 = the spectrum
constexpr int kMenuStrip = 200;      // + registry index; kMenuStrip - 1 = no strip
constexpr int kMenuBeside = 300;
constexpr int kMenuPopOut = 400;
constexpr int kMenuFundamentals = 50;

struct LegendEntry
{
    juce::Colour colour;
    const char* text;
    bool diamond;
};
} // namespace

// =============================================================================
// Options <-> preference string
// =============================================================================
juce::String AnalyzerPanel::Options::toString() const
{
    const auto b = [] (bool v) { return juce::String (v ? 1 : 0); };
    return b (showPre) + "," + b (showPost) + "," + b (tilt) + "," + b (peakHold) + "," + juce::String (juce::roundToInt (eqRangeDb)) + ","
           + b (difference) + "," + b (sharpLows) + "," + b (width) + "," + juce::String ((pianoKeys ? 1 : 0) + (fundamentals ? 2 : 0)) + "," + b (spectrogram) + "," + visualiser + ","
           + strip + "," + b (beside);
}

bool AnalyzerPanel::Options::fromString (const juce::String& text, Options& o)
{
    const auto tokens = juce::StringArray::fromTokens (text, ",", {});
    if (tokens.size() != 5 && tokens.size() != 10 && tokens.size() != 13)
        return false;
    o = Options {};
    o.showPre = tokens[0] != "0";
    o.showPost = tokens[1] != "0";
    o.tilt = tokens[2] != "0";
    o.peakHold = tokens[3] != "0";
    o.eqRangeDb = static_cast<float> (juce::jlimit (6, 24, tokens[4].getIntValue()));
    if (tokens.size() >= 10)
    {
        o.difference = tokens[5] == "1";
        o.sharpLows = tokens[6] == "1";
        o.width = tokens[7] == "1";
        const int keys = tokens[8].getIntValue();
        o.pianoKeys = (keys & 1) != 0;
        o.fundamentals = (keys & 2) != 0;
        o.spectrogram = tokens[9] == "1";
    }
    if (tokens.size() == 13)
    {
        // Ids of views this build does not have (a newer version wrote them) read as the default.
        if (const auto* d = vis::findDescriptor (tokens[10].trim()); d != nullptr && d->canBeMain)
            o.visualiser = d->id;
        if (const auto* d = vis::findDescriptor (tokens[11].trim()); d != nullptr && d->canBeStrip)
            o.strip = d->id;
        o.beside = tokens[12] == "1";
    }
    return true;
}

// =============================================================================
AnalyzerPanel::AnalyzerPanel (EqCurveEditor::StoreProvider storeProvider)
    : eqEditor (analyzer, storeProvider), stores (std::move (storeProvider))
{
    setTitle ("Spectrum and EQ");
    setDescription ("Spectrum analyser with the editable parametric EQ curve of the selected strip");
    addAndMakeVisible (analyzer);
    addAndMakeVisible (eqEditor);

    auto setUpToggle = [this] (juce::TextButton& b, const juce::String& title, const juce::String& tip, bool Options::*field)
    {
        Style::set (b, "chip");
        b.setClickingTogglesState (true);
        Style::describe (b, title, tip);
        b.onClick = [this, &b, field]
        {
            options.*field = b.getToggleState();
            changed();
        };
        addAndMakeVisible (b);
    };
    setUpToggle (preButton, "Show input spectrum", "Show the spectrum before processing (grey)", &Options::showPre);
    setUpToggle (postButton, "Show output spectrum", "Show the processed spectrum", &Options::showPost);
    setUpToggle (tiltButton, "Spectrum tilt", "+4.5 dB/octave tilt: typical music reads flat", &Options::tilt);
    setUpToggle (holdButton, "Peak hold", "Show a peak-hold trace of the output", &Options::peakHold);
    setUpToggle (diffButton, "Show what Flubsound changes",
                 "Output minus input per frequency (green, on the EQ scale at the right: 0 dB = unchanged)", &Options::difference);

    Style::set (viewButton, "chip");
    Style::describe (viewButton, "Analyser views",
                     "More views: spectrogram, what Flubsound changes, sharper lows, stereo width, piano keys, freeze, "
                     "and visualisers (goniometer, stereo field, loudness, waveform, gain reduction, correlation)");
    viewButton.onClick = [this] { showViewMenu(); };
    addAndMakeVisible (viewButton);

    Style::set (freezeButton, "chip");
    Style::describe (freezeButton, "Freeze the spectrum",
                     "Keep the current output (and input) trace as a dashed reference. Click again to re-capture, "
                     "Shift+click (or View > Clear frozen trace) to remove it.");
    freezeButton.onClick = [this]
    {
        const auto mods = juce::ModifierKeys::getCurrentModifiers();
        if (mods.isShiftDown() || mods.isPopupMenu())
            clearFreeze();
        else
            freeze();
    };
    addChildComponent (freezeButton);
    addChildComponent (diffButton);

    rangeBox.addItem (juce::String (juce::CharPointer_UTF8 ("\xc2\xb1")) + "6 dB", 6);
    rangeBox.addItem (juce::String (juce::CharPointer_UTF8 ("\xc2\xb1")) + "12 dB", 12);
    rangeBox.addItem (juce::String (juce::CharPointer_UTF8 ("\xc2\xb1")) + "24 dB", 24);
    Style::describe (rangeBox, "EQ display range", "Vertical range of the EQ curve");
    rangeBox.onChange = [this]
    {
        options.eqRangeDb = static_cast<float> (rangeBox.getSelectedId());
        changed();
    };
    addAndMakeVisible (rangeBox);

    applyOptions();
}

void AnalyzerPanel::setOptions (const Options& newOptions)
{
    options = newOptions;
    applyOptions();
}

void AnalyzerPanel::changed()
{
    applyOptions();
    if (onOptionsChanged != nullptr)
        onOptionsChanged (options);
}

void AnalyzerPanel::applyOptions()
{
    preButton.setToggleState (options.showPre, juce::dontSendNotification);
    postButton.setToggleState (options.showPost, juce::dontSendNotification);
    tiltButton.setToggleState (options.tilt, juce::dontSendNotification);
    holdButton.setToggleState (options.peakHold, juce::dontSendNotification);
    diffButton.setToggleState (options.difference, juce::dontSendNotification);
    viewButton.setToggleState (options.spectrogram || options.sharpLows || options.width || options.pianoKeys, juce::dontSendNotification);
    const int range = options.eqRangeDb <= 6.0f ? 6 : (options.eqRangeDb <= 12.0f ? 12 : 24);
    rangeBox.setSelectedId (range, juce::dontSendNotification);

    analyzer.setShowPre (options.showPre);
    analyzer.setShowPost (options.showPost);
    analyzer.setTiltEnabled (options.tilt);
    analyzer.setPeakHoldEnabled (options.peakHold);
    analyzer.setEqRangeDb (static_cast<float> (range));
    analyzer.setDifferenceEnabled (options.difference);
    analyzer.setSharpLowsEnabled (options.sharpLows);
    analyzer.setWidthEnabled (options.width);
    analyzer.setPianoKeysEnabled (options.pianoKeys);
    analyzer.setFundamentalsOnly (options.fundamentals);
    analyzer.setSpectrogramEnabled (options.spectrogram);
    eqEditor.setRangeDb (static_cast<float> (range));

    // Visualisers: an id that is unknown or registered for the other place shows nothing.
    const auto* mainDesc = vis::findDescriptor (options.visualiser);
    const auto* stripDesc = vis::findDescriptor (options.strip);
    auto* newMain = mainDesc != nullptr && mainDesc->canBeMain ? visualisers.get (mainDesc->id) : nullptr;
    auto* newStrip = stripDesc != nullptr && stripDesc->canBeStrip ? visualisers.get (stripDesc->id) : nullptr;
    for (auto* v : { newMain, newStrip })
        if (v != nullptr && v->getParentComponent() != this)
            addChildComponent (*v);
    for (auto* old : { mainView, stripView })
        if (old != nullptr && old != newMain && old != newStrip)
            old->setVisible (false);
    mainView = newMain;
    stripView = newStrip;
    visualisers.setSelected (mainView != nullptr ? juce::String (mainDesc->id) : juce::String(),
                             stripView != nullptr ? juce::String (stripDesc->id) : juce::String());
    if (mainView != nullptr || stripView != nullptr)
        viewButton.setToggleState (true, juce::dontSendNotification);
    resized();
    repaint();
}

bool AnalyzerPanel::isSpectrumReplaced() const noexcept
{
    return mainView != nullptr && ! analyzer.isVisible();
}

float AnalyzerPanel::loudnessTarget (const flub::param::ParameterStore& store, juce::String& name)
{
    using namespace flub::param;
    if (store.get (MaximizerOn) > 0.5f && store.get (MaxAutoDrive) > 0.5f)
    {
        name = "Maximizer target";
        return store.get (MaxTargetLufs);
    }
    if (store.get (AutoLevelOn) > 0.5f)
    {
        name = "Auto level target (input)";
        return store.get (AutoLevelTargetLufs);
    }
    name = {};
    return std::nanf ("");
}

void AnalyzerPanel::advanceVisualisers (const MeterSnapshot& meters, double dtSeconds, double sampleRate)
{
    visualisers.setSampleRate (sampleRate);
    vis::FrameContext frame { meters, dtSeconds, sampleRate, &analyzer };
    if (const auto* store = stores != nullptr ? stores() : nullptr)
        frame.targetLufs = loudnessTarget (*store, frame.targetName);
    visualisers.advance (frame);
}

juce::String AnalyzerPanel::caption() const
{
    if (isSpectrumReplaced())
        if (const auto* d = vis::findDescriptor (options.visualiser))
            return d->caption;
    return kCaption;
}

void AnalyzerPanel::freeze()
{
    analyzer.freeze();
    freezeButton.setToggleState (true, juce::dontSendNotification);
}

void AnalyzerPanel::clearFreeze()
{
    analyzer.clearFreeze();
    freezeButton.setToggleState (false, juce::dontSendNotification);
}

void AnalyzerPanel::showViewMenu()
{
    juce::PopupMenu menu;
    menu.addSectionHeader ("Analyser views");
    menu.addItem (1, "Spectrogram (scrolling history)", true, options.spectrogram);
    menu.addItem (2, "What Flubsound changes (Out - In)", true, options.difference);
    menu.addItem (3, "Sharper lows (long FFT below 300 Hz)", true, options.sharpLows);
    menu.addItem (4, "Stereo width", true, options.width);
    menu.addItem (5, "Piano keys", true, options.pianoKeys);
    menu.addItem (kMenuFundamentals, "Piano keys: fundamentals only", options.pianoKeys, options.fundamentals);
    menu.addSeparator();
    menu.addItem (6, analyzer.isFrozen() ? "Re-capture frozen trace" : "Freeze the current trace");
    menu.addItem (7, "Clear frozen trace", analyzer.isFrozen());

    // Visualisers: one main view (radio), beside or instead of the spectrum, and a strip.
    menu.addSectionHeader ("Visualiser");
    menu.addItem (kMenuVisualiser - 1, "Spectrum + EQ (default)", true, mainView == nullptr);
    const auto& list = vis::registry();
    for (size_t i = 0; i < list.size(); ++i)
        if (list[i].canBeMain)
            menu.addItem (kMenuVisualiser + static_cast<int> (i), list[i].menuName, true, options.visualiser == list[i].id);
    menu.addItem (kMenuBeside, "Beside the spectrum (wide windows)", mainView != nullptr, options.beside);
    juce::PopupMenu strips;
    strips.addItem (kMenuStrip - 1, "None", true, stripView == nullptr);
    for (size_t i = 0; i < list.size(); ++i)
        if (list[i].canBeStrip)
            strips.addItem (kMenuStrip + static_cast<int> (i), list[i].menuName, true, options.strip == list[i].id);
    menu.addSubMenu ("Strip under the plot", strips);
    if (onPopOutRequested != nullptr)
    {
        const auto blocked = popOutBlockedReason != nullptr ? popOutBlockedReason() : juce::String();
        menu.addSeparator();
        menu.addItem (kMenuPopOut, blocked.isEmpty() ? juce::String ("Open in a window (full screen, second monitor)...")
                                                     : "Open in a window (" + blocked + ")",
                      blocked.isEmpty());
    }

    juce::Component::SafePointer<AnalyzerPanel> safe (this);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&viewButton),
                        [safe] (int result)
                        {
                            if (safe == nullptr || result == 0)
                                return;
                            auto& self = *safe;
                            auto& o = self.options;
                            if (self.applyVisualiserMenuItem (result))
                                return;
                            if (result == kMenuPopOut)
                            {
                                if (self.onPopOutRequested != nullptr)
                                    self.onPopOutRequested();
                                return;
                            }
                            switch (result)
                            {
                                case 1: o.spectrogram = ! o.spectrogram; break;
                                case 2: o.difference = ! o.difference; break;
                                case 3: o.sharpLows = ! o.sharpLows; break;
                                case 4: o.width = ! o.width; break;
                                case 5: o.pianoKeys = ! o.pianoKeys; break;
                                case kMenuFundamentals: o.fundamentals = ! o.fundamentals; break;
                                case 6: self.freeze(); return;
                                case 7: self.clearFreeze(); return;
                                default: return;
                            }
                            self.changed();
                        });
}

float AnalyzerPanel::legendWidth() const
{
    // Caption + gap + the legend as paint() draws it (the legend is drawn only when it all fits).
    const auto font = Theme::font (11.5f);
    float needed = juce::GlyphArrangement::getStringWidth (Theme::caption (11.0f), kCaption) + 18.0f;
    for (const char* text : { "Input", "Output", "EQ", "Dynamic EQ" })
        needed += 15.0f + juce::GlyphArrangement::getStringWidth (font, text) + 14.0f;
    return needed + 10.0f;
}

void AnalyzerPanel::paint (juce::Graphics& g)
{
    Theme::drawPanel (g, getLocalBounds().toFloat());

    auto h = headerArea.toFloat();
    const float w = Theme::drawCaption (g, caption(), h, Palette::muted);
    h.removeFromLeft (w + 18.0f);
    if (isSpectrumReplaced())
        return; // the visualiser draws its own legend

    // Legend (only when all of it fits).
    const auto accent = Theme::accent (*this);
    const LegendEntry entries[] = { { Palette::muted, "Input", false }, { accent, "Output", false }, { Palette::text, "EQ", false },
                                    { Palette::dynamicEq, "Dynamic EQ", true } };
    const auto font = Theme::font (11.5f);
    float needed = 0.0f;
    for (const auto& e : entries)
        needed += 15.0f + juce::GlyphArrangement::getStringWidth (font, e.text) + 14.0f;
    int leftmost = preButton.getX();
    for (auto* b : { &viewButton, &diffButton, &freezeButton })
        if (b->isVisible())
            leftmost = juce::jmin (leftmost, b->getX());
    h.setRight (static_cast<float> (leftmost) - 10.0f);
    if (needed > h.getWidth())
        return;

    g.setFont (font);
    for (const auto& e : entries)
    {
        auto dot = h.removeFromLeft (10.0f).withSizeKeepingCentre (8.0f, 8.0f);
        g.setColour (e.colour);
        if (e.diamond)
        {
            juce::Path p;
            p.addPolygon (dot.getCentre(), 4, 4.5f, juce::MathConstants<float>::pi * 0.25f);
            g.fillPath (p);
        }
        else
        {
            g.fillRoundedRectangle (dot.withHeight (3.0f).withCentre (dot.getCentre()), 1.5f);
        }
        h.removeFromLeft (5.0f);
        g.setColour (Palette::muted);
        const float tw = juce::GlyphArrangement::getStringWidth (font, e.text);
        g.drawText (e.text, h.removeFromLeft (tw + 2.0f), juce::Justification::centredLeft, false);
        h.removeFromLeft (12.0f);
    }
}

void AnalyzerPanel::resized()
{
    auto r = getLocalBounds().reduced (12, 10);
    headerArea = r.removeFromTop (24);
    const bool replaced = mainView != nullptr && ! (options.beside && r.getWidth() >= kBesideMinWidth);
    if (replaced)
    {
        // Only the View chip: the spectrum's controls have nothing to act on.
        viewButton.setBounds (headerArea.withLeft (headerArea.getRight() - (kChipWidth + 4)).reduced (0, 2));
    }
    else
    {
        auto h = headerArea;
        rangeBox.setBounds (h.removeFromRight (82).reduced (0, 1));
        h.removeFromRight (8);
        for (auto* b : { &holdButton, &tiltButton, &postButton, &preButton })
        {
            b->setBounds (h.removeFromRight (kChipWidth).reduced (0, 2));
            h.removeFromRight (kChipGap);
        }
        viewButton.setBounds (h.removeFromRight (kChipWidth + 4).reduced (0, 2));
        h.removeFromRight (kChipGap);

        // Diff, then Freeze, only where the caption and legend still fit.
        const float legend = legendWidth();
        for (auto* b : { &diffButton, &freezeButton })
        {
            const int w = b == &freezeButton ? kChipWidth + 8 : kChipWidth;
            const bool fits = static_cast<float> (h.getWidth() - w - kChipGap) >= legend;
            b->setVisible (fits);
            if (fits)
            {
                b->setBounds (h.removeFromRight (w).reduced (0, 2));
                h.removeFromRight (kChipGap);
            }
        }
    }
    r.removeFromTop (4);

    if (stripView != nullptr)
    {
        stripView->setBounds (r.removeFromBottom (stripView->getStripHeight()));
        r.removeFromBottom (6);
        stripView->setVisible (true);
    }

    // A main visualiser: beside the spectrum where there is room for both, else in its place.
    bool spectrumShown = true;
    if (mainView != nullptr)
    {
        if (options.beside && r.getWidth() >= kBesideMinWidth)
        {
            const int w = juce::jlimit (220, r.getWidth() / 2, juce::roundToInt (static_cast<float> (r.getHeight()) * 1.15f));
            mainView->setBounds (r.removeFromRight (w));
            r.removeFromRight (8);
        }
        else
        {
            mainView->setBounds (r);
            spectrumShown = false;
        }
        mainView->setVisible (true);
    }
    analyzer.setVisible (spectrumShown);
    eqEditor.setVisible (spectrumShown);
    for (auto* c : std::initializer_list<juce::Component*> { &preButton, &postButton, &tiltButton, &holdButton, &rangeBox })
        c->setVisible (spectrumShown);
    if (! spectrumShown)
    {
        diffButton.setVisible (false);
        freezeButton.setVisible (false);
    }
    analyzer.setBounds (r);
    eqEditor.setBounds (r);
}

bool AnalyzerPanel::applyVisualiserMenuItem (int item)
{
    const auto& views = vis::registry();
    const auto count = static_cast<int> (views.size());
    if (item == kMenuVisualiser - 1 || (item >= kMenuVisualiser && item < kMenuVisualiser + count))
        options.visualiser = item < kMenuVisualiser ? juce::String (Options::kSpectrum) : juce::String (views[static_cast<size_t> (item - kMenuVisualiser)].id);
    else if (item == kMenuStrip - 1 || (item >= kMenuStrip && item < kMenuStrip + count))
        options.strip = item < kMenuStrip ? juce::String (Options::kNone) : juce::String (views[static_cast<size_t> (item - kMenuStrip)].id);
    else if (item == kMenuBeside)
        options.beside = ! options.beside;
    else
        return false;
    changed();
    return true;
}
} // namespace flub::app::ui
