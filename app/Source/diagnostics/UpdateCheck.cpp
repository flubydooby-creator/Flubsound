#include "UpdateCheck.h"

#include "flub/io/Json.h"

namespace flub::app::diagnostics::update
{
namespace
{
namespace Keys
{
constexpr const char* check = "updates.check";
constexpr const char* channel = "updates.channel";
constexpr const char* lastCheck = "updates.lastCheck";
constexpr const char* lastResult = "updates.lastResult";
constexpr const char* lastResultUrl = "updates.lastResultUrl";
constexpr const char* notifiedVersion = "updates.notifiedVersion";
} // namespace Keys

constexpr const char* kRepository = "flubydooby-creator/Flubsound";
constexpr int kConnectTimeoutMs = 10000;

bool isIdentifierChar (juce::juce_wchar c) noexcept
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-';
}

bool isNumeric (const juce::String& s) noexcept
{
    return s.isNotEmpty() && s.containsOnly ("0123456789");
}

/** A semver identifier list ("beta.2", "build.5"): non-empty identifiers of
    [0-9A-Za-z-]; with `noLeadingZeros`, numeric ones without leading zeros. */
bool parseIdentifiers (const juce::String& text, bool noLeadingZeros, juce::StringArray& out)
{
    out = juce::StringArray::fromTokens (text, ".", "");
    if (out.isEmpty() || text.endsWithChar ('.'))
        return false;
    for (const auto& id : out)
    {
        if (id.isEmpty())
            return false;
        for (auto c : id)
            if (! isIdentifierChar (c))
                return false;
        if (noLeadingZeros && isNumeric (id) && id.length() > 1 && id[0] == '0')
            return false;
    }
    return true;
}

bool parseCoreNumber (const juce::String& s, int& value)
{
    // Digits only, no leading zero, at most 9 digits (fits an int).
    if (! isNumeric (s) || s.length() > 9 || (s.length() > 1 && s[0] == '0'))
        return false;
    value = s.getIntValue();
    return true;
}

int compareIdentifiers (const juce::String& a, const juce::String& b)
{
    const bool na = isNumeric (a), nb = isNumeric (b);
    if (na && nb) // numerically; no leading zeros, so the longer one is larger
        return a.length() != b.length() ? (a.length() < b.length() ? -1 : 1) : a.compare (b);
    if (na != nb)
        return na ? -1 : 1; // numeric identifiers have lower precedence
    return a.compare (b);   // ASCII order
}

int sign (int v) noexcept
{
    return v < 0 ? -1 : (v > 0 ? 1 : 0);
}

juce::String userAgent()
{
    return "Flubsound-Pro/" + Version::running().toString();
}

juce::String channelWord (Channel channel)
{
    return channel == Channel::Stable ? "stable " : "";
}

/** A release page's path after the prefix: "tag/v0.2.0" and the like. */
bool isSafeUrlTail (const juce::String& tail)
{
    if (tail.isEmpty() || tail.contains ("..") || tail.contains ("//"))
        return false;
    return tail.containsOnly ("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~%/+");
}

// =========================================================================================
// HTTPS (see the header's file comment)
// =========================================================================================
class HttpsFeedSource final : public FeedSource
{
public:
    FetchResult fetch (const juce::String& url) override
    {
       #if JUCE_LINUX || JUCE_BSD
        return fetchWithCurl (url);
       #else
        return fetchWithJuce (url);
       #endif
    }

    void cancel() override
    {
        const juce::ScopedLock sl (lock);
        cancelled = true;
        if (stream != nullptr)
            stream->cancel();
        if (process != nullptr)
            process->kill();
    }

private:
    static FetchResult failed (const juce::String& error)
    {
        FetchResult r;
        r.error = error;
        return r;
    }

    template <typename Read>
    FetchResult readBody (Read&& read)
    {
        juce::MemoryOutputStream out;
        char buffer[16384];
        for (;;)
        {
            const int n = read (buffer, static_cast<int> (sizeof (buffer)));
            if (n <= 0)
                break;
            out.write (buffer, static_cast<size_t> (n));
            if (out.getDataSize() > static_cast<size_t> (kMaxFeedBytes))
                break; // parseFeed refuses it
            const juce::ScopedLock sl (lock);
            if (cancelled)
                return failed ("cancelled");
        }
        FetchResult r;
        r.ok = true;
        r.body = out.toUTF8();
        return r;
    }

