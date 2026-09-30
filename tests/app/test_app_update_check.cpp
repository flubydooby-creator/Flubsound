// App-level tests: the notify-only update check (docs/11 E54 (4)).
//
// * Versions compare by semver precedence (pre-releases below their release,
//   numeric identifiers numerically, build metadata ignored); tags that are
//   not versions are not versions.
// * A fake release feed gives newer / same / older / beta-only on the Stable
//   channel, the pre-release as newer on the Beta channel, and drafts and
//   non-version tags are skipped.
// * A malformed feed is refused: not JSON, not a list, an entry that is not a
//   release, a missing tag / page / pre-release flag, a release page outside
//   the repository's releases, a feed over 1 MB.
// * While the check is off nothing runs: no request, no thread, no notice.
//   On: one request, the result on the message thread and in the settings;
//   at start at most once a day; the tray notice once per new release; a
//   request that hangs is cancelled when the checker goes.
// * Settings > Diagnostics shows "Off", "Not checked yet" or the last result.
#include "AppTestSupport.h"

#include "diagnostics/UpdateCheck.h"
#include "settings/AppSettings.h"
#include "shell/ScreenshotDriver.h"
#include "ui/SettingsDialog.h"

#include <atomic>
#include <iterator>
#include <memory>

using namespace flub::app;
using namespace flub::app::diagnostics::update;

namespace
{
const juce::String kPrefix = "https://github.com/flubydooby-creator/Flubsound/releases/";

juce::String release (const juce::String& tag, bool prerelease, bool draft = false, juce::String url = {})
{
    if (url.isEmpty())
        url = kPrefix + "tag/" + tag;
    return "{\"tag_name\":\"" + tag + "\",\"name\":\"Flubsound " + tag + "\",\"html_url\":\"" + url + "\",\"prerelease\":"
           + (prerelease ? "true" : "false") + ",\"draft\":" + (draft ? "true" : "false") + ",\"published_at\":\"2026-09-01T10:00:00Z\"}";
}

juce::String feed (const juce::StringArray& releases)
{
    return "[" + releases.joinIntoString (",") + "]";
}

Version v (const char* text)
{
    auto parsed = Version::parse (text);
    REQUIRE (parsed.has_value());
    return *parsed;
}

Result check (const char* current, const juce::String& json, Channel channel = Channel::Stable)
{
    return evaluateFeed (v (current), json, kPrefix, channel);
}

/** A feed source that counts requests and answers `body` (or blocks until
    cancelled with `block`). */
class FakeSource final : public FeedSource
{
public:
    explicit FakeSource (juce::String answer, bool blockUntilCancelled = false)
        : body (std::move (answer)), block (blockUntilCancelled)
    {
    }

    FetchResult fetch (const juce::String& url) override
    {
        ++requests;
        lastUrl = url;
        if (block)
        {
            cancelled.wait (10000);
            return { false, 0, {}, "cancelled" };
        }
        return { true, 200, body, {} };
    }

    void cancel() override { cancelled.signal(); }

    std::atomic<int> requests { 0 };
    juce::String lastUrl;

private:
    const juce::String body;
    const bool block;
    juce::WaitableEvent cancelled { true };
};

struct Fixture
{
    flubapptest::TempFolder temp;
    AppSettings settings { temp.file ("settings.xml"), false };
    juce::PropertiesFile& file = settings.getPropertiesFile();
    juce::Time clock = juce::Time (2026, 8, 29, 12, 0);

    UpdateChecker::Options options (std::shared_ptr<FeedSource> source, const char* current = "0.1.0")
    {
        UpdateChecker::Options o;
        o.source = std::move (source);
        o.current = v (current);
        o.now = [this] { return clock; };
        return o;
    }
};
} // namespace

TEST_CASE ("App update check: versions compare by semver precedence; tags that are not versions are refused (E54)")
{
    // semver.org's own precedence example, plus numeric identifiers.
    const char* ordered[] = { "1.0.0-alpha", "1.0.0-alpha.1", "1.0.0-alpha.beta", "1.0.0-beta", "1.0.0-beta.2",
                              "1.0.0-beta.11", "1.0.0-rc.1", "1.0.0", "1.0.1", "1.1.0", "2.0.0", "10.0.0" };
    for (size_t i = 0; i + 1 < std::size (ordered); ++i)
    {
        CHECK (v (ordered[i]).compare (v (ordered[i + 1])) < 0);
        CHECK (v (ordered[i + 1]).compare (v (ordered[i])) > 0);
    }
    CHECK (v ("v1.2.3").compare (v ("1.2.3")) == 0);
    CHECK (v ("1.2.3+build.7").compare (v ("1.2.3")) == 0);
    CHECK (v ("V0.2.0-beta.1").toString() == "0.2.0-beta.1");
    CHECK (v ("0.2.0-beta.1").isPrerelease());
    for (const char* bad : { "1.2", "1.2.3.4", "01.2.3", "1.2.3-", "1.2.3-01", "1.2.3-be$ta", "1.2.3+", "nightly", "", "v", "1..3",
                             "1.2.3-beta..1", "9999999999.0.0" })
        CHECK (! Version::parse (bad).has_value());
    // The running app has a version (0.1.0 in CMakeLists.txt).
    CHECK (Version::running().toString() == juce::String (JUCE_APPLICATION_VERSION_STRING));
}

