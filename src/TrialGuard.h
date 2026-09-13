#pragma once
#include <cmath>
#include <cstdint>

class AxisTrialGuard
{
public:
    struct Frame { float gain = 1.0f; float noise = 0.0f; };

    void prepare (double sampleRate)
    {
        sr = sampleRate > 1.0 ? sampleRate : 48000.0;
        activeSamples = 0.0; phase = 0.0; rng = 0x6d2b79f5u; expired = false;
    }

    Frame next()
    {
        if (! expired)
        {
            activeSamples += 1.0;
            if (activeSamples >= sr * kTrialSeconds) { expired = true; phase = 0.0; }
            return {};
        }
        phase += 1.0 / sr;
        if (phase >= kCycleSeconds) phase -= kCycleSeconds;
        if (phase < kBurstSeconds)
        {
            rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
            const float white = (float) ((rng & 0xffffu) / 32767.5f - 1.0f);
            return { 0.0f, white * 0.055f };
        }
        return { 0.0f, 0.0f };
    }

    bool hasExpired() const noexcept { return expired; }
    static constexpr double kTrialSeconds = 10.0 * 60.0;

private:
    double sr = 48000.0, activeSamples = 0.0, phase = 0.0;
    std::uint32_t rng = 0x6d2b79f5u;
    bool expired = false;
    static constexpr double kBurstSeconds = 0.09;
    static constexpr double kCycleSeconds = 1.50;
};
