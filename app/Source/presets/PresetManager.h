// Flubsound Pro - preset library (factory + user) and preset <-> strip glue.
//
// * Factory presets are the presets/factory/*.json files embedded at build
//   time (juce_add_binary_data, target FlubsoundPresets). Their display name,
//   category, author and tags come from the JSON (flub::preset format).
// * User presets live in <userApplicationDataDirectory>/Flubsound/Presets as
//   *.flubpreset.json (same format) and can be saved, overwritten, renamed,
//   deleted and imported.
// * Presets are applied to a strip's ParameterStore bank with
//   flub::preset::applyToStore (lock-free atomic writes; the audio thread
//   glides continuous values and crossfades discrete ones).
// * App state that shares the parameter table - "Bypass All" (master
//   enable), loudness-matched bypass and the latency profile - is not preset
//   state (flub::preset::isAppState, docs/11 E40): loading keeps the bank's
//   current values (flub::preset::applyPresetToStore), so a preset never
//   re-prepares the engine or pads the other strips, and saving never writes
//   them; a saved user preset names the profile it was made in as its
//   "suggestedLatencyProfile" (PresetInfo::suggestedLatencyProfile).
// * "Modified" compares the active bank's sound values with a snapshot taken
//   when the preset was loaded / saved, so reverting an edit clears it. App
//   state that shares the store is ignored: Bypass All, the latency profile
//   and loudness-matched bypass (application-wide settings) and which A/B
//   bank is active (only the values heard count).
//
// Preset ids (docs/11 E52): a preset's id is its uuid (lower case), which
// survives renames, so automatic profile rules and a strip's last preset
// keep working when a user preset is renamed. Every factory preset has a
// permanent uuid; a user preset found without one, or with one another
// preset already has (a copied file), is given a new uuid, written into its
// file once. Only a preset whose file cannot be written keeps a legacy id.
// The legacy ids "factory:<resource name>" / "user:<file name>" (PresetInfo::
// legacyId) stay accepted by findById and setCurrentPresetId, and
// getLegacyIdAliases() maps them to uuids for the one-time settings
// migration (AppSettings::migratePresetReferences).
// * Saving: a new user preset gets a new uuid; overwriting one keeps its
//   uuid (saveCurrent, Save As over the same name); renameUserPreset keeps
//   it. User presets are written with every sound parameter (full state), so
//   a later change of a default cannot re-voice them.
// * Import keeps the file's uuid unless another preset has it (a preset
//   exported and imported again on the same machine), then assigns one.
// * Warnings (unknown keys, clamped values, a newer minor version:
//   flub::preset::Preset::warnings) of a preset loaded into a strip or
//   imported go to onPresetWarnings, for a toast.
// The list is ordered: factory presets (by category, then name), then user
// presets (by category, then name). Message thread only.
#pragma once

#include "flub/engine/MixEngine.h"
#include "flub/engine/Parameters.h"
#include "flub/io/PresetIO.h"

#include <juce_core/juce_core.h>

#include <array>
#include <functional>
#include <map>
#include <optional>
#include <vector>

namespace flub::app
{
struct PresetInfo
{
    juce::String id;          // the uuid; the legacy id for a preset without one
    juce::String legacyId;    // "factory:<resource stem>" / "user:<file name>" (ids before docs/11 E52)
    juce::String uuid;        // lower case; empty if the preset has none
    juce::String name, category, author, description;
    juce::StringArray tags;
    juce::String mode;        // "Music" / "Gaming" (the preset's mode parameter)
    /** The latency profile the preset was made for ("suggestedLatencyProfile",
        or a profile an older file carried in "params"): metadata for a
        suggestion, never applied on load (docs/11 E40 / E42a). */
    std::optional<flub::param::LatencyProfileValue> suggestedLatencyProfile;
    bool isFactory = false;
    juce::String resourceName; // factory: BinaryData resource
    juce::File file;           // user: preset file

    bool isValid() const noexcept { return id.isNotEmpty(); }
};

class PresetManager
{
public:
    static constexpr int kMaxStrips = flub::MixEngine::kMaxStrips;

    /** Scans factory presets and the default user folder. */
    PresetManager();

    /** Default user folder: <userApplicationDataDirectory>/Flubsound/Presets. */
    static juce::File getDefaultUserPresetFolder();
    void setUserPresetFolder (const juce::File& folder); // rescans
    juce::File getUserPresetFolder() const { return userFolder; }

    /** Rescans both sources (e.g. after files were added outside the app). */
    void refresh();

    const std::vector<PresetInfo>& getPresets() const noexcept { return presets; }
    std::vector<PresetInfo> getFactoryPresets() const;
    std::vector<PresetInfo> getUserPresets() const;
    juce::StringArray getCategories() const;
    /** By id, legacy id ("factory:..." / "user:...") or uuid (any case). */
    const PresetInfo* findById (const juce::String& id) const;
    /** legacy id -> id for every preset whose id is its uuid (the one-time
        settings migration, AppSettings::migratePresetReferences). */
    std::map<juce::String, juce::String> getLegacyIdAliases() const;
    const PresetInfo* findByName (const juce::String& name) const; // first match, factory first

    /** Parses a preset (factory resource or user file). */
    bool readPreset (const PresetInfo& info, flub::preset::Preset& out, juce::String& error) const;