   #if JUCE_LINUX || JUCE_BSD
    FetchResult fetchWithCurl (const juce::String& url)
    {
        // HTTPS only, also after a redirect; the size limit as in readBody.
        const juce::StringArray args { "curl", "--silent", "--show-error", "--fail", "--location", "--max-redirs", "3",
                                       "--proto", "=https", "--proto-redir", "=https",
                                       "--connect-timeout", juce::String (kConnectTimeoutMs / 1000), "--max-time", "30",
                                       "--max-filesize", juce::String (kMaxFeedBytes),
                                       "--header", "Accept: application/vnd.github+json",
                                       "--header", "X-GitHub-Api-Version: 2022-11-28",
                                       "--user-agent", userAgent(), url };
        auto child = std::make_unique<juce::ChildProcess>();
        {
            const juce::ScopedLock sl (lock);
            if (cancelled)
                return failed ("cancelled");
            if (! child->start (args, juce::ChildProcess::wantStdOut))
                return failed ("cannot start the curl program");
            process = child.get();
        }
        auto result = readBody ([&child] (char* buffer, int size) { return child->readProcessOutput (buffer, size); });
        if (! child->waitForProcessToFinish (2000))
            child->kill();
        const auto exitCode = child->getExitCode();
        {
            const juce::ScopedLock sl (lock);
            process = nullptr;
        }
        if (! result.ok)
            return result;
        switch (exitCode)
        {
            case 0: return result;
            case 255: return failed ("the curl program was not found (the update check uses it on Linux)"); // execvp failed
            case 6: return failed ("cannot resolve the host (offline?)");
            case 7: return failed ("cannot connect to the server (offline?)");
            case 22: return failed ("the server answered with an HTTP error");
            case 28: return failed ("no answer in time");
            case 35:
            case 60: return failed ("the secure connection failed (curl " + juce::String (exitCode) + ")");
            case 63: return failed ("the release list is larger than 1 MB");
            default: return failed ("curl failed (exit code " + juce::String (exitCode) + ")");
        }
    }
   #else
    FetchResult fetchWithJuce (const juce::String& url)
    {
        auto web = std::make_unique<juce::WebInputStream> (juce::URL (url), false);
        web->withExtraHeaders ("Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\nUser-Agent: " + userAgent())
            .withConnectionTimeout (kConnectTimeoutMs)
            .withNumRedirectsToFollow (3);
        {
            const juce::ScopedLock sl (lock);
            if (cancelled)
                return failed ("cancelled");
            stream = web.get();
        }
        const bool connected = web->connect (nullptr);
        const int status = web->getStatusCode();
        FetchResult result;
        if (! connected)
            result = failed ("cannot connect to the server (offline?)");
        else if (status < 200 || status >= 300)
            result = failed ("the server answered HTTP " + juce::String (status));
        else
            result = readBody ([&web] (char* buffer, int size) { return web->read (buffer, size); });
        result.status = status;
        {
            const juce::ScopedLock sl (lock);
            stream = nullptr;
        }
        return result;
    }
   #endif

    juce::CriticalSection lock;
    bool cancelled = false;                    // final: a cancelled source fetches nothing more
    juce::WebInputStream* stream = nullptr;    // the running request (Windows / macOS)
    juce::ChildProcess* process = nullptr;     // ... or curl (Linux)
};
} // namespace

