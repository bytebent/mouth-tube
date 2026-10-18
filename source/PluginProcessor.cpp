#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace mt = bytebent::mouthtube;

namespace
{
using APF = juce::AudioParameterFloat;
using Attr = juce::AudioParameterFloatAttributes;

// Length is logarithmic, matching ui/index.html: n = log(v / min) / log(max / min)
juce::NormalisableRange<float> logRange (float min, float max)
{
    return { min, max,
             [] (float a, float b, float n) { return a * std::pow (b / a, n); },
             [] (float a, float b, float v) { return juce::jlimit (0.0f, 1.0f, std::log (v / a) / std::log (b / a)); },
             [] (float, float, float v) { return v; } };
}

juce::String pct (float v, int) { return juce::String (juce::roundToInt (v * 100.0f)) + " %"; }

const mt::ParamInfo& info (mt::Param p) { return mt::paramInfo (p); }
juce::ParameterID pid (mt::Param p) { return { info (p).id, 1 }; }
} // namespace

juce::AudioProcessorValueTreeState::ParameterLayout MouthTubeProcessor::createLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout l;
    auto unit = [&] (mt::Param p)
    {
        l.add (std::make_unique<APF> (pid (p), info (p).name, juce::NormalisableRange<float> (0.0f, 1.0f),
                                      (float) info (p).def, Attr().withStringFromValueFunction (pct)));
    };
    for (auto p : { mt::pTongueX, mt::pTongueY, mt::pTip, mt::pJaw, mt::pLips, mt::pRound, mt::pNasal })
        unit (p);
    l.add (std::make_unique<APF> (pid (mt::pLength), info (mt::pLength).name, logRange ((float) mt::LEN_MIN, (float) mt::LEN_MAX),
        (float) info (mt::pLength).def,
        Attr().withLabel ("cm").withStringFromValueFunction ([] (float v, int) { return juce::String (v, 1) + " cm"; })));
    unit (mt::pDamping);
    unit (mt::pSpread);
    l.add (std::make_unique<juce::AudioParameterChoice> (pid (mt::pRadiation), info (mt::pRadiation).name, juce::StringArray { "Off", "On" }, 1));
    unit (mt::pMix);
    l.add (std::make_unique<APF> (pid (mt::pOutput), info (mt::pOutput).name, juce::NormalisableRange<float> (-24.0f, 12.0f), 0.0f,
        Attr().withLabel ("dB").withStringFromValueFunction ([] (float v, int) { return juce::String (v, 1) + " dB"; })));
    return l;
}

MouthTubeProcessor::MouthTubeProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "STATE", createLayout())
{
    for (int i = 0; i < mt::kNumParams; ++i)
        raw[(size_t) i] = apvts.getRawParameterValue (mt::paramInfo (i).id);
}

// mono → mono, mono → stereo, stereo → stereo
bool MouthTubeProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto in = layouts.getMainInputChannelSet(), out = layouts.getMainOutputChannelSet();
    const bool inOk = in == juce::AudioChannelSet::mono() || in == juce::AudioChannelSet::stereo();
    const bool outOk = out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo();
    return inOk && outOk && in.size() <= out.size();
}

void MouthTubeProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;
    dsp.prepare (sampleRate);
    pushParams();
    dsp.snap();   // start at the current settings instead of gliding from the defaults
    scratchR.assign ((size_t) std::max (samplesPerBlock, 1024), 0.0f);
    setLatencySamples (dsp.latency());
}

void MouthTubeProcessor::pushParams()
{
    // the choice parameter arrives as its index (0 Off, 1 On), which is what the engine expects
    for (int i = 0; i < mt::kNumParams; ++i)
        dsp.setParam (i, raw[(size_t) i]->load (std::memory_order_relaxed));
}

void MouthTubeProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    const int inCh = getTotalNumInputChannels(), outCh = getTotalNumOutputChannels();
    if (outCh <= 0 || n <= 0) return;
    for (int c = 2; c < buffer.getNumChannels(); ++c)
        buffer.clear (c, 0, n);

    pushParams();

    const float* inL = buffer.getReadPointer (0);
    const float* inR = inCh > 1 ? buffer.getReadPointer (1) : nullptr;
    float* outL = buffer.getWritePointer (0);
    const bool stereoOut = outCh > 1 && buffer.getNumChannels() > 1;
    float* outR = stereoOut ? buffer.getWritePointer (1) : nullptr;

    // the engine handles any length; with a mono output the right channel goes to scratch, in chunks
    for (int pos = 0; pos < n;)
    {
        const int m = stereoOut ? n - pos : std::min (n - pos, (int) scratchR.size());
        dsp.process (inL + pos, inR != nullptr ? inR + pos : nullptr, outL + pos, stereoOut ? outR + pos : scratchR.data(), m);
        pos += m;
    }

    if (editorOpen.load (std::memory_order_relaxed))
        publishView (outL, n);
}

void MouthTubeProcessor::publishView (const float* out, int n)
{
    auto& v = view;
    for (int i = 0; i < MouthTubeView::N; ++i)
        v.d[(size_t) i].store (dsp.vizD[i], std::memory_order_relaxed);
    v.nasal.store ((float) dsp.vizNasal, std::memory_order_relaxed);
    v.len.store ((float) dsp.vizLen, std::memory_order_relaxed);
    int p = v.ringPos.load (std::memory_order_relaxed);
    for (int i = 0; i < n; ++i)
    {
        v.ring[(size_t) p].store (out[i], std::memory_order_relaxed);
        p = (p + 1) % MouthTubeView::RING;
    }
    v.ringPos.store (p, std::memory_order_release);
}

juce::AudioProcessorEditor* MouthTubeProcessor::createEditor()
{
    return new MouthTubeEditor (*this);
}

void MouthTubeProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void MouthTubeProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
        {
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
            stateRestored = true;
        }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new MouthTubeProcessor();
}
