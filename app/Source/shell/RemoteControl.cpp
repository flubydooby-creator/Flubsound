#include "RemoteControl.h"

#include "ui/Osd.h"

#include <atomic>

namespace flub::app
{
namespace ctl = flub::cli::ctl;

struct RemoteControl::Shared
{
    std::atomic<bool> stopping { false };
};

namespace
{
ctl::Reply ok (const juce::String& message) { return { ctl::Status::Ok, message.toStdString() }; }
ctl::Reply usage (const juce::String& message) { return { ctl::Status::Usage, message.toStdString() }; }
ctl::Reply failed (const juce::String& message) { return { ctl::Status::Failed, message.toStdString() }; }

/** Feedback of a hotkey action that did not apply (HotkeyManager::perform's
    texts): `ctl` exits with 1 then. */
bool isRefusal (HotkeyAction action, const juce::String& feedback)
{
    switch (action)
    {
        case HotkeyAction::NextPreset:
        case HotkeyAction::PreviousPreset: return feedback == "No presets available";
        case HotkeyAction::ToggleFocus: return feedback.endsWith ("Focus needs Gaming mode");
        case HotkeyAction::ChatMixToChat:
        case HotkeyAction::ChatMixToGame: return feedback.startsWith ("ChatMix needs");
        case HotkeyAction::ToggleEnable:
        case HotkeyAction::ToggleMode:
        case HotkeyAction::BoostUp:
        case HotkeyAction::BoostDown:
        case HotkeyAction::ToggleNight:
        case HotkeyAction::ToggleBypass: break;
    }
    return false;
}

std::optional<bool> parseSwitch (const juce::String& value)
{
    const auto v = value.trim().toLowerCase();
    if (v == "on" || v == "1" || v == "true" || v == "yes")
        return true;
    if (v == "off" || v == "0" || v == "false" || v == "no")
        return false;
    return std::nullopt;
}
} // namespace

RemoteControl::RemoteControl (EngineController& c, HotkeyManager& h)
    : controller (c), hotkeys (h), shared (std::make_shared<Shared>())
{
}

RemoteControl::~RemoteControl() { stop(); }

bool RemoteControl::start (juce::String& error, const std::string& socketPath)
{
    stop();
    shared = std::make_shared<Shared>();

    // The socket thread hands each request to the message thread and waits
    // (bounded) for the reply; stop() ends a wait within 20 ms.
    juce::WeakReference<RemoteControl> weakThis (this);
    auto state = shared;
    auto handler = [weakThis, state] (const ctl::Request& request) -> ctl::Reply
    {
        struct Job
        {
            ctl::Request request;
            ctl::Reply reply;
            juce::WaitableEvent done;
        };
        auto job = std::make_shared<Job>();
        job->request = request;
        const bool posted = juce::MessageManager::callAsync ([weakThis, job]
                                                             {
                                                                 if (auto* self = weakThis.get())
                                                                     job->reply = self->handle (job->request);
                                                                 else
                                                                     job->reply = { ctl::Status::Failed, "Flubsound is closing" };
                                                                 job->done.signal();
                                                             });
        if (! posted)
            return { ctl::Status::Failed, "Flubsound is closing" };
        for (int waited = 0; waited < kReplyTimeoutMs; waited += 20)
        {
            if (job->done.wait (20))
                return job->reply;
            if (state->stopping.load())
                return { ctl::Status::Failed, "Flubsound is closing" };
        }
        return { ctl::Status::Failed, "Flubsound did not answer in time" };
    };

    std::string message;
    if (! server.start (socketPath, std::move (handler), message))
    {
        error = juce::String::fromUTF8 (message.c_str());
        return false;
    }
    return true;
}

void RemoteControl::stop()
{
    if (shared != nullptr)
        shared->stopping = true;
    server.stop();
}

ctl::Reply RemoteControl::handle (const ctl::Request& request)
{
    const auto* info = ctl::findAction (request.action);
    if (info == nullptr)
        return usage ("Unknown action " + juce::String::fromUTF8 (request.action.c_str()));

    // The strip: the one named (it must exist), else the hotkey strip.
    int strip = -1;
    if (info->takesStrip)
    {
        const auto name = juce::String::fromUTF8 (request.strip.c_str());
        strip = name.isNotEmpty() ? controller.findStrip (name) : controller.getHotkeyStrip();
        if (strip < 0)
        {
            juce::StringArray names;
            for (int i = 0; i < controller.getNumStrips(); ++i)
                names.add (controller.getStripName (i));
            return usage ("Unknown strip '" + name + "' (strips: " + names.joinIntoString (", ") + ")");
        }
    }
    const auto value = juce::String::fromUTF8 (request.value.c_str());
    const auto stripName = strip >= 0 ? controller.getStripName (strip) : juce::String();

    // Hotkey actions: exactly what the hotkey does (feedback, display).
    if (info->hotkeyId != 0)
    {
        const auto action = static_cast<HotkeyAction> (info->hotkeyId);
        const auto feedback = info->takesStrip ? hotkeys.perform (action, strip) : hotkeys.perform (action);
        return isRefusal (action, feedback) ? failed (feedback) : ok (feedback);
    }

    const juce::String name (info->name);
    if (name == "Boost")
    {
        auto number = value.trim();
        if (number.endsWithChar ('%'))
            number = number.dropLastCharacters (1).trim();
        if (number.isEmpty() || ! number.containsOnly ("0123456789.") || number.getDoubleValue() > 100.0)
            return usage ("Boost takes 0-100 (percent), not '" + value + "'");
        controller.setBoost (static_cast<float> (number.getDoubleValue() / 100.0), strip);
        const float boost = controller.getBoost (strip);
        const auto text = "Boost " + juce::String (juce::roundToInt (boost * 100.0f)) + "%";
        if (onFeedback != nullptr)
            onFeedback (stripName, text, boost);
        return ok (stripName + ": " + text);
    }
    if (name == "LoadPreset")
    {
        if (controller.getPresetManager().findById (value) == nullptr)
            return usage ("Unknown preset: " + value + " (a preset's uuid; `flubsound-cli presets` lists the factory presets)");
        juce::String error;
        if (! controller.loadPreset (value, strip, error))
            return failed (error.isNotEmpty() ? error : juce::String ("The preset could not be loaded"));
        const auto text = "preset " + controller.getCurrentPresetName (strip);
        if (onFeedback != nullptr)
            onFeedback (stripName, text, -1.0f);
        return ok (stripName + ": " + text);
    }
    if (name == "Show")
    {
        if (onShowWindow == nullptr)
            return failed ("No window to show");
        onShowWindow();
        return ok ("Window shown");
    }
    if (name == "Osd")
    {
        const auto on = parseSwitch (value);
        if (! on.has_value())
            return usage ("Osd takes on or off, not '" + value + "'");
        ui::Osd::setEnabled (controller.getSettings().getPropertiesFile(), *on);
        return ok (juce::String ("On-screen display ") + (*on ? "on" : "off"));
    }
    if (name == "Earcon")
    {
        const auto earcon = ui::Osd::parseEarcon (value);
        if (! earcon.has_value())
            return usage ("Earcon takes off, fullscreen or always, not '" + value + "'");
        ui::Osd::setEarcon (controller.getSettings().getPropertiesFile(), *earcon);
        return ok ("Earcon " + ui::Osd::getEarconName (*earcon));
    }
    return usage ("Unknown action " + name);
}
} // namespace flub::app
