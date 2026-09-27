// PresetIO schema, migration, identity and warnings (docs/11 E52):
//   * "version" is major.minor: a newer minor loads with a warning, a newer
//     major is refused, an older major migrates in memory through the
//     registry (version 1 -> 2 fills the frozen version-1 defaults);
//   * every ignored or changed key / value is reported in Preset::warnings;
//   * uuid (stable identity) and contentHash (identity of the sound);
//   * saved plug-in state: a parameter the state does not carry takes its
//     default on load (resolveSavedState).
#include "TestFramework.h"

#include "CliOptions.h"

#include "flub/io/FilePath.h"
#include "flub/io/Json.h"
#include "flub/io/PresetIO.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

using namespace flub;
using namespace flub::param;

namespace
{
json::Value parseJson (const std::string& text)
{
    json::Value v;
    std::string error;
    REQUIRE (json::parse (text, v, error));
    return v;
}

bool loadText (const std::string& text, preset::Preset& p, std::string& error) { return preset::fromJson (parseJson (text), p, error); }

preset::Preset loadOk (const std::string& text)
{
    preset::Preset p;
    std::string error;
    REQUIRE (loadText (text, p, error));
    return p;
}

bool hasWarning (const preset::Preset& p, const std::string& text)
{
    return std::find (p.warnings.begin(), p.warnings.end(), text) != p.warnings.end();
}

float value (const preset::Preset& p, int id) { return p.values[static_cast<size_t> (id)]; }
} // namespace

TEST_CASE ("Preset schema: the typo key \"bost\" is reported with an exact warning and changes nothing")
{
    const auto p = loadOk (R"({ "format": "flubsound-preset", "version": 2, "params": { "bost": 0.1, "max.drive": 1000 } })");
    CHECK (value (p, BoostIntensity) == layout()[static_cast<size_t> (BoostIntensity)].defaultValue);
    CHECK (hasWarning (p, R"(unknown parameter "bost" ignored (did you mean "boost"?))"));
    CHECK (hasWarning (p, R"("max.drive" = 1000 is out of range [0, 24]: clamped to 24)"));
    CHECK (value (p, MaxDriveDb) == 24.0f);
    CHECK (p.warnings.size() == 2);

    // Nothing close: no suggestion. Bad labels and types are reported too.
    const auto q = loadOk (R"({ "format": "flubsound-preset", "params": { "zzzzzzzzzz": 1, "mode": "Gamer", "boost": "high" } })");
    CHECK (hasWarning (q, R"(unknown parameter "zzzzzzzzzz" ignored)"));
    CHECK (hasWarning (q, R"("mode": unknown choice "Gamer" ignored)"));
    CHECK (hasWarning (q, R"("boost": expected a number, ignored)"));

    // A clean file has no warnings.
    CHECK (loadOk (R"({ "format": "flubsound-preset", "version": 2, "params": { "boost": 0.4, "mode": "Gaming" } })").warnings.empty());
}

TEST_CASE ("Preset schema: the CLI reports preset warnings as \"warning: \" notes (stderr, survives --quiet, --json \"notes\")")
{
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / ("flub-preset-warnings-" + preset::makeUuid());
    fs::create_directories (dir);
    const fs::path file = dir / "typo.flubpreset.json";
    {
        std::ofstream f (file, std::ios::binary);
        f << R"({ "format": "flubsound-preset", "version": 2, "params": { "bost": 0.1 } })";
    }
    cli::RenderOptions options;
    options.presetSpec = io::pathToUtf8 (file);
    cli::ResolvedParameters resolved;
    std::string error;
    REQUIRE (cli::buildParameters (options, resolved, error));
    CHECK (std::find (resolved.notes.begin(), resolved.notes.end(),
                      R"(warning: preset: unknown parameter "bost" ignored (did you mean "boost"?))")
           != resolved.notes.end());
    CHECK (resolved.values[static_cast<size_t> (BoostIntensity)] == layout()[static_cast<size_t> (BoostIntensity)].defaultValue);
    std::error_code ec;
    fs::remove_all (dir, ec);

#ifdef FLUB_PRESET_DIR
    // A factory preset (migrated from version 1) has none.
    options.presetSpec = "Racing";
    options.presetDir = FLUB_PRESET_DIR;
    REQUIRE (cli::buildParameters (options, resolved, error));
    for (const auto& n : resolved.notes)
        CHECK (n.rfind ("warning: preset:", 0) != 0);
#endif
}

