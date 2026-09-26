#include "AnalyzerPanel.h"

#include "Theme.h"

namespace flub::app::ui
{
AnalyzerPanel::AnalyzerPanel (EqCurveEditor::StoreProvider storeProvider)
    : eqEditor (analyzer, std::move (storeProvider))
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
            applyOptions();
            if (onOptionsChanged != nullptr)
                onOptionsChanged (options);
        };
        addAndMakeVisible (b);
    };
    setUpToggle (preButton, "Show input spectrum", "Show the spectrum before processing (grey)", &Options::showPre);
    setUpToggle (postButton, "Show output spectrum", "Show the processed spectrum", &Options::showPost);
    setUpToggle (tiltButton, "Spectrum tilt", "+4.5 dB/octave tilt: typical music reads flat", &Options::tilt);
    setUpToggle (holdButton, "Peak hold", "Show a peak-hold trace of the output", &Options::peakHold);

    rangeBox.addItem (juce::String (juce::CharPointer_UTF8 ("\xc2\xb1")) + "6 dB", 6);
    rangeBox.addItem (juce::String (juce::CharPointer_UTF8 ("\xc2\xb1")) + "12 dB", 12);
    rangeBox.addItem (juce::String (juce::CharPointer_UTF8 ("\xc2\xb1")) + "24 dB", 24);
    Style::describe (rangeBox, "EQ display range", "Vertical range of the EQ curve");
    rangeBox.onChange = [this]
    {
        options.eqRangeDb = static_cast<float> (rangeBox.getSelectedId());
        applyOptions();
        if (onOptionsChanged != nullptr)
            onOptionsChanged (options);
    };
    addAndMakeVisible (rangeBox);

    applyOptions();
}

void AnalyzerPanel::setOptions (const Options& newOptions)
{
    options = newOptions;
    applyOptions();
}

void AnalyzerPanel::applyOptions()
{
    preButton.setToggleState (options.showPre, juce::dontSendNotification);
    postButton.setToggleState (options.showPost, juce::dontSendNotification);
    tiltButton.setToggleState (options.tilt, juce::dontSendNotification);
    holdButton.setToggleState (options.peakHold, juce::dontSendNotification);
    const int range = options.eqRangeDb <= 6.0f ? 6 : (options.eqRangeDb <= 12.0f ? 12 : 24);
    rangeBox.setSelectedId (range, juce::dontSendNotification);

    analyzer.setShowPre (options.showPre);
    analyzer.setShowPost (options.showPost);
    analyzer.setTiltEnabled (options.tilt);
    analyzer.setPeakHoldEnabled (options.peakHold);
    eqEditor.setRangeDb (static_cast<float> (range));
}

void AnalyzerPanel::paint (juce::Graphics& g)
{
    Theme::drawPanel (g, getLocalBounds().toFloat());

    auto h = headerArea.toFloat();
    const float w = Theme::drawCaption (g, "SPECTRUM  +  EQ", h, Palette::muted);
    h.removeFromLeft (w + 18.0f);

    // Legend (only when all of it fits).
    const auto accent = Theme::accent (*this);
    struct Entry
    {
        juce::Colour colour;
        const char* text;
        bool diamond;
    };
    const Entry entries[] = { { Palette::muted, "Input", false }, { accent, "Output", false }, { Palette::text, "EQ", false },
                              { Palette::dynamicEq, "Dynamic EQ", true } };
    const auto font = Theme::font (11.5f);
    float needed = 0.0f;
    for (const auto& e : entries)
        needed += 15.0f + juce::GlyphArrangement::getStringWidth (font, e.text) + 14.0f;
    h.setRight (static_cast<float> (preButton.getX()) - 10.0f);
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
    {
        auto h = headerArea;
        rangeBox.setBounds (h.removeFromRight (82).reduced (0, 1));
        h.removeFromRight (8);
        for (auto* b : { &holdButton, &tiltButton, &postButton, &preButton })
        {
            b->setBounds (h.removeFromRight (46).reduced (0, 2));
            h.removeFromRight (5);
        }
    }
    r.removeFromTop (4);
    analyzer.setBounds (r);
    eqEditor.setBounds (r);
}
} // namespace flub::app::ui
