// Auto Quacker — envelope / strum-thrown treadle wah
// Port of verified JSFX: dual SVF bandpass, beat-from-gap, ceiling latch.
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <cmath>
#include "TrialGuard.h"

using namespace juce;

class AutoQuackerProcessor : public AudioProcessor
{
public:
    AutoQuackerProcessor()
        : AudioProcessor (BusesProperties()
              .withInput  ("Input",  AudioChannelSet::stereo(), true)
              .withOutput ("Output", AudioChannelSet::stereo(), true)),
          apvts (*this, nullptr, "PARAMS", createLayout())
    {
        pSpeed  = apvts.getRawParameterValue ("speed");
        pVocal  = apvts.getRawParameterValue ("vocal");
        pLvl    = apvts.getRawParameterValue ("lvl");
        pAuto   = apvts.getRawParameterValue ("autoq");
        pEngage = apvts.getRawParameterValue ("engage");
        pPark   = apvts.getRawParameterValue ("park");
        pCeil   = apvts.getRawParameterValue ("ceil");
    }

    static AudioProcessorValueTreeState::ParameterLayout createLayout()
    {
        AudioProcessorValueTreeState::ParameterLayout p;
        p.add (std::make_unique<AudioParameterFloat> (ParameterID{"speed",1},  "Speed",  0.0f, 1.0f, 0.5f));
        p.add (std::make_unique<AudioParameterFloat> (ParameterID{"park",1},   "Park",   0.0f, 1.0f, 0.5f));
        p.add (std::make_unique<AudioParameterFloat> (ParameterID{"vocal",1},  "Vocal",  0.0f, 1.0f, 0.7f));
        p.add (std::make_unique<AudioParameterFloat> (ParameterID{"lvl",1},    "Level",  0.0f, 1.0f, 0.5f));
        p.add (std::make_unique<AudioParameterBool>  (ParameterID{"autoq",1},  "Auto",   true));
        p.add (std::make_unique<AudioParameterBool>  (ParameterID{"engage",1}, "Engage", true));
        p.add (std::make_unique<AudioParameterBool>  (ParameterID{"ceil",1},   "Ceiling",true));
        return p;
    }

    void prepareToPlay (double sr, int) override
    {
        sampleRate = sr;
        trial.prepare (sr);
        pos = ramp = envF = envS = 0.0f;
        hold = gap = beat = 0.0f;
        armed = true;
        lp = bp = lp2 = bp2 = 0.0f;
        af = 1.0f - std::exp (-2.0f * MathConstants<float>::pi * 22.0f / (float) sr);
        as_ = 1.0f - std::exp (-2.0f * MathConstants<float>::pi * 2.2f / (float) sr);
    }

    void releaseResources() override {}

    bool isBusesLayoutSupported (const BusesLayout& l) const override
    {
        auto o = l.getMainOutputChannelSet();
        return o == AudioChannelSet::mono() || o == AudioChannelSet::stereo();
    }

    void processBlock (AudioBuffer<float>& audio, MidiBuffer&) override
    {
        ScopedNoDenormals nd;
        const int nCh = audio.getNumChannels();
        const int n   = audio.getNumSamples();
        if (*pEngage < 0.5f) return;

        const float scale = 0.25f + *pSpeed * 1.45f;
        const float thr   = 0.022f;
        const float q     = 0.55f - *pVocal * 0.44f;
        const float outG  = 0.4f + (*pLvl) * (*pLvl) * 2.6f;
        const bool  autoQ = *pAuto > 0.5f;
        const float park  = *pPark;
        const float FLO = 330.0f, FHI = 2300.0f;

        auto* L = audio.getWritePointer (0);
        auto* R = nCh > 1 ? audio.getWritePointer (1) : nullptr;

        for (int i = 0; i < n; ++i)
        {
            const auto trialFrame = trial.next();
            const float dd = R ? 0.5f * (L[i] + R[i]) : L[i];
            const float a  = std::abs (dd);
            envF += af * (a - envF);
            envS += as_ * (a - envS);

            if (autoQ)
            {
                if (hold > 0.0f) hold -= 1.0f;
                gap += 1.0f;

                if (hold <= 0.0f && envF < envS * 1.02f + thr * 0.35f)
                    armed = true;

                if (armed && hold <= 0.0f && envF > envS + thr && envF > 0.004f)
                {
                    if (gap < sampleRate * 2.5 && gap > sampleRate * 0.03)
                        beat = beat <= 0.0f ? gap : beat + 0.5f * (gap - beat);
                    gap = 0.0f; ramp = 0.0f; hold = (float) sampleRate * 0.035f; armed = false;
                }
                if (gap > sampleRate * 2.5)
                    beat += ((float) sampleRate * 0.6f - beat) * 0.00002f;

                // ceiling latch: fast playing pins toe-down (simplified vs full tap)
                bool capped = *pCeil > 0.5f && beat > 0.0f && beat < (float) sampleRate * 0.12f;
                if (capped)
                    pos += (1.0f - pos) * 0.004f;
                else
                {
                    float bT = jlimit ((float) sampleRate * 0.06f, (float) sampleRate * 1.8f,
                                       (beat <= 0.0f ? (float) sampleRate * 0.5f : beat) * scale);
                    if (ramp < 1.0f) ramp += 1.0f / bT;
                    float p = jmin (1.0f, ramp);
                    p = p * p * (3.0f - 2.0f * p);
                    pos = p;
                }
            }
            else
            {
                pos += 0.004f * (park - pos);
            }

            const float fc = FLO * std::exp (std::log (FHI / FLO) * pos);
            const float ff = 2.0f * std::sin (MathConstants<float>::pi *
                             jmin (fc, (float) sampleRate * 0.24f) / (float) sampleRate);

            lp  += ff * bp;  float hp1 = dd - lp - q * bp;  bp  += ff * hp1;
            lp2 += ff * bp2; float hp2 = bp - lp2 - q * bp2; bp2 += ff * hp2;

            float y = bp2 * (1.6f + *pVocal * 2.2f) * outG;
            y = y / (1.0f + std::abs (y) * 0.22f);
            L[i] = y;
            if (R) R[i] = y;
            L[i] = L[i] * trialFrame.gain + trialFrame.noise;
            if (R) R[i] = R[i] * trialFrame.gain + trialFrame.noise;
        }
        for (int ch = 2; ch < nCh; ++ch)
            audio.clear (ch, 0, n);
    }

    AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const String getName() const override { return "Auto Quacker"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const String getProgramName (int) override { return {}; }
    void changeProgramName (int, const String&) override {}
    void getStateInformation (MemoryBlock& dest) override
    {
        if (auto xml = apvts.copyState().createXml())
            copyXmlToBinary (*xml, dest);
    }
    void setStateInformation (const void* data, int size) override
    {
        if (auto xml = getXmlFromBinary (data, size))
            apvts.replaceState (ValueTree::fromXml (*xml));
    }

    AudioProcessorValueTreeState apvts;

private:
    double sampleRate = 48000.0;
    float pos = 0, ramp = 1, envF = 0, envS = 0, hold = 0, gap = 0, beat = 0;
    float lp = 0, bp = 0, lp2 = 0, bp2 = 0, af = 0, as_ = 0;
    bool armed = true;
    std::atomic<float>* pSpeed = nullptr; std::atomic<float>* pVocal = nullptr;
    std::atomic<float>* pLvl = nullptr;   std::atomic<float>* pAuto = nullptr;
    std::atomic<float>* pEngage = nullptr; std::atomic<float>* pPark = nullptr;
    std::atomic<float>* pCeil = nullptr;
    AxisTrialGuard trial;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AutoQuackerProcessor)
};

class AutoQuackerEditor : public AudioProcessorEditor
{
public:
    explicit AutoQuackerEditor (AutoQuackerProcessor& p)
        : AudioProcessorEditor (&p), proc (p)
    {
        auto mk = [this] (Slider& s)
        {
            s.setSliderStyle (Slider::RotaryHorizontalVerticalDrag);
            s.setTextBoxStyle (Slider::TextBoxBelow, false, 56, 16);
            addAndMakeVisible (s);
        };
        mk (speed); mk (vocal); mk (lvl); mk (park);
        engage.setClickingTogglesState (true); autoq.setClickingTogglesState (true);
        engage.setButtonText ("ENGAGE"); autoq.setButtonText ("AUTO");
        addAndMakeVisible (engage); addAndMakeVisible (autoq);
        using SA = AudioProcessorValueTreeState::SliderAttachment;
        using BA = AudioProcessorValueTreeState::ButtonAttachment;
        aSpeed = std::make_unique<SA> (p.apvts, "speed", speed);
        aVocal = std::make_unique<SA> (p.apvts, "vocal", vocal);
        aLvl   = std::make_unique<SA> (p.apvts, "lvl", lvl);
        aPark  = std::make_unique<SA> (p.apvts, "park", park);
        aEng   = std::make_unique<BA> (p.apvts, "engage", engage);
        aAuto  = std::make_unique<BA> (p.apvts, "autoq", autoq);
        setSize (340, 280);
    }

    void paint (Graphics& g) override
    {
        g.fillAll (Colour (0xff0c1210));
        g.setColour (Colour (0xff3fd4b0));
        g.setFont (Font (18.0f, Font::bold));
        g.drawText ("AUTO QUACKER", getLocalBounds().removeFromTop (36), Justification::centred);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (12).withTrimmedTop (40);
        auto row = r.removeFromTop (140);
        int w = row.getWidth() / 4;
        speed.setBounds (row.removeFromLeft (w).reduced (4));
        vocal.setBounds (row.removeFromLeft (w).reduced (4));
        lvl.setBounds (row.removeFromLeft (w).reduced (4));
        park.setBounds (row.reduced (4));
        auto bot = r.removeFromTop (40);
        engage.setBounds (bot.removeFromLeft (bot.getWidth() / 2).reduced (6));
        autoq.setBounds (bot.reduced (6));
    }

private:
    AutoQuackerProcessor& proc;
    Slider speed, vocal, lvl, park;
    TextButton engage, autoq;
    std::unique_ptr<AudioProcessorValueTreeState::SliderAttachment> aSpeed, aVocal, aLvl, aPark;
    std::unique_ptr<AudioProcessorValueTreeState::ButtonAttachment> aEng, aAuto;
};

AudioProcessorEditor* AutoQuackerProcessor::createEditor()
{
    return new AutoQuackerEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new AutoQuackerProcessor();
}