TEST_CASE ("Preset schema: 2.1 loads with a warning, 3.0 is refused, 1.1 migrates with a warning, bad versions are refused")
{
    for (const char* v : { "2.1", "\"2.1\"" })
    {
        const auto p = loadOk (std::string (R"({ "format": "flubsound-preset", "version": )") + v + R"(, "params": { "boost": 0.5 } })");
        CHECK (p.loadedVersion == (preset::SchemaVersion { 2, 1 }));
        CHECK (hasWarning (p, "preset schema 2.1 is newer than this build knows (2.0): settings added since are ignored"));
        CHECK (value (p, BoostIntensity) == 0.5f);
        CHECK (value (p, VirtLfeGainDb) == 6.0f); // a version-2 file: today's default
    }

    preset::Preset p;
    std::string error;
    for (const char* v : { "3", "3.0", "\"3.2\"" })
    {
        error.clear();
        CHECK (! loadText (std::string (R"({ "format": "flubsound-preset", "version": )") + v + " }", p, error));
        CHECK (error.find ("newer Flubsound version") != std::string::npos);
    }
    for (const char* v : { "-1", "0", "true", "\"two\"", "\"2.x\"", "[2]", "2.5e3x" })
    {
        const std::string text = std::string (R"({ "format": "flubsound-preset", "version": )") + v + " }";
        json::Value root;
        std::string parseError;
        if (! json::parse (text, root, parseError))
            continue; // not even JSON
        error.clear();
        CHECK (! preset::fromJson (root, p, error));
        CHECK (! error.empty());
    }

    const auto old = loadOk (R"({ "format": "flubsound-preset", "version": 1.1, "params": { "boost": 0.5 } })");
    CHECK (old.loadedVersion == (preset::SchemaVersion { 1, 1 }));
    CHECK (hasWarning (old, "preset schema 1.1 is newer than this build knows (1.0): settings added since are ignored"));
    CHECK (value (old, VirtLfeGainDb) == 0.0f); // migrated as version 1

    const auto none = loadOk (R"({ "format": "flubsound-preset", "params": {} })");
    CHECK (none.loadedVersion == (preset::SchemaVersion { 1, 0 }));
    CHECK (none.warnings.empty());
}

TEST_CASE ("Preset schema: version text round trips and toJson writes the current version as a number")
{
    preset::SchemaVersion v;
    CHECK (preset::parseSchemaVersion (json::Value(), v));
    CHECK (v == (preset::SchemaVersion { 1, 0 }));
    CHECK (preset::parseSchemaVersion (json::Value (2.0), v));
    CHECK (v == (preset::SchemaVersion { 2, 0 }));
    CHECK (preset::parseSchemaVersion (json::Value ("2.13"), v));
    CHECK (v == (preset::SchemaVersion { 2, 13 }));
    CHECK (preset::toString (v) == "2.13");
    CHECK (preset::toString ({ 2, 0 }) == "2");

    const auto j = preset::toJson (preset::makeDefault());
    REQUIRE (j["version"].isNumber()); // builds before major.minor read it as a number and refuse newer files
    CHECK (preset::parseSchemaVersion (j["version"], v));
    CHECK (v == preset::kSchemaVersion);
}

