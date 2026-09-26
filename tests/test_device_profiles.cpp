// Headset / output-device profiles (Turtle Beach families and connection types).
#include "TestFramework.h"

#include "flub/engine/DeviceProfiles.h"

#include <algorithm>

using namespace flub::device;

namespace
{
const Database& shippedDatabase()
{
    static Database db = [] {
        Database d;
        std::string err;
#ifdef FLUB_DEVICE_PROFILES
        const bool ok = d.loadFile (FLUB_DEVICE_PROFILES, err);
#else
        const bool ok = false;
#endif
        if (! ok)
            std::cerr << "device profile load failed: " << err << "\n";
        return d;
    }();
    return db;
}

std::string idOf (const Match& m) { return m.profile != nullptr ? m.profile->id : std::string ("(generic)"); }

bool hasMessageContaining (const Advice& a, const std::string& needle)
{
    return std::any_of (a.messages.begin(), a.messages.end(), [&] (const std::string& s) { return s.find (needle) != std::string::npos; });
}
} // namespace

TEST_CASE ("DeviceProfiles: shipped database loads and every entry is well formed")
{
    const auto& db = shippedDatabase();
    REQUIRE (db.profiles().size() >= 6);
    for (const auto& p : db.profiles())
    {
        CHECK (! p.id.empty());
        CHECK (! p.displayName.empty());
        CHECK (! p.matchAny.empty());
        CHECK (! p.gamingPreset.empty());
        CHECK (! p.musicPreset.empty());
        for (const auto& t : p.matchAny)
            CHECK (t == normalise (t)); // tokens are stored normalised
    }
}

TEST_CASE ("DeviceProfiles: Turtle Beach families are recognised from typical endpoint names")
{
    const auto& db = shippedDatabase();
    struct Case
    {
        const char* endpoint;
        const char* expectedId;
    };
    const Case cases[] = {
        { "Headphones (Stealth 700 Gen 2 MAX)", "turtle-beach-stealth" },
        { "Speakers (Turtle Beach Stealth Pro)", "turtle-beach-stealth" }, // family beats vendor-generic
        { "Headset Earphone (Stealth 600 Gen 3)", "turtle-beach-stealth" },
        { "Headphones (Recon 200 Gen 2)", "turtle-beach-recon" },
        { "Turtle Beach Elite Atlas Aero", "turtle-beach-atlas" },
        { "Speakers (Elite Pro 2 + SuperAmp)", "turtle-beach-elite-pro" },
        { "ROCCAT Syn Pro Air", "turtle-beach-roccat" },
        { "Speakers (Turtle Beach Ear Force Z60)", "turtle-beach-generic" },
        { "Headset Earphone (Xbox Wireless Headset)", "xbox-wireless-headset" },
    };
    for (const auto& c : cases)
    {
        const auto m = db.match (c.endpoint, 48000.0, 2);
        if (idOf (m) != c.expectedId)
            std::cerr << "    endpoint '" << c.endpoint << "' matched " << idOf (m) << "\n";
        CHECK (idOf (m) == c.expectedId);
    }
}

TEST_CASE ("DeviceProfiles: unrelated devices do not match (no false positives)")
{
    const auto& db = shippedDatabase();
    for (const char* name : { "Speakers (Realtek(R) Audio)", "Razer Blade Stealth Speakers", "Headphones (WH-1000XM5)",
                              "MacBook Pro Speakers", "Built-in Audio Analog Stereo", "Stealthy Mic", "Reconnect Audio" })
        CHECK (idOf (db.match (name, 48000.0, 2)) == "(generic)");
}

