// App-level tests: docs/11 E48's input-link line in the routing panel, from
// the router's connectEndpointInputs status (the JUCE device path; the
// native PipeWire device's own line is tested against a server in
// test_app_pipewire.cpp). A fake router, no device: runs on every OS.
#include "AppTestSupport.h"

#include "engine/EngineController.h"
#include "ui/RoutingPanel.h"

#include <mutex>

using namespace flub::app;

namespace
{
/** Answers connectEndpointInputs with a scripted status. */
class LinkingRouter final : public flub::platform::AppAudioRouter
{
public:
    bool isSupported() const override { return true; }
    std::vector<flub::platform::AudioSessionInfo> enumerateSessions() override { return {}; }
    bool setAppEndpoint (uint32_t, const std::string&, std::string& error) override
    {
        error = "test";
        return false;
    }
    void openSystemRoutingSettings() override {}

    bool connectEndpointInputs (const std::vector<EndpointInput>& inputs, std::string& status) override
    {
        const std::lock_guard<std::mutex> guard (mutex);
        last = inputs;
        ++calls;
        status = scriptedStatus;
        return scriptedOk;
    }

    void script (bool ok, const std::string& status)
    {
        const std::lock_guard<std::mutex> guard (mutex);
        scriptedOk = ok;
        scriptedStatus = status;
    }
    int callCount() const
    {
        const std::lock_guard<std::mutex> guard (mutex);
        return calls;
    }
    std::vector<EndpointInput> lastInputs() const
    {
        const std::lock_guard<std::mutex> guard (mutex);
        return last;
    }

private:
    mutable std::mutex mutex;
    std::vector<EndpointInput> last;
    bool scriptedOk = true;
    std::string scriptedStatus;
    int calls = 0;
};
} // namespace

TEST_CASE ("App: the routing panel's link line shows what the router could not link, and nothing once it is linked (E48)")
{
    const flubapptest::TempFolder temp;
    EngineController::Options o;
    o.openAudioDevice = false;
    o.restoreState = false;
    o.enableAppRouting = false; // started below with the fake router
    o.settingsFile = temp.file ("settings.xml");
    o.persistSettings = false;
    o.foregroundAppFactory = [] { return std::unique_ptr<flub::platform::ForegroundApp>(); };
    EngineController controller (o);
    auto& routing = controller.getRouting();

    // Game and Music read the device input (a JACK client's channels 0 and 8).
    std::array<int, AudioEngineHost::kMaxStrips> map {};
    map.fill (-1);
    map[0] = 0;
    map[1] = 8;
    controller.getHost().setDeviceInputMap (map);

    auto owned = std::make_unique<LinkingRouter>();
    auto* router = owned.get();
    const std::string missing = "PipeWire has no sink 'flubsound_music' (create the Flubsound sinks with "
                                "platform/linux/flubsound-pipewire-setup.sh install).";
    router->script (false, missing);
    routing.setRouter (std::move (owned), false);
    routing.start();

    ui::RoutingPanel panel (controller);
    panel.setSize (340, 900);
    REQUIRE (flubapptest::pumpMessagesUntil ([&] { return router->callCount() > 0; }));

    // The worker handed the device map to the router (the device is not the
    // native node, so it links), and its status reaches the panel.
    const auto inputs = router->lastInputs();
    REQUIRE (inputs.size() >= 2);
    CHECK (inputs[0].firstInputChannel == 0);
    CHECK (inputs[1].firstInputChannel == 8);
    CHECK (routing.getInputLinkStatus() == juce::String (missing));
    CHECK (! routing.areInputsLinked());
    panel.refreshRouting();
    CHECK (panel.getLinkStatus() == juce::String (missing));
    CHECK (panel.isLinkStatusWarning());

    // Linked: the line goes.
    router->script (true, {});
    const int before = router->callCount();
    routing.refresh();
    REQUIRE (flubapptest::pumpMessagesUntil ([&] { return router->callCount() > before; }));
    panel.refreshLinkStatus();
    CHECK (panel.getLinkStatus().isEmpty());
    CHECK (! panel.isLinkStatusWarning());
    CHECK (routing.areInputsLinked());

    routing.shutdown();
}