TEST_CASE ("Preset migration: the registry covers every older major, is pure and fills the frozen version-1 defaults")
{
    const auto& registry = preset::migrations();
    for (int major = 1; major < preset::kSchemaVersion.majorVersion; ++major)
        CHECK (std::count_if (registry.begin(), registry.end(), [major] (const preset::Migration& m) { return m.fromMajor == major; }) == 1);

    const auto v1 = parseJson (R"({ "format": "flubsound-preset", "version": 1, "name": "Old", "params": { "boost": 0.5 } })");
    const std::string before = json::write (v1);
    json::Value migrated;
    preset::SchemaVersion from;
    std::string error;
    REQUIRE (preset::migrate (v1, migrated, from, error));
    CHECK (json::write (v1) == before); // input untouched
    CHECK (from == (preset::SchemaVersion { 1, 0 }));
    CHECK (migrated["version"].asNumber() == 2.0);
    CHECK (migrated["params"]["boost"].asNumber() == 0.5);
    for (const auto& [key, frozen] : preset::frozenDefaults (1))
        CHECK (migrated["params"][key].asNumber (-999.0) == static_cast<double> (frozen));
    CHECK (preset::frozenDefaults (1).size() >= 1);
    CHECK (preset::frozenDefaults (preset::kSchemaVersion.majorVersion).empty());

    // A key the version-1 file does carry is kept.
    REQUIRE (preset::migrate (parseJson (R"({ "format": "flubsound-preset", "version": 1, "params": { "virt.lfe": -4 } })"), migrated, from, error));
    CHECK (migrated["params"]["virt.lfe"].asNumber() == -4.0);

    // The current major passes through unchanged.
    const auto v2 = parseJson (R"({ "format": "flubsound-preset", "version": 2, "params": { "boost": 0.5 } })");
    REQUIRE (preset::migrate (v2, migrated, from, error));
    CHECK (json::write (migrated) == json::write (v2));
}

TEST_CASE ("Preset migration: every factory preset (version 1) round trips v1 -> v2 -> v2 with identical values, uuid and contentHash")
{
#ifdef FLUB_PRESET_DIR
    namespace fs = std::filesystem;
    int count = 0;
    std::error_code ec;
    for (fs::directory_iterator it (FLUB_PRESET_DIR, ec), end; ! ec && it != end; it.increment (ec))
    {
        if (it->path().extension() != ".json")
            continue;
        preset::Preset v1;
        std::string error;
        REQUIRE (preset::load (it->path().string(), v1, error));
        CHECK (v1.loadedVersion == (preset::SchemaVersion { 1, 0 }));
        CHECK (v1.warnings.empty());

        for (const bool full : { false, true })
        {
            const auto saved = preset::toJson (v1, full);
            CHECK (saved["version"].asNumber() == 2.0);
            preset::Preset v2;
            REQUIRE (preset::fromJson (parseJson (json::write (saved)), v2, error));
            CHECK (v2.loadedVersion == preset::kSchemaVersion);
            CHECK (v2.warnings.empty());
            CHECK (v2.values == v1.values);
            CHECK (v2.uuid == v1.uuid);
            CHECK (v2.name == v1.name);
            CHECK (v2.suggestedLatencyProfile == v1.suggestedLatencyProfile);
            CHECK (v2.savedContentHash == preset::contentHash (v1));
            CHECK (preset::contentHash (v2) == preset::contentHash (v1));
        }
        ++count;
    }
    CHECK (count >= 20);
#endif
}

