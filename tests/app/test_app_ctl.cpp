// App-level tests: `flubsound-cli ctl` against the running app's
// RemoteControl (shell/RemoteControl.h, docs/11 E56). The client is the
// CLI's own code (flub::cli::ctl::runCtl, run on a worker thread while this
// thread pumps the message loop, as the app's does); the server listens on a
// socket in a private temporary folder; the controller is headless.
// Done-when: `ctl BoostUp Game` changes only Game's Boost (the window shows
// Music, the hotkey strip is Chat) and prints the new value; a bad action is
// refused with exit code 2; with no instance listening, 3.
#include "AppTestSupport.h"

#include "Ctl.h"

#include "engine/EngineController.h"
#include "settings/AppSettings.h"
#include "shell/HotkeyManager.h"
#include "shell/RemoteControl.h"
#include "ui/Osd.h"

#include <atomic>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

using namespace flub::app;
namespace ctl = flub::cli::ctl;

namespace
{
EngineController::Options headlessOptions (const flubapptest::TempFolder& temp)
{
    EngineController::Options o;
    o.openAudioDevice = false;
    o.restoreState = false;
    o.enableAppRouting = false;
    o.settingsFile = temp.file ("settings.xml");
    o.persistSettings = false;
    o.antiCheatServices = [] { return std::vector<std::string>(); };
    return o;
}

float boostOf (EngineController& controller, const char* strip)
{
    return controller.getParams (controller.findStrip (strip)).get (flub::param::BoostIntensity);
}

/** A socket path in a folder only this user can use (as the server requires).
    A Unix socket path has to fit sockaddr_un (104 bytes on macOS, whose
    temporary folders are long): then the folder is a short one under /tmp,
    removed when the test run ends. */
std::string privateSocket (const flubapptest::TempFolder& temp)
{
    auto folder = temp.file ("run");
   #if ! JUCE_WINDOWS
    if (folder.getChildFile ("ctl.sock").getFullPathName().length() > 100)
    {
        struct ShortFolders
        {
            std::vector<juce::File> made;
            ~ShortFolders()
            {
                for (auto& f : made)
                    f.deleteRecursively();
            }
        };
        static ShortFolders shortFolders;
        folder = juce::File ("/tmp").getNonexistentChildFile ("flubctl", {}, false);
        shortFolders.made.push_back (folder);
    }
   #endif
    folder.createDirectory();
    std::filesystem::permissions (folder.getFullPathName().toStdString(), std::filesystem::perms::owner_all,
                                  std::filesystem::perm_options::replace);
    return folder.getChildFile ("ctl.sock").getFullPathName().toStdString();
}

struct CtlResult
{
    int code = -1;
    std::string out, err;
};

/** Runs `flubsound-cli ctl <args> --socket <path>` on a worker thread while
    the message loop runs here (the app's message thread answers). */
CtlResult runCtl (std::vector<std::string> args, const std::string& socket)
{
    args.push_back ("--socket");
    args.push_back (socket);
    CtlResult result;
    std::atomic<bool> done { false };
    std::thread client ([&] {
        result.code = ctl::runCtl (args, result.out, result.err);
        done = true;
    });
    flubapptest::pumpMessagesUntil ([&] { return done.load(); }, 8000);
    client.join();
    return result;
}
} // namespace