TEST_CASE ("App update check: a fake feed gives newer / same / older / beta-only, and the Beta channel reads pre-releases (E54)")
{
    const auto list = feed ({ release ("v0.3.0-beta.1", true), release ("v0.2.0", false), release ("v0.1.0", false),
                              release ("nightly", true), release ("v0.9.0", false, true) }); // a tag that is no version, a draft

    auto newer = check ("0.1.0", list);
    CHECK (newer.outcome == Outcome::Newer);
    REQUIRE (newer.release.has_value());
    CHECK (newer.release->version.toString() == "0.2.0");
    CHECK (newer.downloadUrl() == kPrefix + "tag/v0.2.0");
    CHECK (newer.describe() == "Flubsound 0.2.0 is available (this is 0.1.0).");

    CHECK (check ("0.2.0", feed ({ release ("v0.2.0", false), release ("v0.1.0", false) })).outcome == Outcome::Same);
    const auto older = check ("0.4.0", list);
    CHECK (older.outcome == Outcome::Older);
    CHECK (older.downloadUrl().isEmpty());
    CHECK (older.describe().contains ("newer than the newest stable release (0.2.0)"));

    // Stable, up to date, a newer pre-release exists: said, not announced.
    const auto betaOnly = check ("0.2.0", list);
    CHECK (betaOnly.outcome == Outcome::BetaOnly);
    REQUIRE (betaOnly.release.has_value());
    CHECK (betaOnly.release->version.toString() == "0.3.0-beta.1");

    // The Beta channel: the pre-release is the newer release.
    const auto beta = check ("0.2.0", list, Channel::Beta);
    CHECK (beta.outcome == Outcome::Newer);
    CHECK (beta.release->prerelease);
    CHECK (beta.describe().contains ("a pre-release"));
    // A beta build sees its own release as newer.
    CHECK (check ("0.3.0-beta.1", feed ({ release ("v0.3.0", false) })).outcome == Outcome::Newer);
    // GitHub's pre-release flag counts even on a plain version.
    CHECK (check ("0.1.0", feed ({ release ("v0.5.0", true) })).outcome == Outcome::BetaOnly);

    CHECK (check ("0.1.0", "[]").outcome == Outcome::NoReleases);
    CHECK (check ("0.1.0", feed ({ release ("nightly", false), release ("v1.0.0", false, true) })).outcome == Outcome::NoReleases);
}