// =========================================================================================
// Version
// =========================================================================================
std::optional<Version> Version::parse (const juce::String& input)
{
    auto text = input.trim();
    if (text.startsWithChar ('v') || text.startsWithChar ('V'))
        text = text.substring (1);

    const int plus = text.indexOfChar ('+');
    if (plus >= 0)
    {
        juce::StringArray build;
        if (! parseIdentifiers (text.substring (plus + 1), false, build))
            return std::nullopt;
        text = text.substring (0, plus);
    }

    Version v;
    const int dash = text.indexOfChar ('-');
    if (dash >= 0)
    {
        if (! parseIdentifiers (text.substring (dash + 1), true, v.prerelease))
            return std::nullopt;
        text = text.substring (0, dash);
    }

    const auto core = juce::StringArray::fromTokens (text, ".", "");
    if (core.size() != 3 || text.endsWithChar ('.')
        || ! parseCoreNumber (core[0], v.major) || ! parseCoreNumber (core[1], v.minor) || ! parseCoreNumber (core[2], v.patch))
        return std::nullopt;
    return v;
}

Version Version::running()
{
   #ifdef JUCE_APPLICATION_VERSION_STRING
    if (auto v = parse (JUCE_APPLICATION_VERSION_STRING))
        return *v;
   #endif
    return {};
}

int Version::compare (const Version& other) const
{
    if (major != other.major)
        return major < other.major ? -1 : 1;
    if (minor != other.minor)
        return minor < other.minor ? -1 : 1;
    if (patch != other.patch)
        return patch < other.patch ? -1 : 1;
    // A pre-release ranks below its release: 1.0.0-beta < 1.0.0.
    if (isPrerelease() != other.isPrerelease())
        return isPrerelease() ? -1 : 1;
    for (int i = 0; i < juce::jmin (prerelease.size(), other.prerelease.size()); ++i)
        if (const int c = compareIdentifiers (prerelease[i], other.prerelease[i]); c != 0)
            return sign (c);
    return prerelease.size() == other.prerelease.size() ? 0 : (prerelease.size() < other.prerelease.size() ? -1 : 1);
}

juce::String Version::toString() const
{
    juce::String s;
    s << major << "." << minor << "." << patch;
    if (isPrerelease())
        s << "-" << prerelease.joinIntoString (".");
    return s;
}

// =========================================================================================
// The feed
// =========================================================================================
bool parseFeed (const juce::String& json, const juce::String& releasePagePrefix, std::vector<Release>& releases, juce::String& error)
{
    releases.clear();
    const auto text = json.toStdString();
    if (text.size() > static_cast<size_t> (kMaxFeedBytes))
    {
        error = "the release list is larger than 1 MB";
        return false;
    }
    flub::json::Value root;
    std::string parseError;
    if (! flub::json::parse (text, root, parseError))
    {
        error = "not JSON (" + juce::String (parseError) + ")";
        return false;
    }
    if (! root.isArray())
    {
        error = "not a list of releases";
        return false;
    }

    std::vector<Release> found;
    int index = 0;
    for (const auto& item : root.asArray())
    {
        ++index;
        const juce::String where = "release " + juce::String (index);
        if (! item.isObject())
        {
            error = where + " is not an object";
            return false;
        }
        const auto& tag = item["tag_name"];
        const auto& url = item["html_url"];
        const auto& pre = item["prerelease"];
        const auto& draft = item["draft"];
        const auto& name = item["name"];
        const auto& published = item["published_at"];
        if (! tag.isString() || tag.asString().empty())
        {
            error = where + " has no tag";
            return false;
        }
        if (! url.isString() || ! pre.isBool() || ! (draft.isNull() || draft.isBool()) || ! (name.isNull() || name.isString())
            || ! (published.isNull() || published.isString()))
        {
            error = where + " (" + juce::String (tag.asString()) + ") lacks its page or pre-release flag, or has a field of the wrong type";
            return false;
        }
        const auto page = juce::String::fromUTF8 (url.asString().c_str());
        if (! page.startsWith (releasePagePrefix) || ! isSafeUrlTail (page.substring (releasePagePrefix.length())))
        {
            error = where + " (" + juce::String (tag.asString()) + ") points outside " + releasePagePrefix;
            return false;
        }
        if (draft.asBool (false))
            continue;
        const auto tagText = juce::String::fromUTF8 (tag.asString().c_str());
        const auto version = Version::parse (tagText);
        if (! version)
            continue; // "nightly" and other tags that are not versions

        Release r;
        r.version = *version;
        r.tag = tagText;
        r.url = page;
        r.name = juce::String::fromUTF8 (name.asString().c_str());
        r.publishedAt = juce::String::fromUTF8 (published.asString().c_str());
        r.prerelease = pre.asBool (false) || version->isPrerelease();
        found.push_back (std::move (r));
    }
    releases = std::move (found);
    error.clear();
    return true;
}

