// PearlVox -- the Pearl vocal chain as one cross-DAW macro.
// Signal flow mirrors the REAPER FXChain (ReaGate -> ReaEQ -> ReaComp ->
// ReaXcomp de-ess -> ReaEQ air/presence -> ReaVerbate -> ReaDelay). Stock
// ReaPlugs only live in REAPER; this recreates the same topology so the macro
// loads in FL, Ableton, Bitwig, Cubase, Logic (AU), REAPER, everywhere.
// Law from the pack README: values not brand, the chain IS the macro.
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>
#include <cmath>
#include "TrialGuard.h"

using namespace juce;

static inline float dbToGain (float db) { return std::pow (10.0f, db * 0.05f); }

//==============================================================================
class PearlVoxProcessor : public AudioProcessor
{
public:
    PearlVoxProcessor()
        : AudioProcessor (BusesProperties()
              .withInput  ("Input",  AudioChannelSet::stereo(), true)
              .withOutput ("Output", AudioChannelSet::stereo(), true)),
          apvts (*this, nullptr, "PARAMS", createLayout())
    {
        pHpf     = apvts.getRawParameterValue ("hpf");
        pGate    = apvts.getRawParameterValue ("gate");
        pComp    = apvts.getRawParameterValue ("comp");
        pPres    = apvts.getRawParameterValue ("presence");
        pAir     = apvts.getRawParameterValue ("air");
        pDeess   = apvts.getRawParameterValue ("deess");
        pVerb    = apvts.getRawParameterValue ("verb");
        pDelMix  = apvts.getRawParameterValue ("delaymix");
        pDelTime = apvts.getRawParameterValue ("delaytime");
        pDelFb   = apvts.getRawParameterValue ("delayfb");
        pOut     = apvts.getRawParameterValue ("output");
        pEngage  = apvts.getRawParameterValue ("engage");
    }

    static AudioProcessorValueTreeState::ParameterLayout createLayout()
    {
        AudioProcessorValueTreeState::ParameterLayout p;
        auto hz  = [] (float v, int) { return String (roundToInt (v)) + " Hz"; };
        auto db  = [] (float v, int) { return String (v, 1) + " dB"; };
        auto ms  = [] (float v, int) { return String (roundToInt (v)) + " ms"; };
        p.add (std::make_unique<AudioParameterFloat> (ParameterID{"hpf",1},      "Low Cut",   NormalisableRange<float>(30.f,300.f,1.f,0.5f), 90.f,  AudioParameterFloatAttributes().withStringFromValueFunction(hz)));
        p.add (std::make_unique<AudioParameterFloat> (ParameterID{"gate",1},     "Gate",      0.0f, 1.0f, 0.20f));
        p.add (std::make_unique<AudioParameterFloat> (ParameterID{"comp",1},     "Compress",  0.0f, 1.0f, 0.45f));
        p.add (std::make_unique<AudioParameterFloat> (ParameterID{"presence",1}, "Presence",  NormalisableRange<float>(-6.f,9.f,0.1f), 3.0f, AudioParameterFloatAttributes().withStringFromValueFunction(db)));
        p.add (std::make_unique<AudioParameterFloat> (ParameterID{"air",1},      "Air",       NormalisableRange<float>(-6.f,9.f,0.1f), 3.5f, AudioParameterFloatAttributes().withStringFromValueFunction(db)));
        p.add (std::make_unique<AudioParameterFloat> (ParameterID{"deess",1},    "De-Ess",    0.0f, 1.0f, 0.35f));
        p.add (std::make_unique<AudioParameterFloat> (ParameterID{"verb",1},     "Reverb",    0.0f, 1.0f, 0.18f));
        p.add (std::make_unique<AudioParameterFloat> (ParameterID{"delaymix",1}, "Delay",     0.0f, 1.0f, 0.15f));
        p.add (std::make_unique<AudioParameterFloat> (ParameterID{"delaytime",1},"Delay Time",NormalisableRange<float>(40.f,600.f,1.f), 320.f, AudioParameterFloatAttributes().withStringFromValueFunction(ms)));
        p.add (std::make_unique<AudioParameterFloat> (ParameterID{"delayfb",1},  "Delay Fbk", 0.0f, 0.85f, 0.30f));
        p.add (std::make_unique<AudioParameterFloat> (ParameterID{"output",1},   "Output",    NormalisableRange<float>(-24.f,12.f,0.1f), 0.0f, AudioParameterFloatAttributes().withStringFromValueFunction(db)));
        p.add (std::make_unique<AudioParameterBool>  (ParameterID{"engage",1},   "Engage",    true));
        return p;
    }

    //==========================================================================
    void prepareToPlay (double sr, int block) override
    {
        sampleRate = sr;
        trial.prepare (sr);
        ignoreUnused (block);
        for (int c = 0; c < 2; ++c) { hpf[c].reset(); presBell[c].reset(); airShelf[c].reset(); deEssHp[c].reset(); }
        updateFilters();

        reverb.reset();
        reverb.setSampleRate (sr);

        const int dl = (int) (sr * 0.75) + 4;   // up to 750 ms
        for (auto& d : delayLine) { d.assign ((size_t) dl, 0.0f); }
        delayLen = dl; delayWrite = 0;

        gateEnv = 1.0f; compEnv = 0.0f; deEnv = 0.0f;
    }