TEST_CASE ("App: ctl BoostUp Game changes only Game's Boost in the running app and prints it; bad actions exit 2 (E56)")
{
    const flubapptest::TempFolder temp;
    EngineController controller (headlessOptions (temp));
    HotkeyManager hotkeys (controller, nullptr);
    double clockMs = 0.0;
    ui::Osd::Environment env;
    env.nowMs = [&clockMs] { return clockMs; };
    env.exclusiveFullscreen = [] { return false; };
    env.playEarcon = [] {};
    env.addToDesktop = false;
    ui::Osd osd (controller, env);
    hotkeys.onActionPerformed = [&osd] (HotkeyAction action, const juce::String& feedback) { osd.showFeedback (action, feedback); };

    RemoteControl remote (controller, hotkeys);
    const auto socket = privateSocket (temp);
    juce::String error;
    REQUIRE (remote.start (error, socket));
    REQUIRE (remote.isListening());

    const int game = controller.findStrip ("Game"), music = controller.findStrip ("Music"), chat = controller.findStrip ("Chat");
    REQUIRE (game >= 0);
    REQUIRE (music >= 0);
    REQUIRE (chat >= 0);
    controller.setSelectedStrip (music);    // the window shows Music
    controller.setHotkeyStripName ("Chat"); // the hotkeys act on Chat
    controller.setBoost (0.5f, game);
    const float musicBoost = boostOf (controller, "Music"), chatBoost = boostOf (controller, "Chat");

    auto r = runCtl ({ "BoostUp", "Game" }, socket);
    CHECK (r.code == ctl::kCtlOk);
    CHECK (r.out == "Game: Boost 60%\n");
    CHECK (r.err.empty());
    CHECK (boostOf (controller, "Game") == 0.6f);
    CHECK (boostOf (controller, "Music") == musicBoost);
    CHECK (boostOf (controller, "Chat") == chatBoost);
    CHECK (controller.getSelectedStrip() == music);
    // The same feedback on the on-screen display as the hotkey's.
    CHECK (osd.isShowingMessage());
    CHECK (osd.getTitle() == "GAME");
    CHECK (osd.getText() == "Boost 60%");

    // Without a strip: the hotkey strip, as the hotkey.
    r = runCtl ({ "boostdown" }, socket);
    CHECK (r.code == ctl::kCtlOk);
    CHECK (juce::String (r.out).startsWith ("Chat: Boost "));
    CHECK (boostOf (controller, "Game") == 0.6f);

    // Refused: an unknown action (by the CLI: nothing is sent), an unknown
    // strip and a bad value (by the app). Nothing changes.
    r = runCtl ({ "Explode", "Game" }, socket);
    CHECK (r.code == ctl::kCtlUsage);
    CHECK (r.err.find ("unknown action 'Explode'") != std::string::npos);
    r = runCtl ({ "BoostUp", "Gmae" }, socket);
    CHECK (r.code == ctl::kCtlUsage);
    CHECK (r.err.find ("Unknown strip 'Gmae' (strips: ") != std::string::npos);
    CHECK (r.err.find ("Game") != std::string::npos);
    r = runCtl ({ "Boost", "Game", "150" }, socket);
    CHECK (r.code == ctl::kCtlUsage);
    r = runCtl ({ "Boost", "Game", "loud" }, socket);
    CHECK (r.code == ctl::kCtlUsage);
    CHECK (boostOf (controller, "Game") == 0.6f);
    CHECK (boostOf (controller, "Music") == musicBoost);

    // Boost <strip> <value> sets it.
    r = runCtl ({ "Boost", "Music", "35" }, socket);
    CHECK (r.code == ctl::kCtlOk);
    CHECK (r.out == "Music: Boost 35%\n");
    CHECK_NEAR (boostOf (controller, "Music"), 0.35, 1e-6);
    CHECK (boostOf (controller, "Game") == 0.6f);

    remote.stop();
    CHECK (! remote.isListening());
    r = runCtl ({ "BoostUp", "Game" }, socket);
    CHECK (r.code == ctl::kCtlNotRunning);
    CHECK (boostOf (controller, "Game") == 0.6f);
}

