#include "HotkeyManager.h"

#include "platform/PlatformBridge.h"

namespace flub::app
{
using BindingResult = flub::platform::GlobalHotkeys::BindingResult;
using flub::platform::KeyChord;

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
    suspended = false;

    auto& settings = controller.getSettings();
    const bool enabled = settings.getHotkeysEnabled();
    unsupported = enabled && ! isSupported();

    std::vector<std::pair<HotkeyAction, KeyChord>> earlier; // valid chords of the actions before
    for (const auto action : AppSettings::getAllHotkeyActions())
    {
        const auto chord = settings.getHotkey (action);
        if (chord.keyCode == 0)
            continue; // unassigned

        ActionStatus status;
        status.chord = AppSettings::chordToString (chord);
        status.status = ! enabled ? Status::SwitchedOff : (unsupported ? Status::NotSupported : Status::Pending);

        // Problems of Flubsound's own settings are named as such, never
        // blamed on another application: an invalid chord, and a chord an
        // earlier action already has (one press would run both; Windows and
        // X11 refuse the second registration anyway).
        if (const auto why = validateChord (chord); why.isNotEmpty())
        {
            if (status.status == Status::Pending)
            {
                status.status = Status::Invalid;
                status.detail = why;
            }
        }
        else
        {
            for (const auto& [other, otherChord] : earlier)
            {
                if (status.status == Status::Pending && AppSettings::sameChord (otherChord, chord))
                {
                    status.status = Status::Conflict;
                    status.detail = AppSettings::getHotkeyActionName (other);
                }
            }
            earlier.emplace_back (action, chord);
        }

        statuses[action] = status;
        if (status.status == Status::Pending)
            registerAction (action, chord);
    }

    if (onStatusChanged != nullptr)
        onStatusChanged();
}

bool HotkeyManager::registerAction (HotkeyAction action, const KeyChord& chord)
{
    // Windows and macOS call back on the message thread; the Linux X11 and
    // Wayland-portal services call from their own X event / D-Bus thread,
    // so hop to the message thread then (see PlatformServices.h,
    // registerHotkey).
    juce::WeakReference<HotkeyManager> weakThis (this);
    const bool ok = hotkeys->registerHotkey (static_cast<int> (action), chord, AppSettings::getHotkeyActionName (action).toStdString(),
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
    return ok;
}

void HotkeyManager::unregisterAll()
{
    if (hotkeys != nullptr)
        hotkeys->unregisterAll();
}

void HotkeyManager::setSuspended (bool shouldBeSuspended)
{
    if (shouldBeSuspended == suspended)
        return;
    if (shouldBeSuspended)
    {
        unregisterAll();
        suspended = true;
    }
    else
    {
        registerAll(); // clears the flag
    }
}

bool HotkeyManager::isProblem (Status status) noexcept
{
    return status == Status::Unavailable || status == Status::Declined || status == Status::Conflict || status == Status::Invalid;
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
        || it->second.status == Status::Conflict || it->second.status == Status::Invalid || refusedAtOnce.count (action) != 0)
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
    if (onStatusChanged != nullptr && ! quiet)
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
        case Status::Unavailable: return "In use by another app";
        case Status::Declined: return "Declined by the desktop";
        case Status::Conflict: return "Same chord as " + status.detail;
        case Status::Invalid: return "Not a valid shortcut";
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
        else if (status.status == Status::Conflict)
            failures.add (name + " is not registered: " + status.detail + " has the same chord");
        else if (status.status == Status::Invalid)
            failures.add (name + " is not a valid shortcut: " + status.detail);
    }
    return failures;
}

std::vector<HotkeyManager::Failure> HotkeyManager::getFailureList() const
{
    std::vector<Failure> list;
    for (const auto action : AppSettings::getAllHotkeyActions())
        if (const auto status = getStatus (action); isProblem (status.status))
            list.push_back ({ action, status });
    return list;
}

juce::String HotkeyManager::describeFailure (const Failure& failure)
{
    const auto& status = failure.status;
    const auto name = AppSettings::getHotkeyActionName (failure.action) + " (" + status.chord + ")";
    switch (status.status)
    {
        case Status::Unavailable: return name + " is in use by another application (or reserved by the system)";
        case Status::Declined: return name + " was declined by the desktop";
        case Status::Conflict: return name + " is the same chord as " + status.detail;
        case Status::Invalid: return name + " is not a valid shortcut: " + status.detail.trimCharactersAtEnd (".");
        case Status::NotAssigned:
        case Status::SwitchedOff:
        case Status::NotSupported:
        case Status::Pending:
        case Status::Registered:
        case Status::Reassigned: break;
    }
    return name + ": " + describe (status);
}

