// App-level tests: preset identity in the app (docs/11 E52 Phase B).
//
// * A preset's id is its uuid: factory presets carry a permanent one; a user
//   preset without one (or with one another preset has) is given a new uuid,
//   written into its file once. Legacy ids ("factory:<stem>", "user:<file>")
//   and uuids in any case still find the preset.
// * Save As writes a new uuid, overwriting keeps it, a rename keeps it; user
//   presets are written with every sound parameter (full state).
// * Import keeps a unique uuid and replaces a taken one.
// * Reader warnings (unknown keys, clamps) reach onPresetWarnings on import
//   and on every load into a strip, for the UI's toast.
#include "AppTestSupport.h"

#include "presets/PresetManager.h"

#include "flub/io/Json.h"
#include "flub/io/PresetIO.h"

#include <string>
#include <vector>

using namespace flub::app;
using namespace flub::param;

namespace
{
constexpr const char* kFpsUuid = "22e4bf40-b070-485a-8fd6-c4f6cfddeaad"; // presets/factory/gaming-competitive-fps.json

bool writeText (const juce::File& file, const juce::String& text) { return file.replaceWithText (text, false, false, "\n"); }

flub::json::Value readJson (const juce::File& file)
{
    flub::json::Value root;
    std::string error;
    flub::json::parse (file.loadFileAsString().toStdString(), root, error);
    return root;
}

std::string uuidInFile (const juce::File& file) { return readJson (file)["uuid"].asString(); }

const PresetInfo* byFile (const PresetManager& m, const juce::File& file)
{
    for (const auto& p : m.getPresets())
        if (p.file == file)
            return &p;
    return nullptr;
}

struct WarningLog
{
    juce::StringArray presets, warnings;

    void attach (PresetManager& m)
    {
        m.onPresetWarnings = [this] (const PresetInfo& p, const juce::StringArray& w)
        {
            presets.add (p.name);
            warnings.addArray (w);
        };
    }
};
} // namespace

TEST_CASE ("App presets: ids are uuids - factory presets keep their permanent uuid, legacy ids and any-case uuids still find them (E52)")
{
    PresetManager manager;
    const auto* fps = manager.findById ("factory:gaming-competitive-fps");
    REQUIRE (fps != nullptr);
    CHECK (fps->id == kFpsUuid);
    CHECK (fps->uuid == kFpsUuid);
    CHECK (fps->legacyId == "factory:gaming-competitive-fps");
    CHECK (manager.findById (kFpsUuid) == fps);
    CHECK (manager.findById (juce::String (kFpsUuid).toUpperCase()) == fps);
    CHECK (manager.findById ("factory:no-such-preset") == nullptr);
    CHECK (manager.findById ({}) == nullptr);

    // Every factory preset has one, and the alias map covers them all.
    const auto aliases = manager.getLegacyIdAliases();
    for (const auto& p : manager.getFactoryPresets())
    {
        CHECK (flub::preset::isValidUuid (p.id.toStdString()));
        CHECK (aliases.count (p.legacyId) == 1);
    }

    // A strip marked with a legacy id holds the preset's id.
    manager.setCurrentPresetId (0, "factory:gaming-competitive-fps");
    CHECK (manager.getCurrentPresetId (0) == kFpsUuid);
    manager.setCurrentPresetId (0, "user:gone.flubpreset.json"); // unknown: kept as it is
    CHECK (manager.getCurrentPresetId (0) == "user:gone.flubpreset.json");
}

