#include "PresetManager.h"

#include "flub/io/Json.h"

#if FLUB_HAS_FACTORY_PRESETS
    #include <FlubsoundPresetData.h>
#endif

#include <algorithm>
#include <cmath>
#include <iterator>

namespace flub::app
{
using namespace flub::param;

namespace
{
constexpr const char* kFactoryPrefix = "factory:";
constexpr const char* kUserPrefix = "user:";
constexpr const char* kUserExtension = ".flubpreset.json";

bool lessByCategoryThenName (const PresetInfo& a, const PresetInfo& b)
{
    const int c = a.category.compareIgnoreCase (b.category);
    if (c != 0)
        return c < 0;
    return a.name.compareNatural (b.name) < 0;
}
} // namespace

PresetManager::PresetManager()
    : userFolder (getDefaultUserPresetFolder())
{
    refresh();
}

juce::File PresetManager::getDefaultUserPresetFolder()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("Flubsound").getChildFile ("Presets");
}

void PresetManager::setUserPresetFolder (const juce::File& folder)
{
    userFolder = folder;
    refresh();
}

void PresetManager::refresh()
{
    presets.clear();
    scanFactory();
    scanUser();
    sortAndPublish();
}

// =============================================================================
// Scanning / parsing
// =============================================================================
bool PresetManager::parseJsonText (const std::string& text, flub::preset::Preset& out, juce::String& error)
{
    flub::json::Value root;
    std::string err;
    if (! flub::json::parse (text, root, err))
    {
        error = "Invalid JSON: " + juce::String (err);
        return false;
    }
    if (! flub::preset::fromJson (root, out, err))
    {
        error = juce::String (err);
        return false;
    }
    return true;
}

PresetInfo PresetManager::describe (const flub::preset::Preset& p)
{
    PresetInfo info;
    info.name = juce::String (p.name).trim();
    info.category = juce::String (p.category).trim();
    info.author = juce::String (p.author);
    info.description = juce::String (p.description);
    for (const auto& t : p.tags)
        info.tags.add (juce::String (t));

    if (info.category.isEmpty())
        info.category = "General";

    const auto& modeInfo = layout()[static_cast<size_t> (Mode)];
    if (static_cast<size_t> (Mode) < p.values.size())
    {
        const auto idx = static_cast<size_t> (std::clamp (static_cast<int> (std::lround (p.values[static_cast<size_t> (Mode)])), 0,
                                                          static_cast<int> (modeInfo.choices.size()) - 1));
        info.mode = juce::String (modeInfo.choices[idx]);
    }
    return info;
}

void PresetManager::scanFactory()
{
#if FLUB_HAS_FACTORY_PRESETS
    for (int i = 0; i < FlubsoundPresetData::namedResourceListSize; ++i)
    {
        const char* resource = FlubsoundPresetData::namedResourceList[i];
        int size = 0;
        const char* data = FlubsoundPresetData::getNamedResource (resource, size);
        if (data == nullptr || size <= 0)
            continue;

        flub::preset::Preset p;
        juce::String error;
        if (! parseJsonText (std::string (data, static_cast<size_t> (size)), p, error))
        {
            DBG ("Flubsound: factory preset " << resource << " is invalid: " << error);
            continue;
        }

        auto info = describe (p);
        const juce::String original = FlubsoundPresetData::getNamedResourceOriginalFilename (resource);
        const juce::String stem = original.isNotEmpty() ? original.upToFirstOccurrenceOf (".", false, false) : juce::String (resource);
        info.id = kFactoryPrefix + stem;
        info.isFactory = true;
        info.resourceName = resource;
        if (info.name.isEmpty())
            info.name = stem;
        presets.push_back (std::move (info));
    }
#endif
}

void PresetManager::scanUser()
{
    if (! userFolder.isDirectory())
        return;

    for (const auto& file : userFolder.findChildFiles (juce::File::findFiles, false, "*.json"))
    {
        flub::preset::Preset p;
        juce::String error;
        if (! parseJsonText (file.loadFileAsString().toStdString(), p, error))
            continue; // not a preset (or damaged): ignore

        auto info = describe (p);
        info.id = kUserPrefix + file.getFileName();
        info.isFactory = false;
        info.file = file;
        if (info.name.isEmpty())
            info.name = file.getFileNameWithoutExtension().upToFirstOccurrenceOf (".", false, false);
        presets.push_back (std::move (info));
    }
}

void PresetManager::sortAndPublish()
{
    std::stable_sort (presets.begin(), presets.end(),
                      [] (const PresetInfo& a, const PresetInfo& b)
                      {
                          if (a.isFactory != b.isFactory)
                              return a.isFactory;
                          return lessByCategoryThenName (a, b);
                      });

    if (onPresetListChanged != nullptr)
        onPresetListChanged();
}

std::vector<PresetInfo> PresetManager::getFactoryPresets() const
{
    std::vector<PresetInfo> result;
    std::copy_if (presets.begin(), presets.end(), std::back_inserter (result), [] (const PresetInfo& p) { return p.isFactory; });
    return result;
}

