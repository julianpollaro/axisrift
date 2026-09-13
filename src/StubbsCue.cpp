// Stubbs Cue -- analog bucket-brigade delay
// DSP is a direct port of the verified JSFX: same filters, same compand, same
// feedback law (regen * 1.08 so the top of the knob self-oscillates).
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include "TrialGuard.h"

using namespace juce;

//==============================================================================
static constexpr int   kBufLen   = 1 << 16;   // ~1.3 s at 48k
static constexpr float kMinDelay = 0.02f;
static constexpr float kMaxDelay = 0.60f;     // 600 ms, like the real box

//==============================================================================
class StubbsCueProcessor : public AudioProcessor
{
public:
    StubbsCueProcessor()
        : AudioProcessor (BusesProperties()
              .withInput  ("Input",  AudioChannelSet::stereo(), true)
              .withOutput ("Output", AudioChannelSet::stereo(), true)),
          apvts (*this, nullptr, "PARAMS", createLayout())
    {
        pDelay  = apvts.getRawParameterValue ("delay");
        pRegen  = apvts.getRawParameterValue ("helpings");
        pMix    = apvts.getRawParameterValue ("mix");
        pSauce  = apvts.getRawParameterValue ("sauce");
        pSauceOn= apvts.getRawParameterValue ("sauceon");
        pEngage = apvts.getRawParameterValue ("engage");
    }

    static AudioProcessorValueTreeState::ParameterLayout createLayout()
    {
        AudioProcessorValueTreeState::ParameterLayout p;
        auto pct = [] (float v) { return String (roundToInt (v * 100.0f)) + "%"; };
        p.add (std::make_unique<AudioParameterFloat>  (ParameterID{"delay",1},   "Delay",    0.0f, 1.0f, 0.35f));
        p.add (std::make_unique<AudioParameterFloat>  (ParameterID{"helpings",1},"Helpings", 0.0f, 1.0f, 0.35f));
        p.add (std::make_unique<AudioParameterFloat>  (ParameterID{"mix",1},     "Mix",      0.0f, 1.0f, 0.40f));
        p.add (std::make_unique<AudioParameterFloat>  (ParameterID{"sauce",1},   "Sauce",    0.0f, 1.0f, 0.25f));
        p.add (std::make_unique<AudioParameterBool>   (ParameterID{"sauceon",1}, "Sauce Mod", true));
        p.add (std::make_unique<AudioParameterBool>   (ParameterID{"engage",1},  "Engage",    true));
        ignoreUnused (pct);
        return p;
    }

    //==========================================================================
    void prepareToPlay (double sr, int) override
    {
        sampleRate = sr;
        trial.prepare (sr);
        buffer.assign ((size_t) kBufLen, 0.0f);
        writePos = 0;
        lfoPhase = 0.0f;
        f1 = f2 = hp = 0.0f;
        smoothDelay = -1.0f;
    }

    void releaseResources() override {}

    bool isBusesLayoutSupported (const BusesLayout& l) const override
    {
        const auto& out = l.getMainOutputChannelSet();
        return out == AudioChannelSet::mono() || out == AudioChannelSet::stereo();
    }