std::vector<HotkeyManager::Failure> HotkeyManager::takeUnannouncedFailures()
{
    auto& settings = controller.getSettings();
    std::vector<Failure> fresh;
    for (const auto action : AppSettings::getAllHotkeyActions())
    {
        const auto status = getStatus (action);
        if (isProblem (status.status))
        {
            if (! settings.isHotkeyFailureAnnounced (action, status.chord))
            {
                fresh.push_back ({ action, status });
                settings.setHotkeyFailureAnnounced (action, status.chord, true);
            }
        }
        else if (status.status == Status::Registered || status.status == Status::Reassigned || status.status == Status::NotAssigned)
        {
            settings.setHotkeyFailureAnnounced (action, {}, false); // working (or gone): a later failure is news again
        }
    }
    return fresh;
}

// ---- Rebinding -----------------------------------------------------------------------------------
juce::String HotkeyManager::validateChord (const KeyChord& chord)
{
    return juce::String::fromUTF8 (platform_bridge::chordProblem (chord).c_str());
}

std::optional<HotkeyAction> HotkeyManager::findConflict (const AppSettings& settings, HotkeyAction action, const KeyChord& chord)
{
    if (chord.keyCode == 0)
        return std::nullopt;
    for (const auto other : AppSettings::getAllHotkeyActions())
        if (other != action && AppSettings::sameChord (settings.getHotkey (other), chord))
            return other;
    return std::nullopt;
}

std::vector<KeyChord> HotkeyManager::freeChordCandidates (const AppSettings& settings, HotkeyAction action)
{
    const auto current = settings.getHotkey (action);
    std::vector<KeyChord> list;
    for (const auto& candidate : AppSettings::getAlternativeHotkeys (action))
        if (! AppSettings::sameChord (candidate, current) && ! findConflict (settings, action, candidate).has_value()
            && validateChord (candidate).isEmpty())
            list.push_back (candidate);
    return list;
}

HotkeyManager::PickResult HotkeyManager::pickFreeChord (HotkeyAction action)
{
    return pickFreeChord (action, freeChordCandidates (controller.getSettings(), action));
}

HotkeyManager::PickResult HotkeyManager::pickFreeChord (HotkeyAction action, const std::vector<KeyChord>& candidates)
{
    PickResult result;
    auto& settings = controller.getSettings();
    const auto name = AppSettings::getHotkeyActionName (action);
    if (! settings.getHotkeysEnabled())
    {
        result.message = "Hotkeys are switched off: switch them on first.";
        return result;
    }
    if (! isSupported())
    {
        result.message = "System-wide hotkeys are not supported here.";
        return result;
    }
    if (suspended)
        registerAll(); // the other chords must hold theirs while this one looks

    const auto before = getStatus (action);
    const bool wasRefused = refusedAtOnce.count (action) != 0;

    // The candidates' own refusals are not news: one change is reported at
    // the end, with the outcome.
    quiet = true;
    for (const auto& candidate : candidates)
    {
        if (validateChord (candidate).isNotEmpty() || findConflict (settings, action, candidate).has_value())
            continue; // never offered to the system
        const auto text = AppSettings::chordToString (candidate);
        result.tried.add (text);

        // Results reported synchronously (Windows, macOS, X11) land on this
        // candidate's status; a refusal makes it Unavailable at once.
        ActionStatus trying;
        trying.status = Status::Pending;
        trying.chord = text;
        statuses[action] = trying;
        refusedAtOnce.erase (action);
        if (registerAction (action, candidate) && ! isProblem (getStatus (action).status))
        {
            settings.setHotkey (action, candidate);
            result.found = true;
            result.chord = candidate;
            result.message = name + " is now " + text
                             + (getStatus (action).status == Status::Pending ? " (waiting for the desktop to confirm it)." : ".");
            break;
        }
    }
    quiet = false;

    if (! result.found)
    {
        statuses[action] = before; // nothing registered for it: as it was
        if (wasRefused)
            refusedAtOnce.insert (action);
        else
            refusedAtOnce.erase (action);
        result.message = result.tried.isEmpty()
                             ? "There is no other combination to try for " + name + ": record one of your own."
                             : "None of " + result.tried.joinIntoString (", ") + " is free: record a combination of your own for " + name + ".";
    }

    if (onStatusChanged != nullptr)
        onStatusChanged();
    return result;
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