    // ---- Strip glue ----------------------------------------------------------------
    /** Loads a preset into a strip's ACTIVE bank and remembers it as the strip's
        current preset. */
    bool loadIntoStrip (int strip, const PresetInfo& info, flub::param::ParameterStore& store, juce::String& error);

    /** Loads into an explicit bank (does not change the strip's current preset).
        Writes the preset's sound only: Bypass All, loudness-matched bypass and
        the latency profile keep the bank's values (see the file comment). */
    bool loadIntoBank (const PresetInfo& info, flub::param::ParameterStore& store, flub::param::Bank bank, juce::String& error) const;

    /** next / previous preset relative to the strip's current one (wraps). */
    bool stepPreset (int strip, int direction, flub::param::ParameterStore& store, juce::String& error);

    juce::String getCurrentPresetId (int strip) const;
    /** Marks `id` as the strip's current preset without loading it (e.g. when
        a saved strip state was restored). A legacy id or uuid of a known
        preset is stored as its id. With a store, the store's current state
        counts as unmodified. */
    void setCurrentPresetId (int strip, const juce::String& id, const flub::param::ParameterStore* store = nullptr);
    /** True if the sound values of the strip's active bank differ from the
        preset as loaded / saved. Cheap to poll: compares only after
        store.version() changed. */
    bool isModified (int strip, const flub::param::ParameterStore& store) const;

    // ---- Saving -------------------------------------------------------------------------
    /** Saves the store's active bank as a user preset, every sound parameter
        included. A new file gets a new uuid; overwriting a file keeps its
        preset's uuid. Returns the preset's id (empty on failure). */
    juce::String saveUserPreset (const juce::String& name, const juce::String& category, const juce::String& description,
                                 const flub::param::ParameterStore& store, juce::String& error, bool overwriteExisting = true);

    /** Overwrites the strip's current preset if it is a user preset. */
    bool saveCurrent (int strip, const flub::param::ParameterStore& store, juce::String& error);

    /** Renames a user preset (its name and file name); its uuid, and so its
        id, rules and strips that use it, stay. Returns the id (empty on failure). */
    juce::String renameUserPreset (const PresetInfo& info, const juce::String& newName, juce::String& error);

    bool deleteUserPreset (const PresetInfo& info, juce::String& error);
    /** Validates and copies a preset file into the user folder (a new uuid if
        it has none or another preset has it). Reports its warnings. */
    juce::String importPresetFile (const juce::File& file, juce::String& error);
    bool exportPreset (const PresetInfo& info, const juce::File& destination, juce::String& error) const;

    // ---- A/B helpers (active bank = what is heard) -----------------------------------------
    static void copyAToB (flub::param::ParameterStore& store) noexcept;
    static void copyBToA (flub::param::ParameterStore& store) noexcept;
    /** Copies the active bank into the inactive one. */
    static void copyActiveToOther (flub::param::ParameterStore& store) noexcept;
    /** Switches the active bank; returns the new active bank. */
    static flub::param::Bank toggleBank (flub::param::ParameterStore& store) noexcept;

    /** Called after the preset list changed (refresh, save, delete, import). */
    std::function<void()> onPresetListChanged;

    /** Called when a preset loaded into a strip or bank (loadIntoStrip,
        loadIntoBank, stepPreset) or imported was read with warnings (unknown
        keys ignored, values clamped, a newer minor version: one sentence
        each). The preset was still loaded. Meant for a toast, e.g.
        "Loaded My Preset with 1 warning: unknown parameter "bost" ignored
        (did you mean "boost"?)". Called on every load, so a caller that shows
        it should not repeat a toast for the same preset over and over. */
    std::function<void (const PresetInfo& preset, const juce::StringArray& warnings)> onPresetWarnings;

private:
    void scanFactory();
    void scanUser (const std::map<juce::String, juce::File>& previousOwners);
    void sortAndPublish();
    static bool parseJsonText (const std::string& text, flub::preset::Preset& out, juce::String& error);
    static PresetInfo describe (const flub::preset::Preset& p);
    static juce::String sanitiseFileName (const juce::String& name);
    /** Writes `uuid` into the preset file's JSON (every other member kept as
        it is, the file's version too). False if it cannot be read or written. */
    static bool writeUuid (const juce::File& file, const std::string& uuid);
    bool isUuidTaken (const std::string& uuid, const juce::File& except = {}) const;
    void reportWarnings (const PresetInfo& info, const flub::preset::Preset& p) const;

    void takeSnapshot (int strip, const flub::param::ParameterStore& store);
    /** False for parameters that live in the store but are application state,
        not part of a preset's sound (Bypass All, latency profile,
        loudness-matched bypass). */
    static bool isPresetSound (int paramId) noexcept;

    /** Active-bank values when the strip's preset was loaded / saved, plus the
        cached result of the last isModified() comparison. */
    struct Snapshot
    {
        std::vector<float> values; // kNumParams, empty until taken
        mutable const flub::param::ParameterStore* checkedStore = nullptr;
        mutable uint32_t checkedVersion = 0;
        mutable bool modified = false;
    };

    juce::File userFolder;
    std::vector<PresetInfo> presets;
    std::array<juce::String, kMaxStrips> currentIds;
    std::array<Snapshot, kMaxStrips> snapshots;
};
} // namespace flub::app