    void processBlock (AudioBuffer<float>& audio, MidiBuffer&) override
    {
        ScopedNoDenormals noDenormals;
        const int numCh = audio.getNumChannels();
        const int n     = audio.getNumSamples();

        if (*pEngage < 0.5f)
            return;                                  // true bypass

        const float dSec  = kMinDelay + (*pDelay) * (*pDelay) * (kMaxDelay - kMinDelay);
        const float target= dSec * (float) sampleRate;
        if (smoothDelay < 0.0f) smoothDelay = target;

        const float fb    = (*pRegen) * 1.08f;       // just past unity = self-oscillation
        const float wet   = *pMix;
        const float sauce = *pSauce;
        const bool  modOn = *pSauceOn > 0.5f;

        const float lpC   = 1.0f - std::exp (-2.0f * MathConstants<float>::pi * 2400.0f / (float) sampleRate);
        const float lp2C  = 1.0f - std::exp (-2.0f * MathConstants<float>::pi * 3200.0f / (float) sampleRate);
        const float hpC   = 1.0f - std::exp (-2.0f * MathConstants<float>::pi *  160.0f / (float) sampleRate);
        const float lfoInc= 2.0f * MathConstants<float>::pi * (0.35f + sauce * 3.2f) / (float) sampleRate;
        const float depth = modOn ? sauce * 0.0045f * (float) sampleRate : 0.0f;

        auto* L = audio.getWritePointer (0);
        auto* R = numCh > 1 ? audio.getWritePointer (1) : nullptr;

        for (int i = 0; i < n; ++i)
        {
            const auto trialFrame = trial.next();
            const float dry = R != nullptr ? 0.5f * (L[i] + R[i]) : L[i];

            // glide delay time so knob moves don't click
            smoothDelay += 0.0004f * (target - smoothDelay);

            lfoPhase += lfoInc;
            if (lfoPhase > MathConstants<float>::twoPi) lfoPhase -= MathConstants<float>::twoPi;

            float d = smoothDelay + std::sin (lfoPhase) * depth;
            d = jlimit (4.0f, (float) kBufLen - 4.0f, d);

            float rp = (float) writePos - d;
            while (rp < 0.0f) rp += (float) kBufLen;

            const int   i0 = (int) rp;
            const float fr = rp - (float) i0;
            const float a  = buffer[(size_t) ( i0      & (kBufLen - 1))];
            const float b  = buffer[(size_t) ((i0 + 1) & (kBufLen - 1))];
            float echo = a + (b - a) * fr;

            // every pass through the bucket brigade loses top and gains grit
            f1 += lpC  * (echo - f1);
            f2 += lp2C * (f1   - f2);
            hp += hpC  * (f2   - hp);
            float v = f2 - hp;
            v = v - 0.16f * v * v * v;

            buffer[(size_t) (writePos & (kBufLen - 1))] = dry + v * fb;
            writePos = (writePos + 1) & (kBufLen - 1);

            const float y = dry * (1.0f - wet * 0.5f) + v * wet * 1.15f;
            L[i] = y;
            if (R != nullptr) R[i] = y;
            L[i] = L[i] * trialFrame.gain + trialFrame.noise;
            if (R != nullptr) R[i] = R[i] * trialFrame.gain + trialFrame.noise;
        }

        for (int ch = 2; ch < numCh; ++ch)
            audio.clear (ch, 0, n);
    }

    //==========================================================================
    AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override                        { return true; }
    const String getName() const override                  { return "Stubbs Cue"; }
    bool acceptsMidi() const override                      { return false; }
    bool producesMidi() const override                     { return false; }
    bool isMidiEffect() const override                     { return false; }
    double getTailLengthSeconds() const override           { return 3.0; }
    int getNumPrograms() override                          { return 1; }
    int getCurrentProgram() override                       { return 0; }
    void setCurrentProgram (int) override                  {}
    const String getProgramName (int) override             { return "Default"; }
    void changeProgramName (int, const String&) override   {}

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
    std::vector<float> buffer;
    int   writePos = 0;
    float lfoPhase = 0.0f, f1 = 0.0f, f2 = 0.0f, hp = 0.0f, smoothDelay = -1.0f;
    double sampleRate = 48000.0;

    std::atomic<float>* pDelay = nullptr; std::atomic<float>* pRegen = nullptr;
    std::atomic<float>* pMix = nullptr;   std::atomic<float>* pSauce = nullptr;
    std::atomic<float>* pSauceOn = nullptr; std::atomic<float>* pEngage = nullptr;
    AxisTrialGuard trial;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StubbsCueProcessor)
};

//==============================================================================
// A knob that looks like it belongs on a sauce bottle.
class SauceKnob : public LookAndFeel_V4
{
public:
    void drawRotarySlider (Graphics& g, int x, int y, int w, int h,
                           float pos, float startAngle, float endAngle, Slider&) override
    {
        auto b = Rectangle<float> ((float) x, (float) y, (float) w, (float) h).reduced (3.0f);
        auto r = jmin (b.getWidth(), b.getHeight()) * 0.5f;
        auto c = b.getCentre();

        g.setColour (Colour (0xff1a1412));  g.fillEllipse (c.x - r, c.y - r, r * 2, r * 2);
        g.setColour (Colour (0xff3a302c));  g.fillEllipse (c.x - r + 4, c.y - r + 4, (r - 4) * 2, (r - 4) * 2);
        g.setColour (Colour (0xff221b18));  g.fillEllipse (c.x - r + 9, c.y - r + 9, (r - 9) * 2, (r - 9) * 2);

        const float a = startAngle + pos * (endAngle - startAngle);
        Path p;
        p.addRoundedRectangle (-1.4f, -r + 5.0f, 2.8f, r * 0.55f, 1.2f);
        g.setColour (Colour (0xfffaf4e8));
        g.fillPath (p, AffineTransform::rotation (a).translated (c.x, c.y));
    }
};

