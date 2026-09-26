// App-level tests: the headset ceiling cap is wired from the output device to
// the master limiter (R6.1). EngineController runs headless
// (Options::openAudioDevice = false) and simulateOutputDevice() stands in for
// the open output device - the same path a real device change takes
// (updateDeviceProfile -> applyDeviceProfile -> AudioEngineHost::
// setMasterCeilingDb, handed to the audio thread through an atomic). The cap
// is then checked where it matters: two full-scale strips pushed ~11 dB over
// full scale must leave the engine at the cap, not above it.
#include "AppTestSupport.h"

#include "engine/EngineController.h"

#include <array>
#include <cmath>
#include <vector>

using namespace flub::app;

namespace
{
/** Full-scale 1 kHz sine on the Music and System strips, silence elsewhere. */
class LoudSource final : public StripSignalSource
{
public:
    bool renderStrip (int strip, const flub::AudioBlock& block) override
    {
        if (strip != 1 && strip != 3)
            return false;
        auto& phase = phases[static_cast<size_t> (strip)];
        for (int i = 0; i < block.numSamples; ++i)
        {
            const float v = static_cast<float> (std::sin (phase));
            for (int c = 0; c < block.numChannels; ++c)
                block.channel (c)[i] = v;
            phase += 2.0 * juce::MathConstants<double>::pi * 1000.0 / 48000.0;
        }
        return true;
    }

private:
    std::array<double, 4> phases {};
};

EngineController::Options headlessOptions (const flubapptest::TempFolder& temp)
{
    EngineController::Options o;
    o.openAudioDevice = false;
    o.restoreState = false;
    o.enableAppRouting = false;
    o.settingsFile = temp.file ("settings.xml");
    o.persistSettings = false;
    return o;
}

/** Renders `seconds` of LoudSource and returns the output sample peak (dBFS)
    over the second half (the limiter has settled). */
float renderLoudPeakDb (EngineController& controller, double seconds)
{
    auto& host = controller.getHost();
    const int block = host.getBlockSize();
    const int total = static_cast<int> (seconds * host.getSampleRate());
    std::vector<float> left (static_cast<size_t> (block)), right (static_cast<size_t> (block));
    float* outs[] = { left.data(), right.data() };

    LoudSource source;
    float peak = 0.0f;
    for (int done = 0; done < total; done += block)
    {
        host.renderOffline (source, block, outs, 2);
        if (done >= total / 2)
            for (int i = 0; i < block; ++i)
                peak = std::max ({ peak, std::abs (left[static_cast<size_t> (i)]), std::abs (right[static_cast<size_t> (i)]) });
    }
    return flubapptest::gainToDb (peak);
}
} // namespace

TEST_CASE ("App: headset ceiling cap follows the output device to the master limiter (-2 BT / -3 hands-free / -1 wired)")
{
    const flubapptest::TempFolder temp;
    EngineController controller (headlessOptions (temp));
    auto& host = controller.getHost();

    // Push the mix well over full scale so the master limiter decides the peak.
    controller.setStripGainDb (1, 6.0f);
    controller.setStripGainDb (3, 6.0f);

    // No device: the default ceiling.
    CHECK (host.getMasterCeilingDb() == -1.0f);

    struct Case
    {
        const char* device;
        double sampleRate;
        int channels;
        flub::device::Connection connection;
        float ceilingDb;
    };
    const Case cases[] = {
        { "Headphones (Stealth 600 Gen 3 Bluetooth)", 48000.0, 2, flub::device::Connection::Bluetooth, -2.0f },
        { "Headset (Stealth 600 Gen 3 Hands-Free AG Audio)", 16000.0, 1, flub::device::Connection::BluetoothHandsFree, -3.0f },
        { "Headphones (Recon 50)", 48000.0, 2, flub::device::Connection::Analog, -1.0f },                // wired, 3.5 mm
        { "Headset (Stealth 600 Gen 3 Hands-Free AG Audio)", 16000.0, 1, flub::device::Connection::BluetoothHandsFree, -3.0f },
        { "Speakers (Realtek(R) Audio)", 48000.0, 2, flub::device::Connection::Analog, -1.0f },           // back to default
    };

    for (const auto& c : cases)
    {
        controller.simulateOutputDevice (c.device, c.sampleRate, c.channels);
        std::cerr << "    " << c.device << "\n";

        CHECK (controller.getOutputDeviceName() == juce::String (c.device));
        CHECK (controller.getDeviceConnection() == c.connection);
        CHECK (controller.getDeviceAdvice().ceilingDbTp == c.ceilingDb);
        CHECK (host.getMasterCeilingDb() == c.ceilingDb);

        // Applied on the audio path: the limited output sits at the cap.
        const float peakDb = renderLoudPeakDb (controller, 1.0);
        CHECK_LE (peakDb, c.ceilingDb + 0.05f);
        CHECK_GE (peakDb, c.ceilingDb - 0.6f);
        CHECK_LE (controller.getMasterGainReductionDb(), -6.0f);
    }

    CHECK (controller.getDeviceProfileName().isEmpty()); // Realtek: generic, no headset profile
}
