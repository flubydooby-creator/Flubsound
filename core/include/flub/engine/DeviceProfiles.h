// Flubsound Pro - output-device (headset) profiles.
//
// Compatibility with a headset never depends on this file. Flubsound
// processes audio before the operating system hands it to ANY standard
// output device (analog jack, USB Audio Class device, 2.4 GHz USB dongle,
// Bluetooth A2DP/HFP). What profiles add is device-aware *advice and safety*:
//
//   * a true-peak ceiling cap per connection type (Bluetooth codecs overshoot:
//     -2 dBTP for A2DP, -3 dBTP for narrowband hands-free),
//   * warnings about stacking with on-board headset DSP (e.g. Turtle Beach
//     Superhuman Hearing, bass boost, virtual surround) and with the vendor's
//     control software,
//   * routing advice for headsets that expose separate Game / Chat endpoints,
//   * suggested factory presets.
//
// Profiles live in presets/devices/device-profiles.json (versioned, vendor
// neutral format). Matching is by case-insensitive tokens in the endpoint
// name (ordinary words only with a vendor or headset word, docs/11 E16:
// Database::match); the connection type comes from the platform layer when it can tell
// (Windows: the endpoint's device enumerator; macOS: the Core Audio transport
// type; Linux reports Unknown - PipeWire's device.bus is not queried) and
// otherwise from name/format heuristics (detectConnection()). Pure C++, no
// allocation after load(); not for the audio thread.
#pragma once

#include "flub/io/Json.h"

#include <cstdint>
#include <string>
#include <vector>

namespace flub::device
{
enum class Connection : uint8_t
{
    Unknown = 0,
    Analog,             // 3.5 mm / onboard codec
    Usb,                // USB Audio Class (wired USB, or a 2.4 GHz wireless dongle)
    Bluetooth,          // A2DP (stereo, lossy codec)
    BluetoothHandsFree  // HFP/HSP (mono, 8 / 16 / 32 kHz while the mic is open)
};

/** Highest sample rate of a Bluetooth voice (hands-free) link: 8 kHz CVSD,
    16 kHz mSBC / LC3-WB, 32 kHz LC3-SWB. A Bluetooth endpoint at or below it
    is hands-free, and so is any mono endpoint at or below it. */
constexpr double kHandsFreeMaxRate = 32000.0;

const char* toString (Connection c) noexcept;
Connection connectionFromString (const std::string& s) noexcept;

struct Profile
{
    std::string id;          // "turtle-beach-stealth"
    std::string vendor;      // "Turtle Beach"
    std::string family;      // "Stealth"
    std::string displayName; // "Turtle Beach Stealth series"
    std::vector<std::string> matchAny; // normalised tokens; any one matching selects the profile
    std::vector<std::string> exclude;  // tokens that veto the profile
    // docs/11 E16: the matchAny tokens flagged "generic" (ordinary words such
    // as "atlas", "stealth", "recon", "pdp"), which count only next to one of
    // vendorWords or a headset-class word (Database::match).
    std::vector<std::string> generic;
    std::vector<std::string> vendorWords; // normalised: "turtle beach", ...
    int specificity = 1;                // higher wins (family > vendor-generic)

    Connection typicalConnection = Connection::Unknown;
    float ceilingDbTp = 0.0f;          // extra cap (0 = none beyond the connection cap)
    bool onboardDsp = false;            // headset/software can apply its own EQ / enhancement
    bool onboardVirtualSurround = false;
    bool mayExposeGameChat = false;     // may present separate Game and Chat endpoints
    std::string musicPreset, gamingPreset;
    std::vector<std::string> notes;
    bool labVerified = false;           // validated on real hardware in the device lab
};

struct Match
{
    const Profile* profile = nullptr; // nullptr = generic device
    Connection connection = Connection::Unknown;
    int score = 0;
};

struct Advice
{
    float ceilingDbTp = -1.0f;         // apply as a cap (min with the user's ceiling)
    bool narrowband = false;            // <= 24 kHz sample rate (hands-free / speech modes)
    std::string suggestedPreset;        // for the current mode, may be empty
    std::vector<std::string> messages;  // user-facing guidance, most important first
};

/** Lower-case, non-alphanumerics collapsed to single spaces, trimmed. */
std::string normalise (const std::string& s);

/** Best-effort connection detection when the platform cannot tell (or to
    refine a Bluetooth hint into hands-free by the format). Evidence, in
    order: the platform hint; hands-free names ("Hands-Free", BlueZ "head
    unit", HFP / HSP); a mono format at a voice-link rate; Bluetooth names
    ("Bluetooth", "BT", BlueZ, A2DP); USB names ("USB", "dongle",
    "transmitter" - not "wireless"); onboard analog names. Any of these beats
    a matched profile's typicalConnection (Database::match). */
Connection detectConnection (const std::string& endpointName, double sampleRate, int outputChannels, Connection platformHint = Connection::Unknown);

/** The shipped profile database: presets/devices/device-profiles.json as a
    string literal in the committed, generated DeviceProfilesData.cpp. Edit
    the JSON, then regenerate with tools/scripts/embed-device-profiles.py
    (the build does not; test_device_profiles.cpp fails if the copy drifts). */
const char* builtInProfilesJson() noexcept;

class Database
{
public:
    bool load (const json::Value& root, std::string& error);
    bool loadFile (const std::string& path, std::string& error);
    /** Loads the embedded, shipped database. */
    bool loadBuiltIn (std::string& error);

    const std::vector<Profile>& profiles() const noexcept { return entries; }

    /** The highest-scoring profile (specificity * 1000 + the longest
        counted token), or none. A generic token (docs/11 E16) counts only
        when the name also holds one of the profile's vendorWords or one of
        the headset-class words, holds none of the speaker-class words
        (speaker, ceiling, soundbar, monitor, TV, HDMI, ...), and holds none
        of the other vendors' words unless it holds the profile's own:
        "Atlas Sound Ceiling Speaker" and "Headphones (Jabra Elite Pro)" match
        nothing, "Headphones (Atlas)" and "Turtle Beach Stealth" do. */
    Match match (const std::string& endpointName, double sampleRate, int outputChannels,
                 Connection platformHint = Connection::Unknown) const;

    /** The file's headsetWords / speakerWords / otherVendorWords, normalised. */
    const std::vector<std::string>& headsetWords() const noexcept { return headsetClass; }
    const std::vector<std::string>& speakerWords() const noexcept { return speakerClass; }
    const std::vector<std::string>& otherVendorWords() const noexcept { return otherVendors; }

private:
    std::vector<Profile> entries;
    std::vector<std::string> headsetClass, speakerClass, otherVendors;
};

/** Connection- and profile-specific safety caps and guidance. gamingMode
    selects the preset suggestion. */
Advice adviceFor (const Match& m, double sampleRate, bool gamingMode);
} // namespace flub::device
