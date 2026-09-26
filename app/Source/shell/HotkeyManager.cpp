#include "HotkeyManager.h"

#include "platform/PlatformBridge.h"

namespace flub::app
{
HotkeyManager::HotkeyManager (EngineController& c)
    : controller (c), hotkeys (platform_bridge::createGlobalHotkeys())
{
}

HotkeyManager::~HotkeyManager()
{
    unregisterAll();
}

bool HotkeyManager::isSupported() const noexcept
{
    return hotkeys != nullptr && hotkeys->isSupported();
}

void HotkeyManager::registerAll()
{
    unregisterAll();
    failures.clear();

    auto& settings = controller.getSettings();
    if (! settings.getHotkeysEnabled())
        return;

    if (! isSupported())
    {
        failures.add ("Global hotkeys are not supported on this system");
        return;
    }

    for (const auto action : AppSettings::getAllHotkeyActions())
    {
        const auto chord = settings.getHotkey (action);
        if (chord.keyCode == 0)
            continue; // unassigned

        // The service promises message-thread callbacks; hop over defensively
        // if an implementation ever calls from its own thread.
        juce::WeakReference<HotkeyManager> weakThis (this);
        const bool ok = hotkeys->registerHotkey (static_cast<int> (action), chord,
                                                 [weakThis, action]
                                                 {
                                                     if (juce::MessageManager::existsAndIsCurrentThread())
                                                     {
                                                         if (auto* self = weakThis.get())
                                                             self->perform (action);
                                                         return;
                                                     }
                                                     juce::MessageManager::callAsync (
                                                         [weakThis, action]
                                                         {
                                                             if (auto* self = weakThis.get())
                                                                 self->perform (action);
                                                         });
                                                 });
        if (! ok)
            failures.add (AppSettings::getHotkeyActionName (action) + " (" + AppSettings::chordToString (chord) + ") is in use by another application");
    }
}

void HotkeyManager::unregisterAll()
{
    if (hotkeys != nullptr)
        hotkeys->unregisterAll();
}

void HotkeyManager::perform (HotkeyAction action)
{
    juce::String feedback;
    switch (action)
    {
        case HotkeyAction::ToggleEnable:
            controller.toggleEnabled();
            feedback = controller.isEnabled() ? "Flubsound enabled" : "Flubsound disabled";
            break;
        case HotkeyAction::ToggleMode:
            controller.toggleMode();
            feedback = controller.getStripName (controller.getSelectedStrip()) + ": "
                       + (controller.getMode() == flub::param::ModeValue::Gaming ? "Gaming" : "Music") + " mode";
            break;
        case HotkeyAction::BoostUp:
        case HotkeyAction::BoostDown:
            controller.nudgeBoost (action == HotkeyAction::BoostUp ? 0.1f : -0.1f);
            feedback = "Boost " + juce::String (juce::roundToInt (controller.getBoost() * 100.0f)) + "%";
            break;
        case HotkeyAction::NextPreset:
        case HotkeyAction::PreviousPreset:
            if (action == HotkeyAction::NextPreset ? controller.nextPreset() : controller.previousPreset())
                feedback = "Preset: " + controller.getCurrentPresetName();
            else
                feedback = "No presets available";
            break;
    }

    if (onActionPerformed != nullptr)
        onActionPerformed (action, feedback);
}
} // namespace flub::app
