// App-level tests: the settings file's integrity and schema (docs/11 E52
// Phase B).
//
// * A settings file that does not parse (truncated, garbage, a bad
//   schemaVersion) is moved aside as "<file>.corrupt-<timestamp>" and the
//   newest valid "<file>.bak1" .. ".bak3" restored in its place; with no
//   valid backup the settings start from defaults. It is never overwritten
//   (before, the next autosave replaced it with an empty file: routing,
//   rules and hotkeys silently gone).
// * Backups rotate once per start, only when the file changed since .bak1.
// * "settings.schemaVersion": 1 without it; migratePresetReferences is the
//   one-time 1 -> 2 step (preset references by uuid), and a newer schema is
//   read as it is and never lowered.
// * persist == false touches nothing on disk.
#include "AppTestSupport.h"

#include "settings/AppSettings.h"

#include <map>

using namespace flub::app;

namespace
{
bool writeText (const juce::File& file, const juce::String& text) { return file.replaceWithText (text, false, false, nullptr); } // as it is

juce::Array<juce::File> quarantined (const juce::File& settingsFile)
{
    return settingsFile.getParentDirectory().findChildFiles (juce::File::findFiles, false, settingsFile.getFileName() + ".corrupt-*");
}

/** Opens the file, lets `edit` change it, saves and closes it. */
void session (const juce::File& file, const std::function<void (AppSettings&)>& edit)
{
    AppSettings s (file, true);
    edit (s);
    s.save();
}

AutoProfileRule makeRule (const char* exe, const char* preset)
{
    AutoProfileRule r;
    r.executable = exe;
    r.stripName = "Game";
    r.presetId = preset;
    return r;
}
} // namespace

TEST_CASE ("App settings: a truncated or garbage settings file is quarantined as .corrupt-<timestamp> and the newest valid backup restored (E52)")
{
    const flubapptest::TempFolder temp;
    const auto file = temp.file ("Flubsound Pro.settings");
    const auto bak = [&file] (int i) { return AppSettings::getBackupFile (file, i); };

    // Three starts with three different files: .bak1 is always the newest
    // file that loaded, the one before it .bak2.
    session (file, [] (AppSettings& s) { s.setHotkeyStripName ("Music"); });
    CHECK (! bak (1).exists()); // the first start had no file to back up
    session (file, [] (AppSettings& s) { s.setAppRoutes ({ { "cs2.exe", "Game" } }); });
    REQUIRE (bak (1).existsAsFile());
    CHECK (AppSettings::isValidSettingsFile (bak (1)));
    CHECK (! bak (2).exists());
    session (file, [] (AppSettings& s) { s.setAutoProfileRules ({ makeRule ("cs2.exe", "22e4bf40-b070-485a-8fd6-c4f6cfddeaad") }); });
    REQUIRE (bak (2).existsAsFile());
    CHECK (bak (1).loadFileAsString().contains ("cs2.exe"));   // routes, no rules yet
    CHECK (! bak (1).loadFileAsString().contains ("RULE"));
    CHECK (! bak (2).loadFileAsString().contains ("cs2.exe")); // the hotkey strip only

    // The next start backs up the file with the rules; starts without a
    // change after it do not push the older backups out. Never more than 3.
    session (file, [] (AppSettings&) {});
    CHECK (bak (1).hasIdenticalContentTo (file));
    CHECK (bak (3).existsAsFile());
    const auto bak2Before = bak (2).loadFileAsString(), bak3Before = bak (3).loadFileAsString();
    for (int i = 0; i < 2; ++i)
        session (file, [] (AppSettings&) {});
    CHECK (bak (1).hasIdenticalContentTo (file));
    CHECK (bak (2).loadFileAsString() == bak2Before);
    CHECK (bak (3).loadFileAsString() == bak3Before);
    CHECK (! bak (4).exists());

    // Truncated in the middle of a write: quarantined, .bak1 (the same
    // settings as the last good file) restored.
    const auto good = file.loadFileAsString();
    REQUIRE (writeText (file, good.substring (0, good.length() / 2)));
    {
        AppSettings s (file, true);
        CHECK (s.isValidFile());
        CHECK (s.getRecovery().wasDamaged());
        CHECK (s.getRecovery().restoredFromBackup == 1);
        CHECK (s.getHotkeyStripName() == "Music");
        CHECK (s.getAppRoutes().size() == 1);
        REQUIRE (quarantined (file).size() == 1);
        CHECK (s.getRecovery().quarantined == quarantined (file)[0]);
        const auto stamp = quarantined (file)[0].getFileName().fromFirstOccurrenceOf ("Flubsound Pro.settings.corrupt-", false, false);
        CHECK (stamp.length() == 15); // yyyymmdd-hhmmss
        CHECK (stamp.substring (8, 9) == "-");
        CHECK (stamp.removeCharacters ("-").containsOnly ("0123456789"));
        CHECK (quarantined (file)[0].loadFileAsString() == good.substring (0, good.length() / 2)); // kept as it was
    }
    CHECK (AppSettings::isValidSettingsFile (file));

    // Garbage, and .bak1 damaged too: the next valid backup (.bak2) is used.
    REQUIRE (writeText (file, "\x01\x02 not a settings file"));
    REQUIRE (writeText (bak (1), "<PROPERTIES><VALUE name=\"x\" val=\"1\""));
    {
        AppSettings s (file, true);
        CHECK (s.getRecovery().restoredFromBackup == 2);
        CHECK (s.getHotkeyStripName() == "Music");
        CHECK (quarantined (file).size() == 2);
    }

    // No valid backup at all: defaults, the damaged file still kept aside.
    for (int i = 1; i <= AppSettings::kNumBackups; ++i)
        bak (i).deleteFile();
    REQUIRE (writeText (file, ""));
    {
        AppSettings s (file, true);
        CHECK (s.getRecovery().wasDamaged());
        CHECK (s.getRecovery().restoredFromBackup == 0);
        CHECK (s.getHotkeyStripName() == "Game"); // the default
        CHECK (s.getAppRoutes().empty());
        s.setHotkeyStripName ("Chat");
    }
    CHECK (quarantined (file).size() == 3);
    CHECK (AppSettings::isValidSettingsFile (file));
    CHECK (AppSettings (file, false).getHotkeyStripName() == "Chat");
}

