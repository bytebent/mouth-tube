#pragma once

#include <juce_gui_extra/juce_gui_extra.h>
#include "PluginProcessor.h"
#include <map>
#include <set>

// The whole interface is ui/index.html, shown in a web view. This class only passes parameter
// values, preset actions and live-view frames between it and the processor.
class MouthTubeEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit MouthTubeEditor (MouthTubeProcessor&);
    ~MouthTubeEditor() override;

    void resized() override;
    void paint (juce::Graphics&) override;

    static constexpr int width = 900, height = 600;
    juce::WebBrowserComponent& getBrowser() { return browser; }   // for tests

private:
    void timerCallback() override;
    void onSetParam (const juce::var&);
    void onGesture (const juce::var&);
    void onPresetAction (const juce::var&);
    void sendPresets();
    static std::optional<juce::WebBrowserComponent::Resource> getResource (const juce::String& url);

    MouthTubeProcessor& proc;
    juce::WebBrowserComponent browser;

    std::map<juce::String, float> lastSent;
    std::set<juce::String> openGestures;
    bool needFullSync = true;
    bool lastModified = false;
    juce::String lastPresetKey;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MouthTubeEditor)
};