TEST_CASE ("App presets: a user preset without a uuid gets one written once; a copied file gets its own; Save As, overwrite and rename (E52)")
{
    const flubapptest::TempFolder temp;
    const auto folder = temp.file ("Presets");
    REQUIRE (folder.createDirectory().wasOk());

    // Saved before E52 (version 1, no uuid) and a copy of it made outside the app.
    const juce::String old = R"({ "format": "flubsound-preset", "version": 1, "name": "Old", "category": "User", "params": { "boost": 0.42 } })";
    REQUIRE (writeText (folder.getChildFile ("Old.flubpreset.json"), old));
    PresetManager manager;
    manager.setUserPresetFolder (folder);
    const auto oldFile = folder.getChildFile ("Old.flubpreset.json");
    const auto* info = byFile (manager, oldFile);
    REQUIRE (info != nullptr);
    const auto uuid = uuidInFile (oldFile);
    CHECK (flub::preset::isValidUuid (uuid));
    CHECK (info->id == juce::String (uuid));
    CHECK (info->legacyId == "user:Old.flubpreset.json");
    CHECK (manager.findById ("user:Old.flubpreset.json") == info);
    const auto root = readJson (oldFile);
    CHECK (root["version"].asNumber() == 1.0);                 // otherwise as it was
    CHECK (root.asObject()[2].first == "uuid");                // after "version"
    CHECK (root["params"]["boost"].asNumber() == 0.42);

    manager.refresh(); // once: the same uuid on the next scan
    CHECK (uuidInFile (oldFile) == uuid);
    REQUIRE (oldFile.copyFileTo (folder.getChildFile ("Old copy.flubpreset.json")));
    manager.refresh();
    CHECK (manager.getUserPresets().size() == 2);
    CHECK (uuidInFile (oldFile) == uuid); // the file that had it keeps it (not the first by name)
    CHECK (uuidInFile (folder.getChildFile ("Old copy.flubpreset.json")) != uuid);

    // Save As: a new preset, a new uuid, every sound parameter written.
    ParameterStore store;
    store.set (BoostIntensity, 0.31f);
    juce::String error;
    const auto id = manager.saveUserPreset ("Fresh", "User", "", store, error, false);
    REQUIRE (id.isNotEmpty());
    const auto freshFile = folder.getChildFile ("Fresh.flubpreset.json");
    const auto fresh = readJson (freshFile);
    CHECK (id == juce::String (fresh["uuid"].asString()));
    CHECK (id != juce::String (uuid));
    size_t soundParams = 0;
    for (int p = 0; p < kNumParams; ++p)
        soundParams += flub::preset::isAppState (p) ? 0 : 1;
    CHECK (fresh["params"].asObject().size() == soundParams); // full state (app state stays out)
    CHECK (fresh["params"]["latency.profile"].isNull());

    // Overwrite (saveCurrent and Save As over the same name): the same uuid.
    manager.setCurrentPresetId (1, id, &store);
    store.set (BoostIntensity, 0.52f);
    REQUIRE (manager.saveCurrent (1, store, error));
    CHECK (juce::String (uuidInFile (freshFile)) == id);
    CHECK (manager.saveUserPreset ("Fresh", "User", "", store, error, true) == id);
    CHECK (manager.getCurrentPresetId (1) == id);

    // Rename: the file and name change, the id does not.
    const auto* freshInfo = manager.findById (id);
    REQUIRE (freshInfo != nullptr);
    CHECK (manager.renameUserPreset (*freshInfo, "Renamed", error) == id);
    CHECK (! freshFile.exists());
    const auto renamedFile = folder.getChildFile ("Renamed.flubpreset.json");
    REQUIRE (renamedFile.existsAsFile());
    CHECK (manager.findById (id)->name == "Renamed");
    CHECK (manager.findById (id)->file == renamedFile);
    CHECK (manager.getCurrentPresetId (1) == id);
    CHECK (readJson (renamedFile)["params"]["boost"].asNumber() == static_cast<double> (0.52f));
    CHECK (manager.renameUserPreset (*manager.findById (id), "Old", error).isEmpty()); // taken
    CHECK (error.isNotEmpty());
    CHECK (manager.renameUserPreset (*manager.findById (kFpsUuid), "Mine", error).isEmpty()); // factory
}

TEST_CASE ("App presets: import keeps a unique uuid, replaces a taken one, and reports reader warnings on import and load (E52)")
{
    const flubapptest::TempFolder temp;
    const auto folder = temp.file ("Presets");
    PresetManager manager;
    manager.setUserPresetFolder (folder);
    WarningLog log;
    log.attach (manager);
    juce::String error;

    // A factory preset exported and imported again: its uuid is taken.
    const auto exported = temp.file ("fps-export.flubpreset.json");
    REQUIRE (manager.exportPreset (*manager.findById (kFpsUuid), exported, error));
    CHECK (uuidInFile (exported) == kFpsUuid);
    const auto importedId = manager.importPresetFile (exported, error);
    REQUIRE (importedId.isNotEmpty());
    CHECK (importedId != kFpsUuid);
    CHECK (flub::preset::isValidUuid (importedId.toStdString()));
    CHECK (manager.findById (kFpsUuid)->isFactory); // unchanged

    // A unique uuid is kept; the typo and the clamp are reported.
    const auto shared = temp.file ("shared.flubpreset.json");
    REQUIRE (writeText (shared, R"({ "format": "flubsound-preset", "version": 2, "uuid": "0F6F5D2E-1C1B-4B6E-9D1E-2A3B4C5D6E7F",
                                     "name": "Shared", "params": { "bost": 0.5, "boost": 7 } })"));
    const auto sharedId = manager.importPresetFile (shared, error);
    CHECK (sharedId == "0f6f5d2e-1c1b-4b6e-9d1e-2a3b4c5d6e7f");
    REQUIRE (log.presets.size() == 1);
    CHECK (log.presets[0] == "Shared");
    CHECK (log.warnings.size() == 2);
    CHECK (log.warnings.joinIntoString ("\n").contains ("\"bost\""));
    CHECK (log.warnings.joinIntoString ("\n").contains ("boost"));

    // ... and on every load into a strip (the preset still loads).
    ParameterStore store;
    const auto* info = manager.findById (sharedId);
    REQUIRE (info != nullptr);
    REQUIRE (manager.loadIntoStrip (0, *info, store, error));
    CHECK (store.get (BoostIntensity) == 1.0f); // clamped
    CHECK (log.presets.size() == 2);
    CHECK (log.warnings.size() == 4);

    // A clean preset reports nothing.
    REQUIRE (manager.loadIntoStrip (0, *manager.findById (kFpsUuid), store, error));
    CHECK (log.presets.size() == 2);
}
