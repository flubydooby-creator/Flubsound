// Flubsound Pro - reference ModelRunners (no inference runtime needed).
//
// Stand-ins with a known answer, for tests and for bringing up a host before a
// real model exists:
//   IdentityRunner     : every control 1.0 (the processor is a pure delay of L).
//   ConstantGainRunner : every control dbToGain (gainDb).
//   FailingRunner      : run() always returns false (exercises the fallback).
// They ignore their input and do not allocate in run().
#pragma once

#include "flub/neural/ModelRunner.h"

namespace flub
{
class IdentityRunner : public ModelRunner
{
public:
    explicit IdentityRunner (int frameSize, int numControls = 1, ControlKind kind = ControlKind::BroadbandGain);

    ModelDescription describe() const override { return desc; }
    bool run (const float* inFrame, float* outControls) override;

private:
    ModelDescription desc;
};

class ConstantGainRunner : public ModelRunner
{
public:
    ConstantGainRunner (int frameSize, float gainDb, int numControls = 1, ControlKind kind = ControlKind::BroadbandGain);

    ModelDescription describe() const override { return desc; }
    bool run (const float* inFrame, float* outControls) override;

private:
    ModelDescription desc;
    float gain = 1.0f;
};

class FailingRunner : public ModelRunner
{
public:
    explicit FailingRunner (int frameSize);

    ModelDescription describe() const override { return desc; }
    bool run (const float* inFrame, float* outControls) override;

private:
    ModelDescription desc;
};
} // namespace flub