TEST_CASE ("DeviceProfiles: connection detection (platform hint, hands-free names, narrowband formats)")
{
    CHECK (detectConnection ("Headset (Stealth 600 Gen 3 Hands-Free AG Audio)", 16000.0, 1) == Connection::BluetoothHandsFree);
    CHECK (detectConnection ("Whatever", 16000.0, 1) == Connection::BluetoothHandsFree);
    CHECK (detectConnection ("Whatever", 48000.0, 2, Connection::Bluetooth) == Connection::Bluetooth);
    CHECK (detectConnection ("Whatever", 16000.0, 1, Connection::Bluetooth) == Connection::BluetoothHandsFree);
    CHECK (detectConnection ("Speakers (USB Audio Device)", 48000.0, 2) == Connection::Usb);
    CHECK (detectConnection ("Speakers (Realtek(R) Audio)", 48000.0, 2) == Connection::Analog);
    CHECK (detectConnection ("Headphones (Stealth 700 Gen 2 MAX)", 48000.0, 2) == Connection::Unknown);
    // Unknown connection falls back to the matched family's typical connection.
    CHECK (shippedDatabase().match ("Headphones (Stealth 700 Gen 2 MAX)", 48000.0, 2).connection == Connection::Usb);
    CHECK (shippedDatabase().match ("Headphones (Recon 50)", 48000.0, 2).connection == Connection::Analog);
}

TEST_CASE ("DeviceProfiles: advice caps the ceiling per connection and warns about stacked headset DSP")
{
    const auto& db = shippedDatabase();

    const auto usb = adviceFor (db.match ("Headphones (Stealth 700 Gen 2 MAX)", 48000.0, 2), 48000.0, true);
    CHECK (usb.ceilingDbTp == -1.0f);
    CHECK (! usb.narrowband);
    CHECK (usb.suggestedPreset == "Competitive FPS");
    CHECK (hasMessageContaining (usb, "Superhuman Hearing"));
    CHECK (hasMessageContaining (usb, "virtual surround"));
    CHECK (hasMessageContaining (usb, "'Game' and 'Chat'"));

    const auto bt = adviceFor (db.match ("Headphones (Stealth 600 Gen 3)", 48000.0, 2, Connection::Bluetooth), 48000.0, false);
    CHECK (bt.ceilingDbTp == -2.0f);
    CHECK (bt.suggestedPreset == "Bluetooth Headphones");

    const auto hfp = adviceFor (db.match ("Headset (Stealth 600 Gen 3 Hands-Free AG Audio)", 16000.0, 1), 16000.0, true);
    CHECK (hfp.ceilingDbTp == -3.0f);
    CHECK (hfp.narrowband);
    CHECK (hasMessageContaining (hfp, "hands-free"));

    const auto generic = adviceFor (db.match ("Speakers (Realtek(R) Audio)", 44100.0, 2), 44100.0, false);
    CHECK (generic.ceilingDbTp == -1.0f);
    CHECK (generic.messages.empty());
    CHECK (generic.suggestedPreset.empty());
}

TEST_CASE ("DeviceProfiles: malformed files are rejected with an error")
{
    Database db;
    std::string err;
    flub::json::Value v;
    REQUIRE (flub::json::parse (R"({"format":"something-else","profiles":[]})", v, err));
    CHECK (! db.load (v, err));
    REQUIRE (flub::json::parse (R"({"format":"flubsound-device-profiles","version":1,"profiles":[{"id":"x"}]})", v, err));
    CHECK (! db.load (v, err));
    CHECK (! db.loadFile ("/nonexistent/profiles.json", err));
}

TEST_CASE ("DeviceProfiles: the embedded database is identical to presets/devices/device-profiles.json")
{
    // Regenerate with: python3 tools/scripts/embed-device-profiles.py
    Database builtIn;
    std::string err;
    REQUIRE (builtIn.loadBuiltIn (err));
    const auto& shipped = shippedDatabase();
    REQUIRE (builtIn.profiles().size() == shipped.profiles().size());
    for (size_t i = 0; i < builtIn.profiles().size(); ++i)
    {
        const auto& a = builtIn.profiles()[i];
        const auto& b = shipped.profiles()[i];
        CHECK (a.id == b.id);
        CHECK (a.matchAny == b.matchAny);
        CHECK (a.exclude == b.exclude);
        CHECK (a.notes == b.notes);
        CHECK (a.specificity == b.specificity);
        CHECK (a.gamingPreset == b.gamingPreset);
    }
}
