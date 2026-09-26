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
// name; the connection type comes from the platform layer when it can tell
// (Windows enumerator, macOS transport type, PipeWire device.bus) and falls
// back to name/format heuristics. Pure C++, no allocation after load(); not
// for the audio thread.
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
    BluetoothHandsFree  // HFP/HSP (mono, narrowband 8/16 kHz while the mic is open)
};

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

/** Best-effort connection detection when the platform cannot tell. */
Connection detectConnection (const std::string& endpointName, double sampleRate, int outputChannels, Connection platformHint = Connection::Unknown);

class Database
{
public:
    bool load (const json::Value& root, std::string& error);
    bool loadFile (const std::string& path, std::string& error);

    const std::vector<Profile>& profiles() const noexcept { return entries; }

    Match match (const std::string& endpointName, double sampleRate, int outputChannels,
                 Connection platformHint = Connection::Unknown) const;

private:
    std::vector<Profile> entries;
};

/** Connection- and profile-specific safety caps and guidance. gamingMode
    selects the preset suggestion. */
Advice adviceFor (const Match& m, double sampleRate, bool gamingMode);
} // namespace flub::device