TEST_CASE ("App update check: a malformed feed is refused, and so is a release page outside the repository (E54)")
{
    const auto good = release ("v0.2.0", false);
    struct Case
    {
        const char* what;
        juce::String json;
    };
    const Case cases[] = {
        { "not JSON", "<html>rate limited</html>" },
        { "truncated", "[" + good },
        { "an object", "{\"message\":\"Not Found\"}" },
        { "an entry that is not an object", "[" + good + ",\"v0.3.0\"]" },
        { "no tag", "[{\"html_url\":\"" + kPrefix + "tag/x\",\"prerelease\":false}]" },
        { "a numeric tag", "[{\"tag_name\":2,\"html_url\":\"" + kPrefix + "tag/x\",\"prerelease\":false}]" },
        { "no page", "[{\"tag_name\":\"v0.2.0\",\"prerelease\":false}]" },
        { "no pre-release flag", "[{\"tag_name\":\"v0.2.0\",\"html_url\":\"" + kPrefix + "tag/v0.2.0\"}]" },
        { "a string flag", "[{\"tag_name\":\"v0.2.0\",\"html_url\":\"" + kPrefix + "tag/v0.2.0\",\"prerelease\":\"no\"}]" },
        { "another site", feed ({ release ("v0.2.0", false, false, "https://evil.example/Flubsound/releases/tag/v0.2.0") }) },
        { "http", feed ({ release ("v0.2.0", false, false, "http://github.com/flubydooby-creator/Flubsound/releases/tag/v0.2.0") }) },
        { "another repository", feed ({ release ("v0.2.0", false, false, "https://github.com/someone/Flubsound/releases/tag/v0.2.0") }) },
        { "a path out of the releases", feed ({ release ("v0.2.0", false, false, kPrefix + "../../../evil/download") }) },
        { "a host trick", feed ({ release ("v0.2.0", false, false, kPrefix + "tag/v0.2.0@evil.example") }) },
        { "over 1 MB", "[" + good + "," + juce::String::repeatedString (" ", kMaxFeedBytes) + "]" },
    };
    for (const auto& c : cases)
    {
        const auto result = check ("0.1.0", c.json);
        if (result.outcome != Outcome::Refused)
            std::cerr << "    not refused: " << c.what << "\n";
        CHECK (result.outcome == Outcome::Refused);
        CHECK (result.downloadUrl().isEmpty());
        CHECK (result.describe().startsWith ("The release list was refused: "));
    }
    // ... while a feed with extra fields and nulls is fine.
    CHECK (check ("0.1.0", "[{\"tag_name\":\"v0.2.0\",\"name\":null,\"published_at\":null,\"html_url\":\"" + kPrefix
                               + "tag/v0.2.0\",\"prerelease\":false,\"assets\":[],\"author\":{\"login\":\"x\"}}]")
               .outcome == Outcome::Newer);
}

TEST_CASE ("App update check: while it is off nothing runs - no request, no thread, no notice (E54)")
{
    Fixture f;
    auto source = std::make_shared<FakeSource> (feed ({ release ("v9.0.0", false) }));
    CHECK (! isEnabled (f.file)); // off by default
    int notices = 0;
    {
        auto checker = startAtLaunch (f.file, [&notices] (const juce::String&, const juce::String&) { ++notices; }, f.options (source));
        CHECK (! checker->isChecking());
        CHECK (! checker->checkNow());
        CHECK (! checker->checkIfDue());
        flubapptest::pumpMessagesUntil ([] { return false; }, 100);
    }
    CHECK (source->requests == 0);
    CHECK (notices == 0);
    CHECK (getLastCheckTime (f.file) == juce::Time());
    CHECK (getLastResultText (f.file).isEmpty());
}

TEST_CASE ("App update check: on, it asks once, stores the result and announces a new release once; at most daily at start (E54)")
{
    Fixture f;
    setEnabled (f.file, true);
    auto source = std::make_shared<FakeSource> (feed ({ release ("v0.2.0", false), release ("v0.1.0", false) }));
    juce::StringArray notices;
    auto notify = [&notices] (const juce::String& title, const juce::String& message) { notices.add (title + " | " + message); };

    {
        auto checker = startAtLaunch (f.file, notify, f.options (source));
        CHECK (checker->isChecking());
        REQUIRE (flubapptest::pumpMessagesUntil ([&checker] { return ! checker->isChecking(); }, 5000));
    }
    CHECK (source->requests == 1);
    CHECK (source->lastUrl == "https://api.github.com/repos/flubydooby-creator/Flubsound/releases?per_page=30");
    CHECK (getLastCheckTime (f.file) == f.clock);
    CHECK (getLastResultText (f.file) == "Flubsound 0.2.0 is available (this is 0.1.0).");
    CHECK (getLastResultUrl (f.file) == kPrefix + "tag/v0.2.0");
    REQUIRE (notices.size() == 1);
    CHECK (notices[0].startsWith ("Flubsound 0.2.0 is available | "));
    CHECK (notices[0].contains ("Nothing is downloaded or installed"));

    // The next start 2 h later: no request.
    f.clock = f.clock + juce::RelativeTime::hours (2.0);
    {
        auto checker = startAtLaunch (f.file, notify, f.options (source));
        CHECK (! checker->isChecking());
    }
    CHECK (source->requests == 1);

    // A day later: asks again, same release, no second notice.
    f.clock = f.clock + juce::RelativeTime::hours (23.0);
    {
        auto checker = startAtLaunch (f.file, notify, f.options (source));
        REQUIRE (flubapptest::pumpMessagesUntil ([&checker] { return ! checker->isChecking(); }, 5000));
        // "Check now" ignores the interval.
        CHECK (checker->checkNow());
        REQUIRE (flubapptest::pumpMessagesUntil ([&checker] { return ! checker->isChecking(); }, 5000));
    }
    CHECK (source->requests == 3);
    CHECK (notices.size() == 1);

    // The Beta channel: a newer pre-release is a newer release.
    setChannel (f.file, Channel::Beta);
    auto betaSource = std::make_shared<FakeSource> (feed ({ release ("v0.3.0-beta.1", true), release ("v0.2.0", false) }));
    {
        UpdateChecker checker (f.file, f.options (betaSource, "0.2.0"));
        Result got;
        checker.onResult = [&got] (const Result& r) { got = r; };
        CHECK (checker.checkNow());
        REQUIRE (flubapptest::pumpMessagesUntil ([&checker] { return ! checker.isChecking(); }, 5000));
        CHECK (got.outcome == Outcome::Newer);
        CHECK (got.channel == Channel::Beta);
    }
    CHECK (getLastResultUrl (f.file) == kPrefix + "tag/v0.3.0-beta.1");
}