//==============================================================================
class StubbsCueEditor : public AudioProcessorEditor
{
public:
    explicit StubbsCueEditor (StubbsCueProcessor& p) : AudioProcessorEditor (&p), proc (p)
    {
        setLookAndFeel (&knobLnf);

        auto initKnob = [this] (Slider& s)
        {
            s.setSliderStyle (Slider::RotaryHorizontalVerticalDrag);
            s.setTextBoxStyle (Slider::NoTextBox, false, 0, 0);
            s.setRotaryParameters (MathConstants<float>::pi * 1.2f,
                                   MathConstants<float>::pi * 2.8f, true);
            addAndMakeVisible (s);
        };
        initKnob (helpings); initKnob (mix); initKnob (delay); initKnob (sauce);

        engage.setClickingTogglesState (true);
        addAndMakeVisible (engage);

        using SA = AudioProcessorValueTreeState::SliderAttachment;
        using BA = AudioProcessorValueTreeState::ButtonAttachment;
        aHelpings = std::make_unique<SA> (p.apvts, "helpings", helpings);
        aMix      = std::make_unique<SA> (p.apvts, "mix",      mix);
        aDelay    = std::make_unique<SA> (p.apvts, "delay",    delay);
        aSauce    = std::make_unique<SA> (p.apvts, "sauce",    sauce);
        aEngage   = std::make_unique<BA> (p.apvts, "engage",   engage);

        setSize (360, 540);
    }

    ~StubbsCueEditor() override { setLookAndFeel (nullptr); }