TEST_CASE ("App settings: without persistence a damaged file is left alone; a bad schemaVersion counts as damage, a newer one does not (E52)")
{
    const flubapptest::TempFolder temp;
    const auto file = temp.file ("settings.xml");
    CHECK (! AppSettings::isValidSettingsFile (file)); // missing
    REQUIRE (writeText (file, "garbage"));
    {
        AppSettings s (file, false);
        CHECK (! s.isValidFile());
        CHECK (! s.getRecovery().wasDamaged());
        s.setHotkeyStripName ("Music");
    }
    CHECK (file.loadFileAsString() == "garbage");
    CHECK (quarantined (file).isEmpty());
    CHECK (! AppSettings::getBackupFile (file, 1).exists());

    const auto withSchema = [] (const char* version)
    {
        return juce::String ("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<PROPERTIES>\n  <VALUE name=\"settings.schemaVersion\" val=\"")
               + version + "\"/>\n  <VALUE name=\"hotkeys.strip\" val=\"Music\"/>\n</PROPERTIES>\n";
    };
    REQUIRE (writeText (file, withSchema ("two")));
    CHECK (! AppSettings::isValidSettingsFile (file));
    REQUIRE (writeText (file, withSchema ("7")));
    CHECK (AppSettings::isValidSettingsFile (file));
    {
        AppSettings s (file, true);
        CHECK (! s.getRecovery().wasDamaged());
        CHECK (s.getSchemaVersion() == 7);
        CHECK (s.migratePresetReferences ({ { "factory:x", "uuid-x" } }) == 0); // never lowered, nothing rewritten
        CHECK (s.getSchemaVersion() == 7);
        CHECK (s.getHotkeyStripName() == "Music");
    }
}

TEST_CASE ("App settings: the one-time schema 1 -> 2 step rewrites strip presets and rule presets through the alias map, once (E52)")
{
    const flubapptest::TempFolder temp;
    const auto file = temp.file ("settings.xml");
    {
        AppSettings s (file, true);
        CHECK (s.getSchemaVersion() == 1); // no "settings.schemaVersion" yet
        s.setLastPreset ("Game", "factory:gaming-competitive-fps");
        s.setLastPreset ("Music", "user:Mine.flubpreset.json");
        s.setLastPreset ("Chat", "user:deleted.flubpreset.json");
        s.setAutoProfileRules ({ makeRule ("cs2.exe", "factory:gaming-competitive-fps"), makeRule ("ghost.exe", "user:deleted.flubpreset.json") });
    }

    const std::map<juce::String, juce::String> aliases { { "factory:gaming-competitive-fps", "22e4bf40-b070-485a-8fd6-c4f6cfddeaad" },
                                                         { "user:Mine.flubpreset.json", "0f6f5d2e-1c1b-4b6e-9d1e-2a3b4c5d6e7f" } };
    {
        AppSettings s (file, true);
        CHECK (s.migratePresetReferences (aliases) == 3);
        CHECK (s.getSchemaVersion() == AppSettings::kSchemaVersion);
        CHECK (s.getLastPreset ("Game") == "22e4bf40-b070-485a-8fd6-c4f6cfddeaad");
        CHECK (s.getLastPreset ("Music") == "0f6f5d2e-1c1b-4b6e-9d1e-2a3b4c5d6e7f");
        CHECK (s.getLastPreset ("Chat") == "user:deleted.flubpreset.json"); // unknown: kept, shown as missing
        const auto rules = s.getAutoProfileRules();
        REQUIRE (rules.size() == 2);
        CHECK (rules[0].presetId == "22e4bf40-b070-485a-8fd6-c4f6cfddeaad");
        CHECK (rules[1].presetId == "user:deleted.flubpreset.json");
    }

    // Once: a later legacy value (e.g. written by an older build) is not
    // rewritten again from the settings side.
    AppSettings again (file, true);
    CHECK (again.getSchemaVersion() == AppSettings::kSchemaVersion);
    again.setLastPreset ("Game", "factory:gaming-competitive-fps");
    CHECK (again.migratePresetReferences (aliases) == 0);
    CHECK (again.getLastPreset ("Game") == "factory:gaming-competitive-fps");
}
