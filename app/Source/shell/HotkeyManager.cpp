#include "HotkeyManager.h"

#include "platform/PlatformBridge.h"

namespace flub::app
{
using BindingResult = flub::platform::GlobalHotkeys::BindingResult;

HotkeyManager::HotkeyManager (EngineController& c)
    : HotkeyManager (c, platform_bridge::createGlobalHotkeys())
{
}

HotkeyManager::HotkeyManager (EngineController& c, std::unique_ptr<flub::platform::GlobalHotkeys> service)
    : controller (c), hotkeys (std::move (service))
{
    if (hotkeys == nullptr)
        return;

    // Results come synchronously from registerHotkey() on this thread, or
    // later from the service's own thread (Wayland portal): hop over then.
    juce::WeakReference<HotkeyManager> weakThis (this);
    hotkeys->setBindingListener ([weakThis] (const BindingResult& result)
                                 {
                                     if (juce::MessageManager::existsAndIsCurrentThread())
                                     {
                                         if (auto* self = weakThis.get())
                                             self->applyBindingResult (result);
                                         return;
                                     }
                                     juce::MessageManager::callAsync (
                                         [weakThis, result]
                                         {
                                             if (auto* self = weakThis.get())
                                                 self->applyBindingResult (result);
                                         });
                                 });
}

HotkeyManager::~HotkeyManager()
{
    unregisterAll();
    if (hotkeys != nullptr)
        hotkeys->setBindingListener (nullptr);
}

bool HotkeyManager::isSupported() const noexcept
{
    return hotkeys != nullptr && hotkeys->isSupported();
}

void HotkeyManager::registerAll()
{
    unregisterAll();
    statuses.clear();
    refusedAtOnce.clear();

    auto& settings = controller.getSettings();
    const bool enabled = settings.getHotkeysEnabled();
    unsupported = enabled && ! isSupported();

    for (const auto action : AppSettings::getAllHotkeyActions())
    {
        const auto chord = settings.getHotkey (action);
        if (chord.keyCode == 0)
            continue; // unassigned

        ActionStatus status;
        status.chord = AppSettings::chordToString (chord);
        status.status = ! enabled ? Status::SwitchedOff : (unsupported ? Status::NotSupported : Status::Pending);
        statuses[action] = status;
        if (status.status != Status::Pending)
            continue;

        // Windows and macOS call back on the message thread; the Linux X11 and
        // Wayland-portal services call from their own X event / D-Bus thread,
        // so hop to the message thread then (see PlatformServices.h,
        // registerHotkey).
        juce::WeakReference<HotkeyManager> weakThis (this);
        const bool ok = hotkeys->registerHotkey (static_cast<int> (action), chord,
                                                 AppSettings::getHotkeyActionName (action).toStdString(),
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

        // A refusal is reported through the listener as well; this covers a
        // service that does not report. It is final for this registerAll():
        // a result of an earlier one still queued for the message thread
        // must not overwrite it.
        if (! ok)
        {
            statuses[action].status = Status::Unavailable;
            refusedAtOnce.insert (action);
        }
    }

    if (onStatusChanged != nullptr)
        onStatusChanged();
}

void HotkeyManager::unregisterAll()
{
    if (hotkeys != nullptr)
        hotkeys->unregisterAll();
}

HotkeyManager::ActionStatus HotkeyManager::getStatus (HotkeyAction action) const
{
    const auto it = statuses.find (action);
    return it != statuses.end() ? it->second : ActionStatus {};
}

void HotkeyManager::applyBindingResult (const BindingResult& result)
{
    const auto action = static_cast<HotkeyAction> (result.id);
    const auto it = statuses.find (action);
    if (it == statuses.end() || it->second.status == Status::SwitchedOff || it->second.status == Status::NotSupported
        || refusedAtOnce.count (action) != 0)
        return; // not requested by the last registerAll() (a late result of an earlier one)

    auto updated = it->second;
    updated.trigger.clear();
    switch (result.status)
    {
        case BindingResult::Status::Registered: updated.status = Status::Registered; break;
        case BindingResult::Status::Reassigned:
            updated.status = Status::Reassigned;
            updated.trigger = juce::String::fromUTF8 (result.trigger.c_str());
            break;
        case BindingResult::Status::Unavailable: updated.status = Status::Unavailable; break;
        case BindingResult::Status::Declined: updated.status = Status::Declined; break;
    }
    setStatus (action, updated);
}

void HotkeyManager::setStatus (HotkeyAction action, ActionStatus status)
{
    auto& current = statuses[action];
    if (current.status == status.status && current.chord == status.chord && current.trigger == status.trigger)
        return;
    current = std::move (status);
    if (onStatusChanged != nullptr)
        onStatusChanged();
}

juce::String HotkeyManager::describe (const ActionStatus& status)
{
    switch (status.status)
    {
        case Status::NotAssigned: return "Not assigned";
        case Status::SwitchedOff: return "Off";
        case Status::NotSupported: return "Not supported here";
        case Status::Pending: return "Waiting for the desktop";
        case Status::Registered: return "Registered";
        case Status::Reassigned: return "Bound by the desktop as " + status.trigger;
        case Status::Unavailable: return "In use / could not register";
        case Status::Declined: return "Declined by the desktop";
    }
    return {};
}

juce::StringArray HotkeyManager::getFailures() const
{
    juce::StringArray failures;
    if (unsupported)
        failures.add ("Global hotkeys are not supported on this system");

    for (const auto& [action, status] : statuses)
    {
        const auto name = AppSettings::getHotkeyActionName (action) + " (" + status.chord + ")";
        if (status.status == Status::Unavailable)
            failures.add (name + " could not be registered: another application may already use it, or the system does not allow that key");
        else if (status.status == Status::Declined)
            failures.add (name + " was declined by the desktop: bind it in the desktop's keyboard settings, or choose another chord");
    }
    return failures;
}

juce::String HotkeyManager::perform (HotkeyAction action)
{
    // Strip actions go to the hotkey strip (an active automatic profile's,
    // else Settings > Hotkeys', default Game), never to the strip selected in
    // the window, and the feedback names it (docs/11 E56).
    return perform (action, controller.getHotkeyStrip());
}

juce::String HotkeyManager::perform (HotkeyAction action, int strip)
{
    const auto stripName = controller.getStripName (strip) + ": ";
    juce::String feedback;
    switch (action)
    {
        case HotkeyAction::ToggleEnable:
            controller.toggleEnabled();
            feedback = controller.isEnabled() ? "Flubsound enabled" : "Flubsound disabled";
            break;
        case HotkeyAction::ToggleMode:
            controller.toggleMode (strip);
            feedback = stripName + (controller.getMode (strip) == flub::param::ModeValue::Gaming ? "Gaming" : "Music") + " mode";
            break;
        case HotkeyAction::BoostUp:
        case HotkeyAction::BoostDown:
            controller.nudgeBoost (action == HotkeyAction::BoostUp ? 0.1f : -0.1f, strip);
            feedback = stripName + "Boost " + juce::String (juce::roundToInt (controller.getBoost (strip) * 100.0f)) + "%";
            break;
        case HotkeyAction::NextPreset:
        case HotkeyAction::PreviousPreset:
            if (action == HotkeyAction::NextPreset ? controller.nextPreset (strip) : controller.previousPreset (strip))
                feedback = stripName + "preset " + controller.getCurrentPresetName (strip);
            else
                feedback = "No presets available";
            break;
        case HotkeyAction::ToggleFocus:
            if (controller.setFocus (strip, ! controller.isFocused (strip)))
                feedback = stripName + (controller.isFocused (strip) ? "Focus on (Footsteps 100%)" : "Focus off");
            else
                feedback = stripName + "Focus needs Gaming mode";
            break;
        case HotkeyAction::ChatMixToChat:
        case HotkeyAction::ChatMixToGame:
            if (controller.nudgeChatMix (action == HotkeyAction::ChatMixToChat ? kChatMixStep : -kChatMixStep))
                feedback = "ChatMix " + controller.describeChatMix();
            else
                feedback = "ChatMix needs a Game and a Chat strip";
            break;
        case HotkeyAction::ToggleNight:
            controller.setNight (strip, ! controller.isNight (strip));
            feedback = stripName + (controller.isNight (strip) ? "Night listening on" : "Night listening off");
            break;
        case HotkeyAction::ToggleBypass:
        {
            controller.setStripBypassed (strip, ! controller.isStripBypassed (strip));
            const bool matched = controller.getParams (strip).get (flub::param::LoudnessMatchBypass) >= 0.5f;
            feedback = stripName + (controller.isStripBypassed (strip) ? (matched ? "bypassed (loudness matched)" : "bypassed") : "processing");
            break;
        }
    }

    // The on-screen display (or the tray bubble) shows it; `ctl` also
    // prints it.
    if (onActionPerformed != nullptr)
        onActionPerformed (action, feedback);
    return feedback;
}
} // namespace flub::app