TEST_CASE ("App: ctl loads a preset by uuid, reports refused hotkey actions, shows the window and sets the OSD options (E56)")
{
    const flubapptest::TempFolder temp;
    EngineController controller (headlessOptions (temp));
    HotkeyManager hotkeys (controller, nullptr);
    RemoteControl remote (controller, hotkeys);
    int shown = 0;
    juce::String fedTitle, fedText;
    remote.onShowWindow = [&shown] { ++shown; };
    remote.onFeedback = [&] (const juce::String& title, const juce::String& text, float) { fedTitle = title; fedText = text; };
    const auto socket = privateSocket (temp);
    juce::String error;
    REQUIRE (remote.start (error, socket));

    // Every hotkey action is a ctl action with the same id.
    for (const auto action : AppSettings::getAllHotkeyActions())
    {
        int found = 0;
        for (const auto& info : ctl::actions())
            found += info.hotkeyId == static_cast<int> (action) ? 1 : 0;
        CHECK (found == 1);
    }

    // LoadPreset by uuid, on the named strip only.
    const int game = controller.findStrip ("Game"), music = controller.findStrip ("Music");
    const PresetInfo* target = nullptr;
    for (const auto& p : controller.getPresetManager().getPresets())
        if (p.uuid.isNotEmpty() && p.id != controller.getCurrentPresetId (game))
        {
            target = &p;
            break;
        }
    REQUIRE (target != nullptr);
    const auto uuid = target->uuid.toStdString();
    const auto musicPreset = controller.getCurrentPresetId (music);
    auto r = runCtl ({ "LoadPreset", "Game", uuid }, socket);
    CHECK (r.code == ctl::kCtlOk);
    CHECK (controller.getCurrentPresetId (game) == target->id);
    CHECK (controller.getCurrentPresetId (music) == musicPreset);
    CHECK (r.out == ("Game: preset " + controller.getCurrentPresetName (game) + "\n").toStdString());
    CHECK (fedTitle == "Game");
    CHECK (fedText == "preset " + controller.getCurrentPresetName (game));
    r = runCtl ({ "LoadPreset", "Game", "00000000-0000-0000-0000-000000000000" }, socket);
    CHECK (r.code == ctl::kCtlUsage);
    CHECK (r.err.find ("Unknown preset") != std::string::npos);

    // A hotkey action that cannot apply exits 1 with the hotkey's feedback.
    controller.setMode (flub::param::ModeValue::Music, game);
    r = runCtl ({ "ToggleFocus", "Game" }, socket);
    CHECK (r.code == ctl::kCtlFailed);
    CHECK (r.err == "error: Game: Focus needs Gaming mode\n");
    // A global action takes no strip (refused by the CLI).
    r = runCtl ({ "ToggleEnable", "Game" }, socket);
    CHECK (r.code == ctl::kCtlUsage);
    r = runCtl ({ "ToggleEnable" }, socket);
    CHECK (r.code == ctl::kCtlOk);
    CHECK (r.out == "Flubsound disabled\n");
    CHECK (! controller.isEnabled());

    r = runCtl ({ "Show" }, socket);
    CHECK (r.code == ctl::kCtlOk);
    CHECK (shown == 1);

    auto& settings = controller.getSettings().getPropertiesFile();
    r = runCtl ({ "Osd", "off" }, socket);
    CHECK (r.code == ctl::kCtlOk);
    CHECK (! ui::Osd::getEnabled (settings));
    r = runCtl ({ "Earcon", "fullscreen" }, socket);
    CHECK (r.code == ctl::kCtlOk);
    CHECK (ui::Osd::getEarcon (settings) == ui::Osd::Earcon::Fullscreen);
    r = runCtl ({ "Earcon", "loud" }, socket);
    CHECK (r.code == ctl::kCtlUsage);
    r = runCtl ({ "Osd", "maybe" }, socket);
    CHECK (r.code == ctl::kCtlUsage);
    CHECK (ui::Osd::getEarcon (settings) == ui::Osd::Earcon::Fullscreen);

    // A second instance cannot take the socket over.
    RemoteControl second (controller, hotkeys);
    CHECK (! second.start (error, socket));
    CHECK (error.contains ("another Flubsound instance"));
}
