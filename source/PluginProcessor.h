#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "MouthTubeDSP.h"
#include "PresetManager.h"
#include <array>
#include <atomic>
#include <vector>

// Live view data, written by the audio thread and read by the editor (relaxed atomics: a torn
// frame only means one slightly odd picture, never a crash or a lock).
struct MouthTubeView
{
    static constexpr int N = bytebent::mouthtube::N_ORAL, RING = 2048;
    std::array<std::atomic<float>, N> d {};
    std::atomic<float> nasal { 0 }, len { 0.17f };
    std::array<std::atomic<float>, RING> ring {};
    std::atomic<int> ringPos { 0 };
};

class MouthTubeProcessor : public juce::AudioProcessor
{
public:
    MouthTubeProcessor();

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "mouth-tube"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.5; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState apvts;
    PresetManager presets { apvts };              // message thread only
    std::atomic<bool> stateRestored { false };    // set when the host loads a project; the editor re-snapshots
    MouthTubeView view;
    std::atomic<bool> editorOpen { false };
    std::atomic<double> currentSampleRate { 48000.0 };

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    void pushParams();
    void publishView (const float* out, int n);

    bytebent::mouthtube::Engine dsp;
    std::array<std::atomic<float>*, bytebent::mouthtube::kNumParams> raw {};
    std::vector<float> scratchR;                  // right output when the host gives us a mono output

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MouthTubeProcessor)
};