Result evaluate (const Version& current, const std::vector<Release>& releases, Channel channel)
{
    Result result;
    result.current = current;
    result.channel = channel;

    const Release* newest = nullptr;    // on the channel
    const Release* newestAny = nullptr; // pre-releases included
    for (const auto& r : releases)
    {
        if (newestAny == nullptr || r.version.compare (newestAny->version) > 0)
            newestAny = &r;
        if (r.isOnChannel (channel) && (newest == nullptr || r.version.compare (newest->version) > 0))
            newest = &r;
    }

    const int vsNewest = newest != nullptr ? newest->version.compare (current) : -1;
    if (vsNewest > 0)
    {
        result.outcome = Outcome::Newer;
        result.release = *newest;
        return result;
    }
    if (channel == Channel::Stable && newestAny != nullptr && newestAny->version.compare (current) > 0)
    {
        result.outcome = Outcome::BetaOnly;
        result.release = *newestAny;
        return result;
    }
    if (newest == nullptr)
    {
        result.outcome = Outcome::NoReleases;
        return result;
    }
    result.outcome = vsNewest == 0 ? Outcome::Same : Outcome::Older;
    result.release = *newest;
    return result;
}

Result evaluateFeed (const Version& current, const juce::String& json, const juce::String& releasePagePrefix, Channel channel)
{
    std::vector<Release> releases;
    juce::String error;
    if (! parseFeed (json, releasePagePrefix, releases, error))
    {
        Result refused;
        refused.outcome = Outcome::Refused;
        refused.current = current;
        refused.channel = channel;
        refused.error = error;
        return refused;
    }
    return evaluate (current, releases, channel);
}

juce::String Result::describe() const
{
    const auto cur = current.toString();
    const auto rel = release ? release->version.toString() : juce::String();
    switch (outcome)
    {
        case Outcome::Newer:
            return "Flubsound " + rel + " is available (this is " + cur + ")" + (release->prerelease ? ", a pre-release." : ".");
        case Outcome::BetaOnly:
            return "No newer stable release (this is " + cur + "); a pre-release, Flubsound " + rel + ", is on the Beta channel.";
        case Outcome::Same:
            return "Up to date: " + cur + " is the newest " + channelWord (channel) + "release.";
        case Outcome::Older:
            return "This build (" + cur + ") is newer than the newest " + channelWord (channel) + "release (" + rel + ").";
        case Outcome::NoReleases:
            return "No " + channelWord (channel) + "release has been published yet.";
        case Outcome::Refused:
            return "The release list was refused: " + error + ".";
        case Outcome::Failed:
            return "Could not check for updates: " + error + ".";
    }
    return {};
}

juce::String Result::downloadUrl() const
{
    return (outcome == Outcome::Newer || outcome == Outcome::BetaOnly) && release ? release->url : juce::String();
}

std::shared_ptr<FeedSource> makeHttpsFeedSource()
{
    return std::make_shared<HttpsFeedSource>();
}

juce::String defaultFeedUrl()
{
    return "https://api.github.com/repos/" + juce::String (kRepository) + "/releases?per_page=30";
}

juce::String defaultReleasePagePrefix()
{
    return "https://github.com/" + juce::String (kRepository) + "/releases/";
}

// =========================================================================================
// Settings
// =========================================================================================
bool isEnabled (const juce::PropertiesFile& settings)
{
    return settings.getBoolValue (Keys::check, false);
}

void setEnabled (juce::PropertiesFile& settings, bool enabled)
{
    settings.setValue (Keys::check, enabled);
}

Channel getChannel (const juce::PropertiesFile& settings)
{
    return settings.getValue (Keys::channel, "stable") == "beta" ? Channel::Beta : Channel::Stable;
}

