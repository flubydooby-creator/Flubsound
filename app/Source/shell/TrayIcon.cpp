#include "TrayIcon.h"

namespace flub::app
{
using flub::param::ModeValue;

TrayIcon::TrayIcon (EngineController& c, Callbacks cb)
    : controller (c), callbacks (std::move (cb))
{
    controller.addListener (this);
    updateIcon();
}

TrayIcon::~TrayIcon()
{
    controller.removeListener (this);
}

juce::Image TrayIcon::createIconImage (bool enabled, bool asTemplate, int size)
{
    juce::Image image (juce::Image::ARGB, size, size, true);
    juce::Graphics g (image);

    const auto s = static_cast<float> (size);
    const auto bounds = juce::Rectangle<float> (0.0f, 0.0f, s, s).reduced (s * 0.06f);

    if (! asTemplate)
    {
        const auto top = enabled ? juce::Colour (0xff22d3c5) : juce::Colour (0xff6b7280);
        const auto bottom = enabled ? juce::Colour (0xff3b5bf6) : juce::Colour (0xff374151);
        g.setGradientFill (juce::ColourGradient (top, bounds.getTopLeft(), bottom, bounds.getBottomRight(), false));
        g.fillRoundedRectangle (bounds, s * 0.22f);
    }

    // Five "equaliser" bars.
    static constexpr float heights[] = { 0.34f, 0.62f, 0.86f, 0.56f, 0.3f };
    const float barArea = bounds.getWidth() * 0.66f;
    const float barWidth = barArea / 9.0f;
    const float x0 = bounds.getCentreX() - barArea * 0.5f;
    g.setColour (asTemplate ? juce::Colours::black : juce::Colours::white.withAlpha (enabled ? 1.0f : 0.7f));
    for (int i = 0; i < 5; ++i)
    {
        const float h = bounds.getHeight() * 0.72f * heights[i];
        g.fillRoundedRectangle (x0 + static_cast<float> (i) * 2.0f * barWidth, bounds.getCentreY() - h * 0.5f, barWidth, h, barWidth * 0.5f);
    }
    return image;
}

void TrayIcon::updateIcon()
{
    const bool enabled = controller.isEnabled();
    if (iconValid && enabled == iconShowsEnabled)
        return;

    setIconImage (createIconImage (enabled, false), createIconImage (enabled, true));
    setIconTooltip (juce::String ("Flubsound Pro - ") + (enabled ? "enabled" : "bypassed"));
    iconShowsEnabled = enabled;
    iconValid = true;
}

void TrayIcon::engineControllerChanged (EngineController::Change change)
{
    if (change == EngineController::Change::MasterEnable)
        updateIcon();

    // One bubble when a CPU overload starts (the watchdog broadcasts
    // Change::Device on both edges); the header readout shows it until it ends.
    if (change == EngineController::Change::Device)
    {
        const auto& overload = controller.getOverloadState();
        if (overload.overloaded && ! overloadNotified)
            notify ("Flubsound Pro - audio overload",
                    "The audio engine is running out of time (peak " + juce::String (juce::roundToInt (overload.peakLoad * 100.0)) + " % CPU"
                        + (overload.episodeGlitches > 0 ? ", " + juce::String (static_cast<juce::int64> (overload.episodeGlitches)) + " dropouts" : juce::String())
                        + "). Try the Low Latency profile or a larger buffer.");
        overloadNotified = overload.overloaded;

        // One bubble per automatic latency-profile step (opt-in overload response).
        const auto steps = controller.getLoadReductionState().sessionSteps;
        if (steps > reductionsNotified && controller.hasReducedLoad())
            notify ("Flubsound Pro - processing load reduced", controller.describeLoadReduction());
        reductionsNotified = steps;
    }
}

void TrayIcon::notify (const juce::String& title, const juce::String& message)
{
    showInfoBubble (title, message);
}

juce::PopupMenu TrayIcon::buildMenu()
{
    juce::PopupMenu menu;
    const int strip = controller.getSelectedStrip();
    menu.addSectionHeader ("Flubsound Pro - " + controller.getStripName (strip));

    menu.addItem ("Enabled", true, controller.isEnabled(), [this] { controller.toggleEnabled(); });
    menu.addSeparator();

    const auto mode = controller.getMode();
    menu.addItem ("Music Mode", true, mode == ModeValue::Music, [this] { controller.setMode (ModeValue::Music); });
    menu.addItem ("Gaming Mode", true, mode == ModeValue::Gaming, [this] { controller.setMode (ModeValue::Gaming); });
    menu.addSeparator();

    const int boostPercent = juce::roundToInt (controller.getBoost() * 100.0f);
    menu.addItem ("Boost +10%   (now " + juce::String (boostPercent) + "%)", boostPercent < 100, false,
                  [this] { controller.nudgeBoost (0.1f); });
    menu.addItem ("Boost -10%", boostPercent > 0, false, [this] { controller.nudgeBoost (-0.1f); });
    menu.addSeparator();

    // Factory preset quick list (grouped by category when there are several).
    juce::PopupMenu presetMenu;
    const auto factory = controller.getPresetManager().getFactoryPresets();
    const auto currentId = controller.getCurrentPresetId();
    if (factory.empty())
    {
        presetMenu.addItem ("No factory presets", false, false, nullptr);
    }
    else
    {
        juce::StringArray categories;
        for (const auto& p : factory)
            categories.addIfNotAlreadyThere (p.category);

        for (const auto& category : categories)
        {
            juce::PopupMenu sub;
            for (const auto& p : factory)
            {
                if (p.category != category)
                    continue;
                const auto id = p.id;
                sub.addItem (p.name, true, p.id == currentId,
                             [this, id]
                             {
                                 juce::String error;
                                 controller.loadPreset (id, -1, error);
                             });
            }
            if (categories.size() == 1)
                presetMenu = sub;
            else
                presetMenu.addSubMenu (category, sub);
        }
    }
    menu.addSubMenu ("Presets", presetMenu);
    menu.addSeparator();

    menu.addItem ("Open Flubsound Pro", [this]
                  {
                      if (callbacks.openWindow != nullptr)
                          callbacks.openWindow();
                  });
    menu.addItem ("Quit", [this]
                  {
                      if (callbacks.quit != nullptr)
                          callbacks.quit();
                  });
    return menu;
}

void TrayIcon::mouseDown (const juce::MouseEvent& e)
{
#if JUCE_MAC
    juce::ignoreUnused (e);
    showDropdownMenu (buildMenu());
#else
    if (e.mods.isPopupMenu())
    {
        // Windows only dismisses tray menus correctly for the foreground process.
        juce::Process::makeForegroundProcess();
        buildMenu().showMenuAsync (juce::PopupMenu::Options().withMousePosition().withDeletionCheck (*this));
    }
    else if (callbacks.openWindow != nullptr)
    {
        callbacks.openWindow();
    }
#endif
}
} // namespace flub::app
