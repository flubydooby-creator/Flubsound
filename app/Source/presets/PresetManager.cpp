#include "PresetManager.h"

#include "settings/UserDataFolder.h"

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

/** `root` with "uuid" set: replaced in place, or inserted after "version"
    (after "format" if there is none, else first), every other member kept. */
flub::json::Value withUuid (const flub::json::Value& root, const std::string& uuid)
{
    flub::json::Value::Object members = root.asObject();
    const auto has = [&members] (const char* key)
    { return std::find_if (members.begin(), members.end(), [key] (const auto& m) { return m.first == key; }); };
    if (const auto it = has ("uuid"); it != members.end())
    {
        it->second = uuid;
        return flub::json::Value (std::move (members));
    }
    auto at = has ("version");
    if (at == members.end())
        at = has ("format");
    const auto pos = at == members.end() ? members.begin() : at + 1;
    members.insert (pos, { "uuid", flub::json::Value (uuid) });
    return flub::json::Value (std::move (members));
}

bool writeJsonFile (const juce::File& file, const flub::json::Value& root)
{
    const auto text = flub::json::write (root, 2);
    return file.replaceWithText (juce::String::fromUTF8 (text.c_str()), false, false, "\n");
}

bool readJsonFile (const juce::File& file, flub::json::Value& root)
{
    std::string err;
    return file.existsAsFile() && flub::json::parse (file.loadFileAsString().toStdString(), root, err) && root.isObject();
}
} // namespace

PresetManager::PresetManager()
    : userFolder (getDefaultUserPresetFolder())
{
    refresh();
}