void setChannel (juce::PropertiesFile& settings, Channel channel)
{
    settings.setValue (Keys::channel, channel == Channel::Beta ? "beta" : "stable");
}

juce::String getLastResultText (const juce::PropertiesFile& settings)
{
    return settings.getValue (Keys::lastResult);
}

juce::String getLastResultUrl (const juce::PropertiesFile& settings)
{
    return settings.getValue (Keys::lastResultUrl);
}

juce::Time getLastCheckTime (const juce::PropertiesFile& settings)
{
    const auto ms = settings.getValue (Keys::lastCheck).getLargeIntValue();
    return ms > 0 ? juce::Time (ms) : juce::Time();
}

// =========================================================================================
// UpdateChecker
// =========================================================================================
UpdateChecker::UpdateChecker (juce::PropertiesFile& s, Options o)
    : juce::Thread ("Flubsound update check"), settings (s), options (std::move (o))
{
}

UpdateChecker::~UpdateChecker()
{
    signalThreadShouldExit();
    if (source != nullptr)
        source->cancel();
    stopThread (4000);
}

bool UpdateChecker::checkIfDue()
{
    if (! isEnabled (settings))
        return false; // off: nothing runs
    const auto last = getLastCheckTime (settings);
    const auto now = options.now != nullptr ? options.now() : juce::Time::getCurrentTime();
    if (last != juce::Time() && now >= last && now - last < options.minInterval)
        return false; // a clock set back checks again
    return start();
}

bool UpdateChecker::checkNow()
{
    return isEnabled (settings) && start();
}

bool UpdateChecker::start()
{
    if (checking.load() || isThreadRunning())
        return false;
    if (source == nullptr)
        source = options.source != nullptr ? options.source : makeHttpsFeedSource();
    runningChannel = getChannel (settings);
    const auto now = options.now != nullptr ? options.now() : juce::Time::getCurrentTime();
    settings.setValue (Keys::lastCheck, now.toMilliseconds());
    checking = true;
    if (! startThread())
    {
        checking = false;
        return false;
    }
    return true;
}

void UpdateChecker::run()
{
    if (threadShouldExit())
        return;
    const auto fetched = source->fetch (options.feedUrl);
    if (threadShouldExit())
        return;

    Result result;
    if (fetched.ok)
    {
        result = evaluateFeed (options.current, fetched.body, options.releasePagePrefix, runningChannel);
    }
    else
    {
        result.outcome = Outcome::Failed;
        result.current = options.current;
        result.channel = runningChannel;
        result.error = fetched.error.isNotEmpty() ? fetched.error : juce::String ("no answer");
    }

    std::weak_ptr<bool> weak = alive; // the destructor (message thread) waits for this thread first
    juce::MessageManager::callAsync ([weak, this, result]
    {
        if (weak.lock() != nullptr)
            deliver (result);
    });
}

void UpdateChecker::deliver (const Result& result)
{
    checking = false;
    settings.setValue (Keys::lastResult, result.describe());
    settings.setValue (Keys::lastResultUrl, result.downloadUrl());
    if (onResult != nullptr)
        onResult (result);
}

std::unique_ptr<UpdateChecker> startAtLaunch (juce::PropertiesFile& settings,
                                              std::function<void (const juce::String&, const juce::String&)> notify,
                                              UpdateChecker::Options options)
{
    auto checker = std::make_unique<UpdateChecker> (settings, std::move (options));
    checker->onResult = [&settings, notify = std::move (notify)] (const Result& result)
    {
        if (juce::Logger::getCurrentLogger() != nullptr)
            juce::Logger::writeToLog ("Update check: " + result.describe());
        if (result.outcome != Outcome::Newer || ! result.release)
            return;
        const auto version = result.release->version.toString();
        if (settings.getValue (Keys::notifiedVersion) == version)
            return; // announced before: the page still shows it
        settings.setValue (Keys::notifiedVersion, version);
        if (notify != nullptr)
            notify ("Flubsound " + version + " is available",
                    "Settings > Diagnostics has the link to its download page. Nothing is downloaded or installed automatically.");
    };
    checker->checkIfDue();
    return checker;
}
} // namespace flub::app::diagnostics::update
