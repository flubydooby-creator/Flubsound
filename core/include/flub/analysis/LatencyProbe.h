// Flubsound Pro - loopback latency probe (docs/11 E42d).
//
// Measures the delay of a real playback -> capture path (a loopback cable
// from an output to an input, or a measurement microphone at the earcup) from
// a recording of a known probe signal:
//
//   probe = lead-in silence, then `runs` x (exponential sine sweep + gap)
//
// Each run is deconvolved on its own: the recording from the run's start is
// divided by the sweep's spectrum (flub::Fft, regularised and band-limited
// to the sweep's range), which leaves the path's impulse response. Its
// largest peak (either polarity) within 0 .. maxDelayMs, refined by a
// parabola through the peak and its neighbours, is that run's delay. A run
// counts only when the peak stands at least minSnrDb above the RMS of the
// rest of the response (the noise, the path's own tail and distortion
// products); the result is the median of the accepted runs, and the
// measurement fails when fewer than half of the runs are accepted.
//
// The delay is the time from the start of the probe file to its arrival in
// the recording, so playback and capture must start sample-locked (one
// duplex stream: a DAW, `jack_iodelay`-style tools, pw-cat on one graph), or
// the recording carries a reference: analyseRelative() measures the delay of
// one channel against another that recorded the probe over a direct path
// (a second loopback cable), so an unknown start offset cancels.
//
// The gap after each sweep is maxDelayMs + 250 ms, so a run's response ends
// before the next sweep starts for any delay up to maxDelayMs: the analysis
// must use the settings the probe was generated with.
//
// Not for the audio thread (allocates; about 10 FFTs of 2^16..2^18 points).
#pragma once

#include "flub/dsp/Fft.h"

#include <cstdint>
#include <string>
#include <vector>

namespace flub::latency
{
struct ProbeSettings
{
    double sampleRate = 48000.0;
    double sweepSeconds = 1.0;
    double startHz = 20.0;
    double endHz = 20000.0;     // limited to 0.45 x sampleRate
    double levelDbfs = -12.0;   // sweep peak
    double maxDelayMs = 500.0;  // longest delay searched (sets the gap)
    double leadInSeconds = 0.5; // silence before the first sweep
    int runs = 10;
    double minSnrDb = 30.0;     // analysis only: runs below are rejected
};

/** "" when the settings can be generated / analysed, else what is wrong. */
std::string validate (const ProbeSettings& settings);

int sweepLength (const ProbeSettings& settings);      // samples
int gapLength (const ProbeSettings& settings);        // samples after each sweep
int64_t runStart (const ProbeSettings& settings, int run); // first sample of sweep `run`
int64_t probeLength (const ProbeSettings& settings);  // the whole probe, samples

/** One exponential (log) sweep from startHz to endHz, 5 ms raised-cosine
    fades, peak at levelDbfs. */
std::vector<float> makeSweep (const ProbeSettings& settings);

/** The probe to play: lead-in, then `runs` x (sweep + gap). */
std::vector<float> makeProbe (const ProbeSettings& settings);

struct Run
{
    int index = 0;
    double delaySamples = 0.0; // refined peak position
    double snrDb = 0.0;        // peak re the RMS of the rest of the response
    bool inverted = false;     // the peak is negative (the path inverts polarity)
    bool accepted = false;     // snrDb >= minSnrDb
};

struct Result
{
    bool ok = false;
    std::string error;        // why !ok
    std::vector<Run> runs;    // every run the recording covers
    int acceptedRuns = 0;
    double delaySamples = 0.0; // median of the accepted runs
    double delayMs = 0.0;
    double spreadSamples = 0.0; // max - min over the accepted runs
    bool inverted = false;      // most accepted runs are inverted
};

class LatencyProbe
{
public:
    /** Allocates (the sweep's inverse spectrum). Settings must validate(). */
    explicit LatencyProbe (const ProbeSettings& settings);

    const ProbeSettings& getSettings() const noexcept { return settings; }

    /** Deconvolves run `run` of a recording that started with the probe. */
    Run measureRun (const float* recording, int64_t numSamples, int run);

    /** Every run of the recording, median of those accepted. */
    Result analyse (const float* recording, int64_t numSamples);

    /** Per run: the delay of `recording` minus the delay of `reference` (both
        must be accepted); the median of those differences. For a recording
        whose start is not locked to playback, with the probe also captured
        over a direct path: the runs are found in `reference` first (locate),
        so any start offset works as long as the recording holds complete
        runs, and the difference may be negative down to -10 ms and positive
        up to maxDelayMs - 10 ms. */
    Result analyseRelative (const float* recording, const float* reference, int64_t numSamples);

    /** Sample index of the first arrival of the probe's sweep in a recording
        (to within a few samples), -1 when none is found. */
    int64_t locate (const float* recording, int64_t numSamples) const;

    /** The deconvolved response of one run, lags 0 .. responseLength() - 1
        (for plots and tests). */
    std::vector<float> impulseResponse (const float* recording, int64_t numSamples, int run);
    int responseLength() const noexcept { return windowLength; }

private:
    void deconvolve (const float* recording, int64_t numSamples, int64_t start);
    Run measureAt (const float* recording, int64_t numSamples, int run, int64_t start);
    Result summarise (std::vector<Run> runs) const;

    ProbeSettings settings;
    Fft fft;
    int fftSize = 0, sweepSamples = 0, windowLength = 0, segmentLength = 0;
    std::vector<Fft::Complex> inverse; // conj(X) / (|X|^2 + eps), band-limited; fftSize / 2 + 1 bins
    std::vector<float> segment, response;
    std::vector<Fft::Complex> bins;
};
} // namespace flub::latency