juce::File PresetManager::getDefaultUserPresetFolder()
{
    return userDataFolder().getChildFile ("Presets");
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
    info.suggestedLatencyProfile = p.suggestedLatencyProfile;
    info.uuid = juce::String (p.uuid); // fromJson: valid and lower case, or empty

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
        info.legacyId = kFactoryPrefix + stem;
        info.id = info.uuid.isNotEmpty() ? info.uuid : info.legacyId;
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

    // By file name, so which of two files with the same uuid (a copy made
    // outside the app) keeps it does not depend on the directory order.
    auto files = userFolder.findChildFiles (juce::File::findFiles, false, "*.json");
    std::sort (files.begin(), files.end(), [] (const juce::File& a, const juce::File& b) { return a.getFileName() < b.getFileName(); });
    for (const auto& file : files)
    {
        flub::preset::Preset p;
        juce::String error;
        if (! parseJsonText (file.loadFileAsString().toStdString(), p, error))
            continue; // not a preset (or damaged, or a newer major): ignore, never rewritten

        auto info = describe (p);
        // A preset without a uuid (saved before docs/11 E52, or by hand), or
        // with one another preset has, gets a new one, written into its file
        // once so that it survives renames from here on.
        if (info.uuid.isEmpty() || isUuidTaken (p.uuid))
        {
            const auto uuid = flub::preset::makeUuid();
            info.uuid = writeUuid (file, uuid) ? juce::String (uuid) : juce::String();
        }
        info.legacyId = kUserPrefix + file.getFileName();
        info.id = info.uuid.isNotEmpty() ? info.uuid : info.legacyId;
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
    if (id.isEmpty())
        return nullptr;
    for (const auto& p : presets)
        if (p.id == id)
            return &p;
    for (const auto& p : presets)
        if (p.legacyId == id)
            return &p;
    if (flub::preset::isValidUuid (id.toStdString()))
        for (const auto& p : presets)
            if (p.uuid.equalsIgnoreCase (id))
                return &p;
    return nullptr;
}

std::map<juce::String, juce::String> PresetManager::getLegacyIdAliases() const
{
    std::map<juce::String, juce::String> aliases;
    for (const auto& p : presets)
        if (p.legacyId.isNotEmpty() && p.id != p.legacyId)
            aliases[p.legacyId] = p.id;
    return aliases;
}

bool PresetManager::isUuidTaken (const std::string& uuid, const juce::File& except) const
{
    const auto wanted = juce::String (uuid);
    return std::any_of (presets.begin(), presets.end(),
                        [&] (const PresetInfo& p) { return p.uuid.isNotEmpty() && p.uuid.equalsIgnoreCase (wanted) && (except == juce::File() || p.file != except); });
}

bool PresetManager::writeUuid (const juce::File& file, const std::string& uuid)
{
    flub::json::Value root;
    return readJsonFile (file, root) && writeJsonFile (file, withUuid (root, uuid));
}

void PresetManager::reportWarnings (const PresetInfo& info, const flub::preset::Preset& p) const
{
    if (p.warnings.empty() || onPresetWarnings == nullptr)
        return;
    juce::StringArray warnings;
    for (const auto& w : p.warnings)
        warnings.add (juce::String::fromUTF8 (w.c_str()));
    onPresetWarnings (info, warnings);
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

    // App state (Bypass All = master enable, loudness-matched bypass, the
    // latency profile) keeps the bank's value: a preset never re-prepares the
    // engine or, through the MixEngine padding, delays the other strips
    // (docs/11 E40). A profile the file carries is info.suggestedLatencyProfile.
    flub::preset::applyPresetToStore (p, store, bank);
    reportWarnings (info, p);
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
    const auto* info = findById (id);
    currentIds[static_cast<size_t> (strip)] = info != nullptr ? info->id : id;
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

    // App state is not written into "params" (it would never be applied: see
    // loadIntoBank); the profile the preset was made in becomes its
    // "suggestedLatencyProfile" label (docs/11 E40, E42a).
    const auto profile = static_cast<int> (std::lround (store.get (store.getActiveBank(), LatencyProfile)));
    p.suggestedLatencyProfile = static_cast<LatencyProfileValue> (
        std::clamp (profile, static_cast<int> (LatencyProfileValue::Quality), static_cast<int> (LatencyProfileValue::LowLatency)));
    for (int id = 0; id < kNumParams; ++id)
        if (flub::preset::isAppState (id))
            p.unsetAppState.push_back (id);

    const auto file = userFolder.getChildFile (sanitiseFileName (name) + kUserExtension);
    if (file.existsAsFile() && ! overwriteExisting)
    {
        error = "A preset with this name already exists";
        return {};
    }

    // Overwriting keeps the preset's identity (rules that play it keep
    // working); a new file is a new preset.
    const auto existing = std::find_if (presets.begin(), presets.end(), [&file] (const PresetInfo& i) { return ! i.isFactory && i.file == file; });
    p.uuid = existing != presets.end() && existing->uuid.isNotEmpty() ? existing->uuid.toStdString() : flub::preset::makeUuid();

    // Full state: every sound parameter, so the preset does not depend on
    // today's defaults (app state stays out: p.unsetAppState).
    if (! writeJsonFile (file, flub::preset::toJson (p, true)))
    {
        error = "Cannot write " + file.getFullPathName();
        return {};
    }

    refresh();
    const auto* saved = findById (juce::String (p.uuid));
    return saved != nullptr ? saved->id : kUserPrefix + file.getFileName();
}

juce::String PresetManager::renameUserPreset (const PresetInfo& info, const juce::String& newName, juce::String& error)
{
    if (info.isFactory)
    {
        error = "Factory presets cannot be renamed";
        return {};
    }
    if (newName.trim().isEmpty())
    {
        error = "Please enter a preset name";
        return {};
    }

    flub::json::Value root;
    if (! readJsonFile (info.file, root))
    {
        error = "Cannot read " + info.file.getFullPathName();
        return {};
    }
    const auto target = userFolder.getChildFile (sanitiseFileName (newName) + kUserExtension);
    if (target.existsAsFile() && target != info.file)
    {
        error = "A preset with this name already exists";
        return {};
    }

    // The file as it is (version, params, uuid) with the new name; a file
    // without a uuid (one that could not be written before) gets one now.
    root.set ("name", newName.trim().toStdString());
    const auto uuid = info.uuid.isNotEmpty() ? info.uuid.toStdString() : flub::preset::makeUuid();
    if (! writeJsonFile (target, withUuid (root, uuid)))
    {
        error = "Cannot write " + target.getFullPathName();
        return {};
    }
    if (target != info.file)
        info.file.deleteFile();

    const auto oldId = info.id; // `info` may point into the list refresh() rebuilds
    refresh();
    const auto* renamed = findById (juce::String (uuid));
    const auto id = renamed != nullptr ? renamed->id : juce::String();
    for (auto& current : currentIds)
        if (current == oldId)
            current = id;
    return id;
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
    // (the same file, so the same uuid and id)
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
    // The copy is a preset of its own when its uuid is missing or taken (the
    // same preset exported and imported again): a new uuid, so the two never
    // share rules. Otherwise the file is kept byte for byte.
    if (p.uuid.empty() || isUuidTaken (p.uuid))
        writeUuid (target, flub::preset::makeUuid()); // failure: refresh() retries

    refresh();
    const auto imported = std::find_if (presets.begin(), presets.end(), [&target] (const PresetInfo& i) { return i.file == target; });
    if (imported == presets.end())
    {
        error = "The imported preset could not be read back from " + target.getFullPathName();
        return {};
    }
    reportWarnings (*imported, p);
    return imported->id;
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