std::vector<PresetInfo> PresetManager::getUserPresets() const
{
    std::vector<PresetInfo> result;
    std::copy_if (presets.begin(), presets.end(), std::back_inserter (result), [] (const PresetInfo& p) { return ! p.isFactory; });
    return result;
}

juce::StringArray PresetManager::getCategories() const
{
    juce::StringArray categories;
    for (const auto& p : presets)
        categories.addIfNotAlreadyThere (p.category);
    return categories;
}

const PresetInfo* PresetManager::findById (const juce::String& id) const
{
    for (const auto& p : presets)
        if (p.id == id)
            return &p;
    return nullptr;
}

const PresetInfo* PresetManager::findByName (const juce::String& name) const
{
    for (const auto& p : presets)
        if (p.name.equalsIgnoreCase (name))
            return &p;
    return nullptr;
}

bool PresetManager::readPreset (const PresetInfo& info, flub::preset::Preset& out, juce::String& error) const
{
    if (info.isFactory)
    {
#if FLUB_HAS_FACTORY_PRESETS
        int size = 0;
        const char* data = FlubsoundPresetData::getNamedResource (info.resourceName.toRawUTF8(), size);
        if (data == nullptr || size <= 0)
        {
            error = "Factory preset not found";
            return false;
        }
        return parseJsonText (std::string (data, static_cast<size_t> (size)), out, error);
#else
        error = "This build contains no factory presets";
        return false;
#endif
    }

    if (! info.file.existsAsFile())
    {
        error = "Preset file not found: " + info.file.getFullPathName();
        return false;
    }
    return parseJsonText (info.file.loadFileAsString().toStdString(), out, error);
}

// =============================================================================
// Strip glue
// =============================================================================
bool PresetManager::loadIntoBank (const PresetInfo& info, ParameterStore& store, Bank bank, juce::String& error) const
{
    flub::preset::Preset p;
    if (! readPreset (info, p, error))
        return false;

    // Bypass is master-enable state owned by the app, never by a preset.
    const float bypass = store.get (bank, BypassAll);
    flub::preset::applyToStore (p, store, bank);
    store.set (bank, BypassAll, bypass);
    return true;
}

bool PresetManager::loadIntoStrip (int strip, const PresetInfo& info, ParameterStore& store, juce::String& error)
{
    if (! loadIntoBank (info, store, store.getActiveBank(), error))
        return false;
    setCurrentPresetId (strip, info.id, &store);
    return true;
}

bool PresetManager::stepPreset (int strip, int direction, ParameterStore& store, juce::String& error)
{
    if (presets.empty())
    {
        error = "No presets available";
        return false;
    }

    const auto current = getCurrentPresetId (strip);
    const auto n = static_cast<int> (presets.size());
    int index = -1;
    for (int i = 0; i < n; ++i)
        if (presets[static_cast<size_t> (i)].id == current)
            index = i;

    int next = 0;
    if (index < 0)
        next = direction >= 0 ? 0 : n - 1;
    else
        next = ((index + (direction >= 0 ? 1 : -1)) % n + n) % n;

    const auto info = presets[static_cast<size_t> (next)]; // copy: loading may not invalidate, but be safe
    return loadIntoStrip (strip, info, store, error);
}

juce::String PresetManager::getCurrentPresetId (int strip) const
{
    return strip >= 0 && strip < kMaxStrips ? currentIds[static_cast<size_t> (strip)] : juce::String();
}

void PresetManager::setCurrentPresetId (int strip, const juce::String& id, const ParameterStore* store)
{
    if (strip < 0 || strip >= kMaxStrips)
        return;
    currentIds[static_cast<size_t> (strip)] = id;
    if (store != nullptr)
        takeSnapshot (strip, *store);
}

void PresetManager::takeSnapshot (int strip, const ParameterStore& store)
{
    auto& snap = snapshots[static_cast<size_t> (strip)];
    snap.values.resize (static_cast<size_t> (kNumParams));
    store.snapshot (snap.values.data());
    snap.checkedStore = &store;
    snap.checkedVersion = store.version();
    snap.modified = false;
}

bool PresetManager::isPresetSound (int paramId) noexcept
{
    return paramId != BypassAll && paramId != LatencyProfile && paramId != LoudnessMatchBypass;
}

bool PresetManager::isModified (int strip, const ParameterStore& store) const
{
    if (strip < 0 || strip >= kMaxStrips || currentIds[static_cast<size_t> (strip)].isEmpty())
        return false;

    // Polled by UI timers: compare only when the store changed since the last call.
    const auto& snap = snapshots[static_cast<size_t> (strip)];
    const auto version = store.version();
    if (&store == snap.checkedStore && version == snap.checkedVersion)
        return snap.modified;

    bool modified = snap.values.empty();
    for (int i = 0; i < kNumParams && ! modified; ++i)
        modified = isPresetSound (i) && store.get (i) != snap.values[static_cast<size_t> (i)];
    snap.checkedStore = &store;
    snap.checkedVersion = version;
    snap.modified = modified;
    return modified;
}

