// Flubsound Pro - the notify-only update check (docs/11 E54 (4), the software
// half; signed installers and installing an update stay gated on certificates).
//
// Opt-in: Settings > Diagnostics > "Check for updates" is off by default, and
// while it is off nothing of this runs: no thread, no request, no timer.
// When it is on, the app reads the repository's release list over HTTPS once
// at start (at most once a day) and when the user presses "Check now":
//
//   GET https://api.github.com/repos/flubydooby-creator/Flubsound/releases
//
// It compares the newest release of the chosen channel with the running
// version by semantic versioning (semver.org 2.0: 1.2.0-beta.2 < 1.2.0;
// build metadata ignored) and says what it found. A newer release gives a
// notice with the link to its release page (the tray at start, and
// Settings > Diagnostics). It never downloads or installs anything, so no
// signing key is involved: the user downloads the installer from the page.
//
// Channels: Stable reads the releases GitHub does not mark as pre-releases
// (and whose version has no pre-release part); Beta reads all of them.
//
// A feed is refused (Outcome::Refused, no notice) when it does not parse, is
// not a list of release objects, is larger than kMaxFeedBytes, lacks a
// release's tag, page or pre-release flag, or points a release page outside
// https://github.com/<owner>/<repo>/releases/ (so a tampered or redirected
// answer cannot send the user to another site). Tags that are not versions
// ("nightly") and draft releases are skipped.
//
// Settings (keys in AppSettings' PropertiesFile, message thread only):
//   updates.check            bool, default off
//   updates.channel          "stable" (default) | "beta"
//   updates.lastCheck        when the last check started (ms since 1970)
//   updates.lastResult       the last result as one line for the page
//   updates.lastResultUrl    the release page of a Newer / BetaOnly result
//   updates.notifiedVersion  the release the start notice was last shown for
//
// Threads: UpdateChecker runs the request on its own thread and delivers the
// result on the message thread. The request (HttpsFeedSource): juce's
// WebInputStream on Windows (WinINet) and macOS (NSURLSession); on Linux,
// where the app is built without libcurl, the system's curl program, HTTPS
// only (without curl the check fails with a message).
#pragma once

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace flub::app::diagnostics::update
{
/** A semantic version (semver.org 2.0). */
struct Version
{
    int major = 0, minor = 0, patch = 0;
    juce::StringArray prerelease; // dot-separated identifiers after '-': "beta.2" -> { "beta", "2" }

    /** "1.2.3", "v1.2.3-beta.2", "1.2.3+build.5" (build metadata ignored).
        nullopt for anything else ("1.2", "nightly", "01.2.3"). */
    static std::optional<Version> parse (const juce::String& text);
    /** The running app's version (JUCE_APPLICATION_VERSION_STRING). */
    static Version running();

    bool isPrerelease() const noexcept { return ! prerelease.isEmpty(); }
    /** < 0, 0, > 0 by semver precedence. */
    int compare (const Version& other) const;
    juce::String toString() const;
};

enum class Channel
{
    Stable,
    Beta
};

struct Release
{
    Version version;
    juce::String tag, name, url, publishedAt; // url: the release page (html_url)
    bool prerelease = false;                  // GitHub's flag, or a version with a pre-release part

    bool isOnChannel (Channel channel) const noexcept { return channel == Channel::Beta || ! prerelease; }
};

/** The feed size beyond which it is refused. */
constexpr int kMaxFeedBytes = 1024 * 1024;

/** GitHub's "list releases" answer -> releases (drafts and tags that are not
    versions skipped). Returns false and sets `error` for a malformed feed
    (see the file comment). `releasePagePrefix`: every html_url must start
    with it. */
bool parseFeed (const juce::String& json, const juce::String& releasePagePrefix, std::vector<Release>& releases, juce::String& error);

enum class Outcome
{
    Newer,      // a newer release on the channel: the notice
    Same,       // this is the newest release on the channel
    Older,      // this build is newer than every release on the channel (a test build)
    BetaOnly,   // Stable: no newer stable release, but a newer pre-release exists
    NoReleases, // no release on the channel (and no newer pre-release)
    Refused,    // the feed was malformed
    Failed      // no answer: network, HTTP status, curl missing
};

struct Result
{
    Outcome outcome = Outcome::Failed;
    std::optional<Release> release; // Newer / BetaOnly: the newer one; Same / Older: the channel's newest
    Version current;
    Channel channel = Channel::Stable;
    juce::String error;             // Refused / Failed

    /** One line for the page and the log, e.g. "Flubsound 0.2.0 is available
        (this is 0.1.0)." */
    juce::String describe() const;
    /** The release page to open, for Newer and BetaOnly; else empty. */
    juce::String downloadUrl() const;
};

/** Compares `current` with the newest release on `channel`. */
Result evaluate (const Version& current, const std::vector<Release>& releases, Channel channel);

/** parseFeed + evaluate; a malformed feed -> Refused. */
Result evaluateFeed (const Version& current, const juce::String& json, const juce::String& releasePagePrefix, Channel channel);

// ---- The request ----------------------------------------------------------------------
struct FetchResult
{
    bool ok = false;     // an answer with a 2xx status and at most kMaxFeedBytes + 1 bytes read
    int status = 0;      // HTTP status when known
    juce::String body;   // UTF-8 text
    juce::String error;  // when ! ok
};

/** Where the feed comes from. fetch() runs on the checker's thread; cancel()
    may be called from another thread and makes a running fetch return. */
class FeedSource
{
public:
    virtual ~FeedSource() = default;
    virtual FetchResult fetch (const juce::String& url) = 0;
    virtual void cancel() {}
};

/** The real request over HTTPS (see the file comment). */
std::shared_ptr<FeedSource> makeHttpsFeedSource();

/** "https://api.github.com/repos/flubydooby-creator/Flubsound/releases?per_page=30" */
juce::String defaultFeedUrl();
/** "https://github.com/flubydooby-creator/Flubsound/releases/" */
juce::String defaultReleasePagePrefix();

// ---- Settings ---------------------------------------------------------------------------
bool isEnabled (const juce::PropertiesFile& settings);
void setEnabled (juce::PropertiesFile& settings, bool enabled);
Channel getChannel (const juce::PropertiesFile& settings);
void setChannel (juce::PropertiesFile& settings, Channel channel);
/** The last result's line and release page ("" before the first check). */
juce::String getLastResultText (const juce::PropertiesFile& settings);
juce::String getLastResultUrl (const juce::PropertiesFile& settings);
juce::Time getLastCheckTime (const juce::PropertiesFile& settings);

// ---- The checker ------------------------------------------------------------------------
class UpdateChecker final : private juce::Thread
{
public:
    struct Options
    {
        juce::String feedUrl = defaultFeedUrl();
        juce::String releasePagePrefix = defaultReleasePagePrefix();
        Version current = Version::running();
        std::shared_ptr<FeedSource> source;                        // null: makeHttpsFeedSource()
        juce::RelativeTime minInterval = juce::RelativeTime::hours (24.0); // between checks at start
        std::function<juce::Time()> now;                           // null: juce::Time::getCurrentTime
    };

    /** `settings` must outlive the checker. Starts nothing. */
    UpdateChecker (juce::PropertiesFile& settings, Options options);
    /** Cancels a running request and waits for its thread; a result not yet
        delivered is dropped. */
    ~UpdateChecker() override;

    /** At start: checks when the check is on and the last one started more
        than minInterval ago. Returns true if a check started. Message thread. */
    bool checkIfDue();
    /** "Check now": checks when the check is on, whatever the interval.
        Returns false when it is off or a check is running. Message thread. */
    bool checkNow();
    bool isChecking() const noexcept { return checking.load(); }

    /** Called on the message thread with each result, after it was stored in
        the settings. */
    std::function<void (const Result&)> onResult;

private:
    void run() override;
    void deliver (const Result& result);
    bool start();

    juce::PropertiesFile& settings;
    const Options options;
    std::shared_ptr<FeedSource> source;
    Channel runningChannel = Channel::Stable;
    std::atomic<bool> checking { false };
    std::shared_ptr<bool> alive = std::make_shared<bool> (true); // guards results posted to the message thread

    JUCE_DECLARE_NON_COPYABLE (UpdateChecker)
};

/** The app's check at start (FlubsoundApplication): an UpdateChecker that runs
    checkIfDue() now, logs each result and, for a Newer release not announced
    before (updates.notifiedVersion), calls `notify` (title, message: the
    tray). Returns the checker (it must be kept alive); with the check off it
    starts nothing. */
std::unique_ptr<UpdateChecker> startAtLaunch (juce::PropertiesFile& settings,
                                              std::function<void (const juce::String& title, const juce::String& message)> notify,
                                              UpdateChecker::Options options = {});
} // namespace flub::app::diagnostics::update