TEST_CASE ("Preset identity: uuids are RFC 4122 v4, unique, read case-insensitively and found by findByUuid")
{
    std::set<std::string> seen;
    for (int i = 0; i < 64; ++i)
    {
        const auto u = preset::makeUuid();
        CHECK (preset::isValidUuid (u));
        CHECK (u[14] == '4');
        CHECK (std::string ("89ab").find (u[19]) != std::string::npos);
        CHECK (seen.insert (u).second);
    }
    CHECK (! preset::isValidUuid (""));
    CHECK (! preset::isValidUuid ("6f1c2c1e3a0b4d519a436c8f0e2b7d10"));
    CHECK (! preset::isValidUuid ("6f1c2c1e-3a0b-4d51-9a43-6c8f0e2b7d1g"));
    CHECK (preset::isValidUuid ("6F1C2C1E-3A0B-4D51-9A43-6C8F0E2B7D10"));

    auto p = loadOk (R"({ "format": "flubsound-preset", "uuid": "6F1C2C1E-3A0B-4D51-9A43-6C8F0E2B7D10", "params": {} })");
    CHECK (p.uuid == "6f1c2c1e-3a0b-4d51-9a43-6c8f0e2b7d10");
    CHECK (preset::toJson (p)["uuid"].asString() == p.uuid);

    const auto bad = loadOk (R"({ "format": "flubsound-preset", "uuid": "not-a-uuid", "params": {} })");
    CHECK (bad.uuid.empty());
    CHECK (hasWarning (bad, R"(invalid "uuid" ignored)"));
    CHECK (preset::toJson (bad)["uuid"].isNull());

    std::vector<preset::Preset> library { bad, p };
    CHECK (preset::findByUuid (library, "6F1C2C1E-3A0B-4D51-9A43-6C8F0E2B7D10") == &library[1]);
    CHECK (preset::findByUuid (library, preset::makeUuid()) == nullptr);
    CHECK (preset::findByUuid (library, "") == nullptr);
}

TEST_CASE ("Preset identity: contentHash follows the sound values only")
{
    auto p = preset::makeDefault();
    const auto h = preset::contentHash (p);
    CHECK (h.size() == 16);
    CHECK (h.find_first_not_of ("0123456789abcdef") == std::string::npos);

    auto renamed = p;
    renamed.name = "Other";
    renamed.uuid = preset::makeUuid();
    renamed.values[static_cast<size_t> (LatencyProfile)] = 0.0f; // app state
    renamed.values[static_cast<size_t> (BypassAll)] = 1.0f;
    CHECK (preset::contentHash (renamed) == h);

    auto louder = p;
    louder.values[static_cast<size_t> (BoostIntensity)] = 0.25f;
    CHECK (preset::contentHash (louder) != h);

    auto negativeZero = p;
    negativeZero.values[static_cast<size_t> (BoostIntensity)] = -0.0f; // default 0: same sound
    CHECK (preset::contentHash (negativeZero) == h);
}

TEST_CASE ("Plug-in state recall: a parameter absent from saved state takes its default, not its previous value")
{
    // Older state: carries boost and max.drive only (as if everything else
    // was added later). resolveSavedState is what the plug-in's
    // setStateInformation applies to every parameter.
    std::vector<std::string> warnings;
    const auto values = preset::resolveSavedState ({ { "boost", 0.7f }, { "max.drive", 99.0f }, { "future.param", 1.0f }, { "mode", 1.0f } },
                                                   &warnings);
    REQUIRE (values.size() == static_cast<size_t> (kNumParams));
    CHECK (values[static_cast<size_t> (BoostIntensity)] == 0.7f);
    CHECK (values[static_cast<size_t> (MaxDriveDb)] == 24.0f);
    CHECK (values[static_cast<size_t> (Mode)] == 1.0f);
    const auto& t = layout();
    for (int id = 0; id < kNumParams; ++id)
        if (id != BoostIntensity && id != MaxDriveDb && id != Mode)
            CHECK (values[static_cast<size_t> (id)] == t[static_cast<size_t> (id)].defaultValue);
    CHECK (std::find (warnings.begin(), warnings.end(), R"(unknown parameter "future.param" ignored)") != warnings.end());
    CHECK (std::find (warnings.begin(), warnings.end(), R"("max.drive" = 99 is out of range [0, 24]: clamped to 24)") != warnings.end());
    CHECK (warnings.size() == 2);

    // Empty state: all defaults; no warnings.
    const auto empty = preset::resolveSavedState ({}, &warnings);
    CHECK (empty == preset::makeDefault().values);
    CHECK (warnings.empty());
}