TEST_CASE ("App update check: a refused feed and a failed request are reported, not announced (E54)")
{
    Fixture f;
    setEnabled (f.file, true);
    int notices = 0;
    auto notify = [&notices] (const juce::String&, const juce::String&) { ++notices; };
    {
        auto checker = startAtLaunch (f.file, notify, f.options (std::make_shared<FakeSource> ("{\"message\":\"API rate limit exceeded\"}")));
        REQUIRE (flubapptest::pumpMessagesUntil ([&checker] { return ! checker->isChecking(); }, 5000));
    }
    CHECK (getLastResultText (f.file) == "The release list was refused: not a list of releases.");
    CHECK (getLastResultUrl (f.file).isEmpty());

    struct Failing final : FeedSource
    {
        FetchResult fetch (const juce::String&) override { return { false, 0, {}, "cannot connect to the server (offline?)" }; }
    };
    f.clock = f.clock + juce::RelativeTime::days (2.0);
    {
        auto checker = startAtLaunch (f.file, notify, f.options (std::make_shared<Failing>()));
        REQUIRE (flubapptest::pumpMessagesUntil ([&checker] { return ! checker->isChecking(); }, 5000));
    }
    CHECK (getLastResultText (f.file) == "Could not check for updates: cannot connect to the server (offline?).");
    CHECK (notices == 0);
}

TEST_CASE ("App update check: a request that hangs is cancelled when the checker goes, and its result is dropped (E54)")
{
    Fixture f;
    setEnabled (f.file, true);
    auto source = std::make_shared<FakeSource> (juce::String(), true);
    bool delivered = false;
    const auto start = juce::Time::getMillisecondCounterHiRes();
    {
        UpdateChecker checker (f.file, f.options (source));
        checker.onResult = [&delivered] (const Result&) { delivered = true; };
        CHECK (checker.checkNow());
        REQUIRE (flubapptest::pumpMessagesUntil ([&source] { return source->requests.load() == 1; }, 5000));
    }
    CHECK_LE (juce::Time::getMillisecondCounterHiRes() - start, 1500.0);
    flubapptest::pumpMessagesUntil ([] { return false; }, 50);
    CHECK (! delivered);
    CHECK (getLastResultText (f.file).isEmpty());
}

TEST_CASE ("App update check: Settings > Diagnostics shows off, not checked yet, or the last result (E54)")
{
    Fixture f;
    CHECK (ui::SettingsDialog::describeUpdateCheck (f.file) == "Off: Flubsound does not look for updates.");
    setEnabled (f.file, true);
    CHECK (ui::SettingsDialog::describeUpdateCheck (f.file) == "Not checked yet.");
    {
        UpdateChecker checker (f.file, f.options (std::make_shared<FakeSource> (feed ({ release ("v0.1.0", false) }))));
        CHECK (checker.checkNow());
        REQUIRE (flubapptest::pumpMessagesUntil ([&checker] { return ! checker.isChecking(); }, 5000));
    }
    const auto text = ui::SettingsDialog::describeUpdateCheck (f.file);
    CHECK (text.startsWith ("Last check " + f.clock.formatted ("%Y-%m-%d %H:%M") + ": "));
    CHECK (text.endsWith ("Up to date: 0.1.0 is the newest stable release."));
    setEnabled (f.file, false);
    CHECK (ui::SettingsDialog::describeUpdateCheck (f.file).startsWith ("Off"));

    // The page's screenshot state (--state settings-diagnostics).
    ScreenshotDriver::Options o;
    juce::String error;
    CHECK (ScreenshotDriver::parseCommandLine (juce::StringArray ({ "--screenshot", "a.png", "--state", "settings-diagnostics" }), o, error));
    CHECK (o.states.contains ("settings-diagnostics"));
}