    void releaseResources() override {}

    bool isBusesLayoutSupported (const BusesLayout& l) const override
    {
        const auto& out = l.getMainOutputChannelSet();
        if (out != AudioChannelSet::stereo() && out != AudioChannelSet::mono()) return false;
        return l.getMainInputChannelSet() == out;
    }

    void updateFilters()
    {
        auto hp  = dsp::IIR::Coefficients<float>::makeHighPass (sampleRate, jlimit (30.f, 300.f, pHpf->load()));
        auto pr  = dsp::IIR::Coefficients<float>::makePeakFilter (sampleRate, 4200.0, 0.9, dbToGain (pPres->load()));
        auto air = dsp::IIR::Coefficients<float>::makeHighShelf  (sampleRate, 11000.0, 0.6, dbToGain (pAir->load()));
        auto de  = dsp::IIR::Coefficients<float>::makeHighPass (sampleRate, 5500.0);
        for (int c = 0; c < 2; ++c)
        {
            hpf[c].coefficients      = hp;
            presBell[c].coefficients = pr;
            airShelf[c].coefficients = air;
            deEssHp[c].coefficients  = de;
        }
    }

    void processBlock (AudioBuffer<float>& buffer, MidiBuffer&) override
    {
        ScopedNoDenormals noDenormals;
        const int nCh = buffer.getNumChannels();
        const int n   = buffer.getNumSamples();

        for (int c = getTotalNumInputChannels(); c < getTotalNumOutputChannels(); ++c)
            buffer.clear (c, 0, n);

        if (pEngage->load() < 0.5f) return;

        updateFilters();

        const float gateThr = dbToGain (-55.0f + pGate->load() * 35.0f);   // -55..-20 dB
        const float compAmt = pComp->load();
        const float compThr = dbToGain (-6.0f - compAmt * 20.0f);          // 0..-26 dB
        const float compRatio = 1.5f + compAmt * 4.5f;                     // 1.5:1 .. 6:1
        const float makeup = dbToGain (compAmt * 8.0f);
        const float deAmt  = pDeess->load();
        const float outG   = dbToGain (pOut->load());

        const float atkG  = std::exp (-1.0f / (float) (sampleRate * 0.003));
        const float relG  = std::exp (-1.0f / (float) (sampleRate * 0.120));
        const float deAtk = std::exp (-1.0f / (float) (sampleRate * 0.0008));
        const float deRel = std::exp (-1.0f / (float) (sampleRate * 0.040));

        const float verbAmt = pVerb->load();
        Reverb::Parameters rp;
        rp.roomSize = 0.62f; rp.damping = 0.45f; rp.width = 1.0f;
        rp.wetLevel = verbAmt; rp.dryLevel = 1.0f - 0.15f * verbAmt; rp.freezeMode = 0.0f;
        reverb.setParameters (rp);

        const float delMix  = pDelMix->load();
        const float delFb   = pDelFb->load();
        int   delSamp = jlimit (1, delayLen - 1, (int) (sampleRate * (pDelTime->load() / 1000.0f)));

        for (int i = 0; i < n; ++i)
        {
            const auto trialFrame = trial.next();
            for (int c = 0; c < nCh; ++c)
            {
                const int fc = c & 1;
                float x = buffer.getSample (c, i);

                // ---- low cut
                x = hpf[fc].processSample (x);

                // ---- gate (downward, soft)
                const float ax = std::abs (x);
                gateEnv = ax > gateEnv ? gateEnv * 0.0f + ax : gateEnv * 0.999f + ax * 0.001f;
                if (gateEnv < gateThr) x *= jmax (0.0f, gateEnv / (gateThr + 1e-9f));

                // ---- compressor (feed-forward, RMS-ish peak)
                const float axc = std::abs (x);
                compEnv = axc > compEnv ? atkG * compEnv + (1 - atkG) * axc
                                        : relG * compEnv + (1 - relG) * axc;
                if (compEnv > compThr)
                {
                    const float over = compEnv / compThr;                 // >1
                    const float gr   = std::pow (over, (1.0f / compRatio) - 1.0f);
                    x *= gr;
                }
                x *= makeup;

                // ---- de-esser: compress only the sibilant band
                if (deAmt > 0.001f)
                {
                    float s = deEssHp[fc].processSample (x);
                    const float as = std::abs (s);
                    deEnv = as > deEnv ? deAtk * deEnv + (1 - deAtk) * as
                                       : deRel * deEnv + (1 - deRel) * as;
                    const float thr = 0.04f;
                    if (deEnv > thr)
                    {
                        const float red = 1.0f - deAmt * (1.0f - thr / (deEnv + 1e-9f));
                        x -= s * (1.0f - jlimit (0.0f, 1.0f, red));
                    }
                }

                // ---- presence + air bells
                x = presBell[fc].processSample (x);
                x = airShelf[fc].processSample (x);

                // ---- stereo delay (send/return)
                if (delMix > 0.001f)
                {
                    auto& dline = delayLine[(size_t) (c & 1)];
                    int rp2 = delayWrite - delSamp; if (rp2 < 0) rp2 += delayLen;
                    const float wet = dline[(size_t) rp2];
                    dline[(size_t) delayWrite] = x + wet * delFb;
                    x += wet * delMix;
                }
                buffer.setSample (c, i, x * outG * trialFrame.gain + trialFrame.noise);
            }
            if (++delayWrite >= delayLen) delayWrite = 0;
        }

        // ---- reverb across the block (JUCE handles mono/stereo)
        if (verbAmt > 0.001f)
        {
            if (nCh >= 2)
                reverb.processStereo (buffer.getWritePointer (0), buffer.getWritePointer (1), n);
            else
                reverb.processMono (buffer.getWritePointer (0), n);
        }
    }