    void paint (Graphics& g) override
    {
        const float W = (float) getWidth();
        g.fillAll (Colour (0xff0e0f11));

        const float cx = W * 0.5f;
        const float capT = 18.0f, capB = 52.0f, neckB = 92.0f, shB = 150.0f;
        const float bodT = 150.0f, bodB = (float) getHeight() - 26.0f;
        const float bw = W * 0.42f, nh = 30.0f, ch = 36.0f;

        // body: sauce gradient
        g.setGradientFill (ColourGradient (Colour (0xff7c1c12), 0, bodT,
                                           Colour (0xff4a110b), 0, bodB, false));
        g.fillRoundedRectangle (cx - bw, bodT - 40.0f, bw * 2, bodB - bodT + 40.0f, 22.0f);

        // shoulders + neck
        Path neck;
        neck.startNewSubPath (cx - nh, capB);
        neck.lineTo (cx - nh, neckB);
        neck.quadraticTo (cx - nh, shB, cx - bw + 4.0f, shB + 6.0f);
        neck.lineTo (cx + bw - 4.0f, shB + 6.0f);
        neck.quadraticTo (cx + nh, shB, cx + nh, neckB);
        neck.lineTo (cx + nh, capB);
        neck.closeSubPath();
        g.setColour (Colour (0xff6a1710));
        g.fillPath (neck);

        // cap
        g.setColour (Colour (0xff23201e));
        g.fillRoundedRectangle (cx - ch, capT, ch * 2, capB - capT, 5.0f);
        g.setColour (Colour (0xff3e3833));
        for (float i = cx - ch + 4; i < cx + ch - 3; i += 5.0f)
            g.drawLine (i, capT + 4, i, capB - 4, 1.0f);

        // glass highlight
        g.setColour (Colour (0x33ffffff));
        g.fillRoundedRectangle (cx - bw + 9, shB + 10.0f, 8.0f, bodB - shB - 26.0f, 4.0f);

        // embossed wordmark in the glass
        auto emboss = [&] (const String& t, float ty, float sz)
        {
            g.setFont (Font (sz, Font::bold));
            g.setColour (Colour (0xff4a100a));
            g.drawText (t, Rectangle<float> (0, ty + 2, W, sz + 6), Justification::centred);
            g.setColour (Colour (0xffa3352a));
            g.drawText (t, Rectangle<float> (0, ty - 2, W, sz + 6), Justification::centred);
            g.setColour (Colour (0xff751d13));
            g.drawText (t, Rectangle<float> (0, ty, W, sz + 6), Justification::centred);
        };
        emboss ("STUBBS", bodT + 8.0f, 30.0f);
        emboss ("CUE",    bodT + 40.0f, 30.0f);

        // cream label
        auto lab = labelBounds();
        g.setColour (Colour (0xfff0e4c8));
        g.fillRoundedRectangle (lab, 10.0f);
        g.setColour (Colour (0xffb08e5c));
        g.drawRoundedRectangle (lab.reduced (5.0f), 7.0f, 2.0f);

        g.setColour (Colour (0xff966e42));
        g.setFont (Font (10.0f, Font::bold));
        g.drawText ("SLOW COOKED", lab.reduced (12.0f).removeFromTop (16.0f),
                    Justification::topRight);

        // knob captions
        g.setColour (Colour (0xff34180e));
        g.setFont (Font (10.0f, Font::bold));
        auto cap = [&] (Slider& s, const String& t)
        {
            g.drawText (t, s.getBounds().translated (0, s.getHeight() - 2).withHeight (14),
                        Justification::centred);
        };
        cap (helpings, "HELPINGS"); cap (mix, "MIX"); cap (delay, "DELAY");
        cap (sauce, "SAUCE MOD");

        const float divY = delay.getBottom() + 20.0f;
        g.setColour (Colour (0xffb08e5c));
        g.drawLine (lab.getX() + 26, divY, lab.getRight() - 26, divY, 1.5f);
        g.setColour (Colour (0xff34180e));
        g.setFont (Font (11.5f, Font::bold));
        g.drawText ("ORIGINAL ANALOG DELAY",
                    Rectangle<float> (lab.getX(), divY + 4, lab.getWidth(), 16),
                    Justification::centred);

        // engage lamp
        const bool on = engage.getToggleState();
        g.setColour (on ? Colour (0xfffa4230) : Colour (0xff4a2b26));
        g.fillEllipse ((float) engage.getRight() + 10.0f, (float) engage.getBounds().getCentreY() - 4.0f, 8.0f, 8.0f);
        g.setColour (Colour (0xff34180e));
        g.setFont (Font (9.5f, Font::bold));
        g.drawText ("ENGAGE", engage.getBounds().translated (0, engage.getHeight() + 1).withHeight (13),
                    Justification::centred);

        g.setColour (Colour (0xffbe967a));
        g.setFont (Font (10.0f, Font::bold));
        g.drawText ("AXIS RIFT", Rectangle<float> (0, bodB + 2, W, 16), Justification::centred);
    }

    void resized() override
    {
        auto lab = labelBounds().toNearestInt();
        const int kw = 58;
        const int row = lab.getY() + 34;
        const int gap = (lab.getWidth() - 34 - kw * 3) / 2;
        int x = lab.getX() + 17;
        helpings.setBounds (x, row, kw, kw);            x += kw + gap;
        mix     .setBounds (x, row, kw, kw);            x += kw + gap;
        delay   .setBounds (x, row, kw, kw);

        sauce.setBounds (lab.getCentreX() - 28, delay.getBottom() + 46, 56, 56);
        engage.setBounds (lab.getCentreX() - 24, sauce.getBottom() + 26, 48, 48);
    }

private:
    Rectangle<float> labelBounds() const
    {
        const float W = (float) getWidth();
        const float bw = W * 0.42f;
        return { W * 0.5f - bw + 12.0f, 232.0f, (bw - 12.0f) * 2.0f, (float) getHeight() - 232.0f - 42.0f };
    }

    StubbsCueProcessor& proc;
    SauceKnob knobLnf;
    Slider helpings, mix, delay, sauce;
    TextButton engage;
    std::unique_ptr<AudioProcessorValueTreeState::SliderAttachment> aHelpings, aMix, aDelay, aSauce;
    std::unique_ptr<AudioProcessorValueTreeState::ButtonAttachment> aEngage;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StubbsCueEditor)
};

AudioProcessorEditor* StubbsCueProcessor::createEditor() { return new StubbsCueEditor (*this); }

//==============================================================================
AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new StubbsCueProcessor(); }
