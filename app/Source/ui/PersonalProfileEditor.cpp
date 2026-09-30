#include "PersonalProfileEditor.h"

#include "Theme.h"
#include "Widgets.h"

#include <algorithm>
#include <cmath>

namespace flub::app::ui
{
namespace
{
using flub::PersonalProfile;

constexpr int kRowHeight = 30;
constexpr int kEarCaptionWidth = 58;
constexpr int kBandRowHeight = 118;

void setUpSlider (juce::Slider& s, juce::Slider::SliderStyle style, float range, const juce::String& title)
{
    s.setSliderStyle (style);
    // An editable value box: the manual entry (type a number, Enter).
    s.setTextBoxStyle (style == juce::Slider::LinearVertical ? juce::Slider::TextBoxBelow : juce::Slider::TextBoxRight, false,
                       style == juce::Slider::LinearVertical ? 52 : 72, 20);
    s.setTextBoxIsEditable (true);
    s.setRange (-static_cast<double> (range), static_cast<double> (range), 0.5);
    s.setDoubleClickReturnValue (true, 0.0);
    s.setTextValueSuffix (" dB");
    s.setTitle (title);
}
} // namespace

PersonalProfileEditor::PersonalProfileEditor (EngineController& c)
    : controller (c)
{
    setTitle ("Personal profile");

    Style::set (enableToggle, "switch");
    enableToggle.setTitle ("Personal profile");
    enableToggle.setTooltip ("Applies the per-ear gains below to every strip. A listening preference, not a hearing test or a hearing aid.");
    enableToggle.onClick = [this] { edited(); };
    addAndMakeVisible (enableToggle);

    setUpSlider (balance, juce::Slider::LinearHorizontal, PersonalProfile::kBalanceRangeDb, "Balance");
    balance.setTooltip ("Right of the centre turns the left ear down, left of it the right ear; a balance never boosts.");
    balance.onValueChange = [this] { edited(); };
    addAndMakeVisible (balance);

    for (int ear = 0; ear < 2; ++ear)
    {
        const juce::String side = ear == 0 ? "Left" : "Right";
        auto& g = gain[static_cast<size_t> (ear)];
        setUpSlider (g, juce::Slider::LinearVertical, PersonalProfile::kGainRangeDb, side + " gain");
        g.setTooltip (side + " ear, every frequency (+-12 dB).");
        g.onValueChange = [this] { edited(); };
        addAndMakeVisible (g);
        for (int b = 0; b < PersonalProfile::kNumBands; ++b)
        {
            auto& s = bands[static_cast<size_t> (ear)][static_cast<size_t> (b)];
            setUpSlider (s, juce::Slider::LinearVertical, PersonalProfile::kBandRangeDb, side + " " + bandName (b));
            s.setTooltip (side + " ear around " + bandName (b) + " (+-15 dB; the ear's total at most +15 dB).");
            s.onValueChange = [this] { edited(); };
            addAndMakeVisible (s);
        }
    }

    flatButton.setTooltip ("Every gain, band and the balance back to 0 dB (the switch stays as it is)");
    flatButton.onClick = [this]
    {
        auto p = controller.getPersonalProfile();
        const bool on = p.enabled;
        p = {};
        p.enabled = on;
        controller.setPersonalProfile (p);
        refresh();
    };
    addAndMakeVisible (flatButton);

    refresh();
}

juce::String PersonalProfileEditor::bandName (int band)
{
    const double hz = PersonalProfile::kBandHz[static_cast<size_t> (juce::jlimit (0, PersonalProfile::kNumBands - 1, band))];
    return hz >= 1000.0 ? juce::String (juce::roundToInt (hz / 1000.0)) + " kHz" : juce::String (juce::roundToInt (hz)) + " Hz";
}

void PersonalProfileEditor::showProfile (const PersonalProfile& p)
{
    const juce::ScopedValueSetter<bool> guard (updating, true);
    enableToggle.setToggleState (p.enabled, juce::dontSendNotification);
    balance.setValue (p.balanceDb, juce::dontSendNotification);
    for (size_t ear = 0; ear < 2; ++ear)
    {
        gain[ear].setValue (p.gainDb[ear], juce::dontSendNotification);
        for (size_t b = 0; b < static_cast<size_t> (PersonalProfile::kNumBands); ++b)
            bands[ear][b].setValue (p.bandDb[ear][b], juce::dontSendNotification);
    }
    // The sliders are the preference; they stay live while it is off, so a
    // shape can be prepared and then switched on.
    shown = p;
}

void PersonalProfileEditor::refresh()
{
    const auto& p = controller.getPersonalProfile();
    if (! (p == shown))
        showProfile (p);
    const float reservation = controller.getChain (controller.getSelectedStrip()).getPersonalReservationDb();
    if (const auto text = describeProfile (shown, reservation); text != description || reservation != shownReservationDb)
    {
        description = text;
        shownReservationDb = reservation;
        repaint (descriptionArea);
    }
}

void PersonalProfileEditor::edited()
{
    if (updating)
        return;
    PersonalProfile p;
    p.enabled = enableToggle.getToggleState();
    p.balanceDb = static_cast<float> (balance.getValue());
    for (size_t ear = 0; ear < 2; ++ear)
    {
        p.gainDb[ear] = static_cast<float> (gain[ear].getValue());
        for (size_t b = 0; b < static_cast<size_t> (PersonalProfile::kNumBands); ++b)
            p.bandDb[ear][b] = static_cast<float> (bands[ear][b].getValue());
    }
    controller.setPersonalProfile (p); // every strip's chain, crossfaded; the file is written by the controller
    shown = controller.getPersonalProfile();
    refresh();
}

juce::String PersonalProfileEditor::describeProfile (const PersonalProfile& profile, float reservationDb)
{
    const auto p = profile.sanitised();
    if (! p.enabled)
        return "Off: both ears play as the presets intend. Switch it on to apply the gains above to every strip.";
    if (p.isNeutral())
        return "Flat: every gain at 0 dB, so nothing changes.";

    // What the core's caps did to the entry: each ear's target at most
    // kMaxBoostDb, the ears at most kMaxEarDifferenceDb apart.
    bool capped = false;
    juce::String ears;
    for (int ear = 0; ear < 2; ++ear)
    {
        const auto e = static_cast<size_t> (ear);
        const float balanceCut = ear == 0 ? std::min (0.0f, -p.balanceDb) : std::min (0.0f, p.balanceDb);
        float lo = 1000.0f, hi = -1000.0f;
        for (int b = 0; b < PersonalProfile::kNumBands; ++b)
        {
            const float target = p.targetDb (ear, b);
            const float asked = p.gainDb[e] + balanceCut + p.bandDb[e][static_cast<size_t> (b)];
            capped = capped || std::abs (asked - target) > 0.01f;
            lo = std::min (lo, target);
            hi = std::max (hi, target);
        }
        ears << (ear == 0 ? "Left ear " : "; right ear ") << Theme::formatSignedDb (p.broadbandDb (ear), 1) << " dB";
        if (hi - lo > 0.05f)
            ears << " (" << Theme::formatSignedDb (lo, 1) << " .. " << Theme::formatSignedDb (hi, 1) << " dB from 250 Hz to 8 kHz)";
    }
    juce::String t = ears + ".";
    if (capped)
        t << " Capped: each ear at most +" << juce::String (juce::roundToInt (PersonalProfile::kMaxBoostDb)) << " dB, the ears at most "
          << juce::String (juce::roundToInt (PersonalProfile::kMaxEarDifferenceDb)) << " dB apart.";
    if (reservationDb < -0.05f)
        t << " Both ears are turned down " << juce::String (-reservationDb, 1)
          << " dB to leave the boosted ear its headroom; the maximizer's drive or the volume gives it back.";
    return t;
}

void PersonalProfileEditor::paint (juce::Graphics& g)
{
    g.setColour (Palette::faint.brighter (0.2f));
    g.setFont (Theme::font (11.5f));
    g.drawFittedText ("A listening preference entered by hand, not a hearing test, a fitting or a hearing aid. It applies to every strip "
                      "and is kept outside the presets. If one ear hears much less, see a hearing professional.",
                      introArea, juce::Justification::topLeft, 3, 1.0f);

    g.setColour (Palette::text.withAlpha (0.88f));
    g.setFont (Theme::font (13.0f));
    g.drawText ("Balance", balanceCaption, juce::Justification::centredLeft, true);

    // Column captions over the grid: Gain, then the eight bands.
    g.setColour (Palette::muted);
    g.setFont (Theme::font (11.0f));
    const auto& left = gain[0];
    g.drawText ("Gain", left.getX(), gridHeader.getY(), left.getWidth(), gridHeader.getHeight(), juce::Justification::centred, false);
    for (int b = 0; b < PersonalProfile::kNumBands; ++b)
    {
        const auto& s = bands[0][static_cast<size_t> (b)];
        g.drawText (bandName (b), s.getX(), gridHeader.getY(), s.getWidth(), gridHeader.getHeight(), juce::Justification::centred, false);
    }
    g.setColour (Palette::text.withAlpha (0.88f));
    g.setFont (Theme::font (12.5f));
    g.drawText ("Left", rowCaptions[0], juce::Justification::centredLeft, false);
    g.drawText ("Right", rowCaptions[1], juce::Justification::centredLeft, false);

    g.setColour (enableToggle.getToggleState() ? Palette::muted : Palette::faint);
    g.setFont (Theme::font (11.5f));
    g.drawFittedText (description, descriptionArea, juce::Justification::topLeft, 3, 1.0f);
}

void PersonalProfileEditor::resized()
{
    auto r = getLocalBounds();
    introArea = r.removeFromTop (34);
    r.removeFromTop (4);
    {
        auto row = r.removeFromTop (kRowHeight);
        flatButton.setBounds (row.removeFromRight (70).reduced (0, 3));
        row.removeFromRight (8);
        enableToggle.setBounds (row.withWidth (juce::jmin (row.getWidth(), 360)).reduced (0, 2));
    }
    r.removeFromTop (4);
    {
        auto row = r.removeFromTop (kRowHeight);
        balanceCaption = row.removeFromLeft (kEarCaptionWidth + 30);
        balance.setBounds (row.withWidth (juce::jmin (row.getWidth(), 360)).reduced (0, 3));
    }
    r.removeFromTop (6);
    gridHeader = r.removeFromTop (16).withTrimmedLeft (kEarCaptionWidth);
    constexpr int columns = 1 + PersonalProfile::kNumBands;
    for (int ear = 0; ear < 2; ++ear)
    {
        auto row = r.removeFromTop (kBandRowHeight);
        rowCaptions[ear] = row.removeFromLeft (kEarCaptionWidth);
        const int columnWidth = row.getWidth() / columns;
        auto cell = [&row, columnWidth] { return row.removeFromLeft (columnWidth).reduced (2, 2); };
        gain[static_cast<size_t> (ear)].setBounds (cell());
        for (auto& s : bands[static_cast<size_t> (ear)])
            s.setBounds (cell());
        r.removeFromTop (4);
    }
    descriptionArea = r.removeFromTop (48);
}
} // namespace flub::app::ui
