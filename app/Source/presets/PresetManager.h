// Flubsound Pro - preset library (factory + user) and preset <-> strip glue.
//
// * Factory presets are the presets/factory/*.json files embedded at build
//   time (juce_add_binary_data, target FlubsoundPresets). Their display name,
//   category, author and tags come from the JSON (flub::preset format).
// * User presets live in <userApplicationDataDirectory>/Flubsound/Presets as
//   *.flubpreset.json (same format) and can be saved, overwritten, deleted
//   and imported.
// * Presets are applied to a strip's ParameterStore bank with
//   flub::preset::applyToStore (lock-free atomic writes; the audio thread
//   glides continuous values and crossfades discrete ones).
// * "Bypass All" is application state (master enable), not preset state:
//   loading keeps the bank's current bypass value and saving never writes it.
//
// Preset ids are stable strings: "factory:<resource name>" / "user:<file name>".
// The list is ordered: factory presets (by category, then name), then user
// presets (by category, then name). Message thread only.
#pragma once

#include "flub/engine/MixEngine.h"
#include "flub/engine/Parameters.h"
#include "flub/io/PresetIO.h"

#include <juce_core/juce_core.h>

#include <array>
#include <functional>
#include <vector>

namespace flub::app
{
struct PresetInfo
{
    juce::String id;          // "factory:..." or "user:..."
    juce::String name, category, author, description;
    juce::StringArray tags;
    juce::String mode;        // "Music" / "Gaming" (the preset's mode parameter)
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
    const PresetInfo* findById (const juce::String& id) const;
    const PresetInfo* findByName (const juce::String& name) const; // first match, factory first

    /** Parses a preset (factory resource or user file). */
    bool readPreset (const PresetInfo& info, flub::preset::Preset& out, juce::String& error) const;

    // ---- Strip glue ----------------------------------------------------------------
    /** Loads a preset into a strip's ACTIVE bank and remembers it as the strip's
        current preset. */
    bool loadIntoStrip (int strip, const PresetInfo& info, flub::param::ParameterStore& store, juce::String& error);

    /** Loads into an explicit bank (does not change the strip's current preset). */
    bool loadIntoBank (const PresetInfo& info, flub::param::ParameterStore& store, flub::param::Bank bank, juce::String& error) const;

    /** next / previous preset relative to the strip's current one (wraps). */
    bool stepPreset (int strip, int direction, flub::param::ParameterStore& store, juce::String& error);

    juce::String getCurrentPresetId (int strip) const;
    void setCurrentPresetId (int strip, const juce::String& id);
    /** True if the strip's store changed since its preset was loaded. */
    bool isModified (int strip, const flub::param::ParameterStore& store) const;

    // ---- Saving -------------------------------------------------------------------------
    /** Saves the store's active bank as a user preset. Returns the new preset's
        id (empty on failure). */
    juce::String saveUserPreset (const juce::String& name, const juce::String& category, const juce::String& description,
                                 const flub::param::ParameterStore& store, juce::String& error, bool overwriteExisting = true);

    /** Overwrites the strip's current preset if it is a user preset. */
    bool saveCurrent (int strip, const flub::param::ParameterStore& store, juce::String& error);

    bool deleteUserPreset (const PresetInfo& info, juce::String& error);
    /** Validates and copies a preset file into the user folder. */
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

private:
    void scanFactory();
    void scanUser();
    void sortAndPublish();
    static bool parseJsonText (const std::string& text, flub::preset::Preset& out, juce::String& error);
    static PresetInfo describe (const flub::preset::Preset& p);
    static juce::String sanitiseFileName (const juce::String& name);

    juce::File userFolder;
    std::vector<PresetInfo> presets;
    std::array<juce::String, kMaxStrips> currentIds;
    std::array<uint32_t, kMaxStrips> versionAtLoad {};
};
} // namespace flub::app
