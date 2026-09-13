#pragma once
#include <cmath>
#include <cstdint>
#include <juce_cryptography/juce_cryptography.h>

class AxisTrialGuard
{
public:
    struct Frame { float gain = 1.0f; float noise = 0.0f; };

    void setProduct (const char* productName) { product = productName; }

    void prepare (double sampleRate)
    {
        sr = sampleRate > 1.0 ? sampleRate : 48000.0;
        activeSamples = 0.0; phase = 0.0; rng = 0x6d2b79f5u; expired = false;
        licensed = verifyLicense();
    }

    Frame next()
    {
        if (licensed || ! expired)
        {
            if (licensed) return {};
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
    bool isLicensed() const noexcept { return licensed; }
    static constexpr double kTrialSeconds = 10.0 * 60.0;

private:
    bool verifyLicense() const
    {
        const auto file = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                            .getChildFile ("AxisRift").getChildFile ("license.txt");
        const auto text = file.loadFileAsString();
        if (text.isEmpty() || ! text.contains ("signature=") || ! text.contains ("product=" + product)) return false;
        const auto payload = text.upToLastOccurrenceOf ("signature=", false, false);
        auto signature = text.fromLastOccurrenceOf ("signature=", false, false).trim();
        if (signature.containsAnyOf ("\\r\\n")) signature = signature.upToFirstOccurrenceOf ("\\r", false, false).upToFirstOccurrenceOf ("\\n", false, false);
        juce::RSAKey publicKey (juce::String ("10001,cb63257af000b4c5ce65f233016b3bebcb995fb3a43d22a9560fcfdbf266bdd7c0c5932874da395c00070b2efca6d7f34cf36d5218576a6dde13df523fa66654d6487b462aab11fbfdf97ae8f22e6d3b69e7cb70312278cd943c7ea0e675675157d3920518ca860b7f9608fa9d73b4900d23a01c43fc76dcf692d1f9de7c65972a71ed9e1154f245dede6acec55ec35f6d91288e47687d0ae94b236a33051d939479c705a78ef4b7b568164edf3e2e530a0d0b322762436a9375d4b0e43b71714c0417d8e84c1eb1c643120581248e259e4ed17441b76dcb84b2390efe7383a1ae1a73d2289f26e83d4125cb1dbb6ee85f7234e33e8c6030ca755bbfd5533845"));
        if (! publicKey.isValid()) return false;
        juce::BigInteger signedHash; signedHash.parseString (signature, 16);
        if (! publicKey.applyToValue (signedHash)) return false;
        juce::BigInteger expected; expected.parseString (juce::SHA256 (payload.toRawUTF8()).toHexString(), 16);
        return signedHash == expected;
    }

    double sr = 48000.0, activeSamples = 0.0, phase = 0.0;
    std::uint32_t rng = 0x6d2b79f5u;
    bool expired = false;
    bool licensed = false;
    juce::String product;
    static constexpr double kBurstSeconds = 0.09;
    static constexpr double kCycleSeconds = 1.50;
};