// =============================================================================
// Saving
// =============================================================================
juce::String PresetManager::sanitiseFileName (const juce::String& name)
{
    auto cleaned = juce::File::createLegalFileName (name.trim()).trim();
    return cleaned.isEmpty() ? juce::String ("Preset") : cleaned;
}

juce::String PresetManager::saveUserPreset (const juce::String& name, const juce::String& category, const juce::String& description,
                                            const ParameterStore& store, juce::String& error, bool overwriteExisting)
{
    if (name.trim().isEmpty())
    {
        error = "Please enter a preset name";
        return {};
    }

    if (! userFolder.isDirectory())
    {
        const auto result = userFolder.createDirectory();
        if (result.failed())
        {
            error = "Cannot create the preset folder: " + result.getErrorMessage();
            return {};
        }
    }

    auto p = flub::preset::captureFromStore (store, store.getActiveBank());
    p.name = name.trim().toStdString();
    p.category = (category.trim().isNotEmpty() ? category.trim() : juce::String ("User")).toStdString();
    p.author = "User";
    p.description = description.toStdString();
    p.values[static_cast<size_t> (BypassAll)] = layout()[static_cast<size_t> (BypassAll)].defaultValue;

    const auto file = userFolder.getChildFile (sanitiseFileName (name) + kUserExtension);
    if (file.existsAsFile() && ! overwriteExisting)
    {
        error = "A preset with this name already exists";
        return {};
    }

    const auto text = flub::json::write (flub::preset::toJson (p, false), 2);
    if (! file.replaceWithText (juce::String::fromUTF8 (text.c_str()), false, false, "\n"))
    {
        error = "Cannot write " + file.getFullPathName();
        return {};
    }

    refresh();
    return kUserPrefix + file.getFileName();
}

bool PresetManager::saveCurrent (int strip, const ParameterStore& store, juce::String& error)
{
    const auto* info = findById (getCurrentPresetId (strip));
    if (info == nullptr || info->isFactory)
    {
        error = "Factory presets cannot be overwritten - save as a new preset";
        return false;
    }

    const auto copy = *info;
    if (saveUserPreset (copy.name, copy.category, copy.description, store, error, true).isEmpty())
        return false;
    if (strip >= 0 && strip < kMaxStrips)
        takeSnapshot (strip, store);
    return true;
}

bool PresetManager::deleteUserPreset (const PresetInfo& info, juce::String& error)
{
    if (info.isFactory)
    {
        error = "Factory presets cannot be deleted";
        return false;
    }
    if (! info.file.moveToTrash() && ! info.file.deleteFile())
    {
        error = "Cannot delete " + info.file.getFullPathName();
        return false;
    }
    for (auto& id : currentIds)
        if (id == info.id)
            id.clear();
    refresh();
    return true;
}

juce::String PresetManager::importPresetFile (const juce::File& source, juce::String& error)
{
    flub::preset::Preset p;
    if (! parseJsonText (source.loadFileAsString().toStdString(), p, error))
        return {};

    if (! userFolder.isDirectory() && userFolder.createDirectory().failed())
    {
        error = "Cannot create the preset folder";
        return {};
    }

    const auto stem = sanitiseFileName (p.name.empty() ? source.getFileNameWithoutExtension() : juce::String (p.name));
    const auto target = userFolder.getNonexistentChildFile (stem, kUserExtension, false);
    if (! source.copyFileTo (target))
    {
        error = "Cannot copy the preset into " + userFolder.getFullPathName();
        return {};
    }
    refresh();
    return kUserPrefix + target.getFileName();
}

bool PresetManager::exportPreset (const PresetInfo& info, const juce::File& destination, juce::String& error) const
{
    flub::preset::Preset p;
    if (! readPreset (info, p, error))
        return false;
    const auto text = flub::json::write (flub::preset::toJson (p, false), 2);
    if (! destination.replaceWithText (juce::String::fromUTF8 (text.c_str()), false, false, "\n"))
    {
        error = "Cannot write " + destination.getFullPathName();
        return false;
    }
    return true;
}

// =============================================================================
// A/B
// =============================================================================
void PresetManager::copyAToB (ParameterStore& store) noexcept { store.copyBank (Bank::A, Bank::B); }
void PresetManager::copyBToA (ParameterStore& store) noexcept { store.copyBank (Bank::B, Bank::A); }

void PresetManager::copyActiveToOther (ParameterStore& store) noexcept
{
    const auto active = store.getActiveBank();
    store.copyBank (active, active == Bank::A ? Bank::B : Bank::A);
}

Bank PresetManager::toggleBank (ParameterStore& store) noexcept
{
    const auto next = store.getActiveBank() == Bank::A ? Bank::B : Bank::A;
    store.setActiveBank (next);
    return next;
}
} // namespace flub::app
