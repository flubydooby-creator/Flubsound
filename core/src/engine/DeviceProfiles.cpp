#include "flub/engine/DeviceProfiles.h"

#include "flub/io/FilePath.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace flub::device
{
const char* toString (Connection c) noexcept
{
    switch (c)
    {
        case Connection::Analog: return "analog";
        case Connection::Usb: return "usb";
        case Connection::Bluetooth: return "bluetooth";
        case Connection::BluetoothHandsFree: return "bluetooth-hands-free";
        case Connection::Unknown: break;
    }
    return "unknown";
}

Connection connectionFromString (const std::string& s) noexcept
{
    if (s == "analog") return Connection::Analog;
    if (s == "usb" || s == "wireless" || s == "2.4ghz") return Connection::Usb;
    if (s == "bluetooth") return Connection::Bluetooth;
    if (s == "bluetooth-hands-free") return Connection::BluetoothHandsFree;
    return Connection::Unknown;
}

std::string normalise (const std::string& s)
{
    std::string out;
    out.reserve (s.size());
    bool pendingSpace = false;
    for (unsigned char ch : s)
    {
        if (std::isalnum (ch))
        {
            if (pendingSpace && ! out.empty())
                out.push_back (' ');
            pendingSpace = false;
            out.push_back (static_cast<char> (std::tolower (ch)));
        }
        else
        {
            pendingSpace = true;
        }
    }
    return out;
}

namespace
{
/** Whole-word (token sequence) containment on normalised strings. */
bool containsToken (const std::string& haystack, const std::string& token)
{
    if (token.empty())
        return false;
    const std::string h = " " + haystack + " ";
    return h.find (" " + token + " ") != std::string::npos;
}
} // namespace

Connection detectConnection (const std::string& endpointName, double sampleRate, int outputChannels, Connection platformHint)
{
    if (platformHint != Connection::Unknown)
    {
        // A Bluetooth endpoint in a narrowband mono format is the hands-free profile.
        if (platformHint == Connection::Bluetooth && sampleRate > 0.0 && sampleRate <= 16000.0)
            return Connection::BluetoothHandsFree;
        return platformHint;
    }

    const std::string n = normalise (endpointName);
    if (containsToken (n, "hands free") || containsToken (n, "handsfree") || containsToken (n, "hands free ag audio"))
        return Connection::BluetoothHandsFree;
    if (sampleRate > 0.0 && sampleRate <= 16000.0 && outputChannels <= 1)
        return Connection::BluetoothHandsFree; // narrowband mono: speech profile
    if (containsToken (n, "bluetooth") || containsToken (n, "bt"))
        return Connection::Bluetooth;
    if (containsToken (n, "usb") || containsToken (n, "dongle") || containsToken (n, "transmitter") || containsToken (n, "wireless"))
        return Connection::Usb;
    if (containsToken (n, "realtek") || containsToken (n, "high definition audio") || containsToken (n, "line out"))
        return Connection::Analog;
    return Connection::Unknown;
}

bool Database::load (const json::Value& root, std::string& error)
{
    if (root["format"].asString() != "flubsound-device-profiles")
    {
        error = "not a flubsound device profile file";
        return false;
    }
    if (root["version"].asNumber (0.0) > 1.0)
    {
        error = "device profile file is from a newer Flubsound version";
        return false;
    }

    std::vector<Profile> loaded;
    for (const auto& v : root["profiles"].asArray())
    {
        Profile p;
        p.id = v["id"].asString();
        p.vendor = v["vendor"].asString();
        p.family = v["family"].asString();
        p.displayName = v["displayName"].asString();
        for (const auto& t : v["matchAny"].asArray())
            if (t.isString())
                p.matchAny.push_back (normalise (t.asString()));
        for (const auto& t : v["exclude"].asArray())
            if (t.isString())
                p.exclude.push_back (normalise (t.asString()));
        p.specificity = static_cast<int> (v["specificity"].asNumber (1.0));
        p.typicalConnection = connectionFromString (v["typicalConnection"].asString());
        p.ceilingDbTp = static_cast<float> (v["ceilingDbTp"].asNumber (0.0));
        p.onboardDsp = v["onboardDsp"].asBool (false);
        p.onboardVirtualSurround = v["onboardVirtualSurround"].asBool (false);
        p.mayExposeGameChat = v["mayExposeGameChat"].asBool (false);
        p.musicPreset = v["musicPreset"].asString();
        p.gamingPreset = v["gamingPreset"].asString();
        for (const auto& t : v["notes"].asArray())
            if (t.isString())
                p.notes.push_back (t.asString());
        p.labVerified = v["labVerified"].asBool (false);

        if (p.id.empty() || p.matchAny.empty())
        {
            error = "profile without id or matchAny tokens";
            return false;
        }
        loaded.push_back (std::move (p));
    }
    entries = std::move (loaded);
    return true;
}

bool Database::loadFile (const std::string& path, std::string& error)
{
    std::ifstream f (io::pathFromUtf8 (path), std::ios::binary);
    if (! f)
    {
        error = "cannot open " + path;
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    json::Value v;
    if (! json::parse (ss.str(), v, error))
        return false;
    return load (v, error);
}

bool Database::loadBuiltIn (std::string& error)
{
    json::Value v;
    if (! json::parse (builtInProfilesJson(), v, error))
        return false;
    return load (v, error);
}

Match Database::match (const std::string& endpointName, double sampleRate, int outputChannels, Connection platformHint) const
{
    Match best;
    best.connection = detectConnection (endpointName, sampleRate, outputChannels, platformHint);
    const std::string n = normalise (endpointName);

    for (const auto& p : entries)
    {
        if (std::any_of (p.exclude.begin(), p.exclude.end(), [&] (const std::string& t) { return containsToken (n, t); }))
            continue;
        int longest = 0;
        for (const auto& t : p.matchAny)
            if (containsToken (n, t))
                longest = std::max (longest, static_cast<int> (t.size()));
        if (longest == 0)
            continue;
        const int score = p.specificity * 1000 + longest;
        if (score > best.score)
        {
            best.profile = &p;
            best.score = score;
        }
    }

    if (best.connection == Connection::Unknown && best.profile != nullptr)
        best.connection = best.profile->typicalConnection;
    return best;
}

Advice adviceFor (const Match& m, double sampleRate, bool gamingMode)
{
    Advice a;
    a.narrowband = sampleRate > 0.0 && sampleRate <= 24000.0;

    switch (m.connection)
    {
        case Connection::Bluetooth:
            a.ceilingDbTp = -2.0f;
            a.messages.push_back ("Bluetooth output: the lossy codec can overshoot peaks, so the output ceiling is capped at -2 dBTP. "
                                  "Bluetooth adds its own ~100-300 ms of latency; for competitive play use the USB / 2.4 GHz dongle or a cable.");
            break;
        case Connection::BluetoothHandsFree:
            a.ceilingDbTp = -3.0f;
            a.messages.push_back ("The headset is in Bluetooth hands-free mode (microphone open): audio is mono and narrowband. "
                                  "For full-quality game and music audio, use the stereo (A2DP) endpoint or the headset's USB / 2.4 GHz connection, "
                                  "and a separate microphone path if possible.");
            break;
        case Connection::Usb:
        case Connection::Analog:
        case Connection::Unknown:
            break;
    }

    if (a.narrowband && m.connection != Connection::BluetoothHandsFree)
        a.messages.push_back ("The output runs at a low sample rate; Flubsound adapts automatically (the air exciter is disabled below 42 kHz), "
                              "but 48 kHz gives the best quality if the device supports it.");

    if (const Profile* p = m.profile)
    {
        if (p->ceilingDbTp < 0.0f)
            a.ceilingDbTp = std::min (a.ceilingDbTp, p->ceilingDbTp);
        if (p->onboardDsp)
            a.messages.push_back ("This headset (or its companion software) can apply its own EQ and enhancement. Use its flat / default "
                                  "sound setting while Flubsound is active, so the two do not stack.");
        if (p->onboardVirtualSurround)
            a.messages.push_back ("Turn off the headset's own virtual surround (and Windows Sonic / other spatial sound) when using "
                                  "Flubsound's Headphone Virtualizer: two HRTF stages in series sound muffled and distant.");
        if (p->mayExposeGameChat)
            a.messages.push_back ("If the headset shows separate 'Game' and 'Chat' outputs, send Flubsound's output to the Game output and leave "
                                  "voice chat apps on the Chat output, so the headset's game/chat balance control keeps working.");
        for (const auto& note : p->notes)
            a.messages.push_back (note);
        a.suggestedPreset = gamingMode ? p->gamingPreset : p->musicPreset;
    }

    if (m.connection == Connection::Bluetooth || m.connection == Connection::BluetoothHandsFree)
        a.suggestedPreset = "Bluetooth Headphones";
    return a;
}
} // namespace flub::device