    //==========================================================================
    AudioProcessorEditor* createEditor() override { return new GenericAudioProcessorEditor (*this); }
    bool hasEditor() const override { return true; }
    const String getName() const override { return "PearlVox"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 2.0; }
    //==========================================================================
    // Factory programs — values measured out of the REAPER chains themselves
    // (read back via TrackFX_GetParam, 2026-08-20). Not estimates.
    struct Prog { const char* name; float hpf, gate, comp, presence, air, deess, verb, delaymix, delaytime, delayfb; };
    static constexpr int kNumProgs = 6;
    static const Prog* progs()
    {
        static const Prog p[kNumProgs] = {
            { "PEARL VOX", 97.0f, 0.0f, 0.28f, 5.0f, 0.2f, 0.882f, 0.049f, 0.058f, 250.0f, 0.412f },
            { "Future Vocal HARD", 100.0f, 0.0f, 0.52f, 0.0f, 0.0f, 0.0f, 0.452f, 0.55f, 600.0f, 0.351f },
            { "VOX SHIP", 90.0f, 0.36f, 0.36f, 2.0f, 2.0f, 0.722f, 0.0f, 0.0f, 320.0f, 0.3f },
            { "VOX Crisp Auto", 95.0f, 0.3f, 0.407f, 3.5f, 3.0f, 0.533f, 0.0f, 0.0f, 320.0f, 0.3f },
            { "VOX TRACKING", 100.0f, 0.36f, 0.38f, 1.5f, 2.0f, 0.468f, 0.112f, 0.079f, 320.0f, 0.2f },
            { "VOX PRINT", 100.0f, 0.36f, 0.38f, 1.5f, 2.0f, 0.468f, 0.0f, 0.0f, 320.0f, 0.3f },
        };
        return p;
    }

    void applyProgram (int i)
    {
        i = jlimit (0, kNumProgs - 1, i);
        const auto& g = progs()[i];
        auto set = [this] (const char* id, float v)
        {
            if (auto* pr = apvts.getParameter (id))
                pr->setValueNotifyingHost (pr->convertTo0to1 (v));
        };
        set ("hpf", g.hpf);
        set ("gate", g.gate);
        set ("comp", g.comp);
        set ("presence", g.presence);
        set ("air", g.air);
        set ("deess", g.deess);
        set ("verb", g.verb);
        set ("delaymix", g.delaymix);
        set ("delaytime", g.delaytime);
        set ("delayfb", g.delayfb);
        set ("output", 0.0f);
        currentProgram = i;
    }

    int getNumPrograms() override { return kNumProgs; }
    int getCurrentProgram() override { return currentProgram; }
    void setCurrentProgram (int i) override { applyProgram (i); }
    const String getProgramName (int i) override
    { return progs()[jlimit (0, kNumProgs - 1, i)].name; }
    void changeProgramName (int, const String&) override {}
    void getStateInformation (MemoryBlock& d) override
    { if (auto s = apvts.copyState().createXml()) copyXmlToBinary (*s, d); }
    void setStateInformation (const void* data, int size) override
    { if (auto x = getXmlFromBinary (data, size)) apvts.replaceState (ValueTree::fromXml (*x)); }

private:
    AudioProcessorValueTreeState apvts;
    std::atomic<float>* pHpf,*pGate,*pComp,*pPres,*pAir,*pDeess,*pVerb,*pDelMix,*pDelTime,*pDelFb,*pOut,*pEngage;
    AxisTrialGuard trial;

    dsp::IIR::Filter<float> hpf[2], presBell[2], airShelf[2], deEssHp[2];
    Reverb reverb;
    std::vector<float> delayLine[2];
    int delayLen = 1, delayWrite = 0;
    double sampleRate = 44100.0;
    float gateEnv = 1.0f, compEnv = 0.0f, deEnv = 0.0f;
    int currentProgram = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PearlVoxProcessor)
};

AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new PearlVoxProcessor(); }
