#include "DeviceAdviceBanner.h"

#include "FlubLookAndFeel.h"
#include "Theme.h"

namespace flub::app::ui
{
namespace
{
const juce::String kDot { juce::CharPointer_UTF8 (" \xc2\xb7 ") };

juce::String connectionText (flub::device::Connection c)
{
    using flub::device::Connection;
    switch (c)
    {
        case Connection::Analog: return "wired";
        case Connection::Usb: return "USB / wireless dongle";
        case Connection::Bluetooth: return "Bluetooth";
        case Connection::BluetoothHandsFree: return "Bluetooth hands-free";
        case Connection::Unknown: break;
    }
    return {};
}

/** Simple headphone glyph (headband arc + two ear cups). */
void drawHeadphones (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour colour)
{
    const auto s = juce::jmin (r.getWidth(), r.getHeight());
    const auto b = r.withSizeKeepingCentre (s, s);
    juce::Path band;
    band.addCentredArc (b.getCentreX(), b.getCentreY() + s * 0.08f, s * 0.38f, s * 0.36f, 0.0f,
                        -juce::MathConstants<float>::halfPi, juce::MathConstants<float>::halfPi, true);
    g.setColour (colour);
    g.strokePath (band, juce::PathStrokeType (s * 0.09f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    const float cupW = s * 0.2f, cupH = s * 0.34f, cupY = b.getCentreY() + s * 0.02f;
    g.fillRoundedRectangle (b.getX() + s * 0.08f, cupY, cupW, cupH, cupW * 0.4f);
    g.fillRoundedRectangle (b.getRight() - s * 0.08f - cupW, cupY, cupW, cupH, cupW * 0.4f);
}
} // namespace

DeviceAdviceBanner::DeviceAdviceBanner (EngineController& c) : controller (c)
{
    setTitle ("Output device advice");
    setDescription ("Headset profile, safety ceiling and setup advice for the current output device");

    presetButton.onClick = [this] { applySuggestedPreset(); };
    detailsButton.setTooltip ("All guidance for this device (Settings > Audio)");
    detailsButton.onClick = [this] {
        if (onDetailsRequested)
            onDetailsRequested();
    };
    dismissButton.setButtonText (juce::String (juce::CharPointer_UTF8 ("\xc3\x97"))); // multiplication sign
    dismissButton.setTooltip ("Hide the advice for this output device for the rest of the session");
    dismissButton.setTitle ("Dismiss device advice");
    dismissButton.onClick = [this] {
        dismissedFor = deviceName;
        if (refresh() && getParentComponent() != nullptr)
            getParentComponent()->resized();
    };
    for (auto* b : { &presetButton, &detailsButton, &dismissButton })
        addAndMakeVisible (b);
    setVisible (false);
}

bool DeviceAdviceBanner::refresh()
{
    using flub::device::Connection;
    const auto& adv = controller.getDeviceAdvice();
    const auto profile = controller.getDeviceProfileName();
    const auto connection = controller.getDeviceConnection();
    const bool bluetooth = connection == Connection::Bluetooth || connection == Connection::BluetoothHandsFree;

    deviceName = controller.getOutputDeviceName();
    const bool want = deviceName.isNotEmpty() && (profile.isNotEmpty() || bluetooth) && ! adv.messages.empty()
                      && deviceName != dismissedFor;

    headline = profile.isNotEmpty() ? profile : deviceName;
    if (const auto ct = connectionText (connection); ct.isNotEmpty())
        headline << kDot << ct;
    headline << kDot << "ceiling " << juce::String (adv.ceilingDbTp, 1) << " dBTP";
    advice = adv.messages.empty() ? juce::String() : juce::String::fromUTF8 (adv.messages.front().c_str());

    juce::String tip;
    for (const auto& m : adv.messages)
        tip << (tip.isEmpty() ? "" : "\n") << juce::String::fromUTF8 (m.c_str());
    setTooltip (tip);
    setDescription (headline + ". " + tip);

    // Offer the suggested preset only when it exists and is not already loaded.
    suggestedPreset = juce::String::fromUTF8 (adv.suggestedPreset.c_str());
    const bool offerPreset = suggestedPreset.isNotEmpty() && controller.getPresetManager().findByName (suggestedPreset) != nullptr
                             && controller.getCurrentPresetName() != suggestedPreset;
    presetButton.setVisible (offerPreset);
    if (offerPreset)
    {
        presetButton.setButtonText ("Use " + suggestedPreset);
        presetButton.setTooltip ("Load the preset suggested for this device into the selected strip");
    }

    const bool changed = want != showing;
    showing = want;
    setVisible (want);
    resized();
    repaint();
    return changed;
}

void DeviceAdviceBanner::applySuggestedPreset()
{
    if (const auto* info = controller.getPresetManager().findByName (suggestedPreset))
    {
        juce::String error;
        if (! controller.loadPreset (*info, controller.getSelectedStrip(), error))
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Preset", error);
    }
    refresh();
}

void DeviceAdviceBanner::resized()
{
    auto r = getLocalBounds().reduced (6, 4);
    dismissButton.setBounds (r.removeFromRight (r.getHeight()));
    r.removeFromRight (6);
    detailsButton.setBounds (r.removeFromRight (74));
    if (presetButton.isVisible())
    {
        r.removeFromRight (6);
        const int w = juce::jlimit (90, 240, juce::roundToInt (juce::GlyphArrangement::getStringWidth (Theme::font (13.0f), presetButton.getButtonText())) + 28);
        presetButton.setBounds (r.removeFromRight (w));
    }
}

void DeviceAdviceBanner::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat().reduced (0.5f);
    const auto accent = Palette::amber;
    g.setColour (Palette::panelRaised);
    g.fillRoundedRectangle (bounds, Theme::kControlRadius);
    g.setColour (accent.withAlpha (0.45f));
    g.drawRoundedRectangle (bounds, Theme::kControlRadius, 1.0f);
    g.setColour (accent);
    g.fillRoundedRectangle (bounds.withWidth (4.0f), 2.0f);

    auto r = getLocalBounds().reduced (10, 0);
    drawHeadphones (g, r.removeFromLeft (22).toFloat().reduced (0.0f, 7.0f), accent);
    r.removeFromLeft (8);

    // Text stops before the leftmost visible button.
    int textRight = dismissButton.getX();
    for (auto* b : { &detailsButton, &presetButton })
        if (b->isVisible())
            textRight = juce::jmin (textRight, b->getX());
    r.setRight (textRight - 10);

    const auto headFont = Theme::font (13.0f, true);
    const int headW = juce::jmin (r.getWidth(), juce::roundToInt (juce::GlyphArrangement::getStringWidth (headFont, headline)) + 2);
    g.setFont (headFont);
    g.setColour (Palette::text);
    g.drawText (headline, r.removeFromLeft (headW), juce::Justification::centredLeft, true);
    if (advice.isNotEmpty() && r.getWidth() > 60)
    {
        r.removeFromLeft (12);
        g.setFont (Theme::font (13.0f));
        g.setColour (Palette::muted);
        g.drawText (advice, r, juce::Justification::centredLeft, true);
    }
}
} // namespace flub::app::ui
