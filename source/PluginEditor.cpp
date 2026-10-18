#include "PluginEditor.h"
#include "BinaryData.h"

MouthTubeEditor::MouthTubeEditor (MouthTubeProcessor& p)
    : AudioProcessorEditor (p),
      proc (p),
      browser (juce::WebBrowserComponent::Options {}
                   .withNativeIntegrationEnabled()
                   .withInitialisationData ("version", juce::String (JucePlugin_VersionString))
                   .withResourceProvider ([] (const juce::String& url) { return getResource (url); })
                   .withEventListener ("setParam", [this] (const juce::var& v) { onSetParam (v); })
                   .withEventListener ("gesture", [this] (const juce::var& v) { onGesture (v); })
                   .withEventListener ("presetAction", [this] (const juce::var& v) { onPresetAction (v); })
                   .withEventListener ("uiReady", [this] (const juce::var&) { needFullSync = true; lastPresetKey = {}; }))
{
    addAndMakeVisible (browser);
    browser.goToURL (juce::WebBrowserComponent::getResourceProviderRoot());
    setSize (width, height);
    proc.editorOpen = true;
    if (proc.stateRestored.exchange (false)) proc.presets.snapshot();
    startTimerHz (30);
}

MouthTubeEditor::~MouthTubeEditor()
{
    stopTimer();
    proc.editorOpen = false;
    for (auto& id : openGestures)
        if (auto* param = proc.apvts.getParameter (id))
            param->endChangeGesture();
}

void MouthTubeEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xffdcdce0));   // platinum, shown for a moment while the page loads
}

void MouthTubeEditor::resized()
{
    browser.setBounds (getLocalBounds());
}

std::optional<juce::WebBrowserComponent::Resource> MouthTubeEditor::getResource (const juce::String& url)
{
    const auto path = url.fromFirstOccurrenceOf ("/", false, false).upToFirstOccurrenceOf ("?", false, false);
    if (path.isEmpty() || path == "index.html")
    {
        const auto* bytes = reinterpret_cast<const std::byte*> (BinaryData::index_html);
        return juce::WebBrowserComponent::Resource {
            std::vector<std::byte> (bytes, bytes + BinaryData::index_htmlSize), "text/html; charset=utf-8" };
    }
    return std::nullopt;
}

void MouthTubeEditor::onSetParam (const juce::var& v)
{
    const auto id = v.getProperty ("id", {}).toString();
    auto* param = proc.apvts.getParameter (id);
    if (param == nullptr) return;
    const float value = juce::jlimit (0.0f, 1.0f, (float) (double) v.getProperty ("value", 0.0));

    const bool inGesture = openGestures.count (id) > 0;
    if (! inGesture) param->beginChangeGesture();
    param->setValueNotifyingHost (value);
    if (! inGesture) param->endChangeGesture();
    lastSent[id] = param->getValue();   // don't echo our own change back
}

void MouthTubeEditor::onGesture (const juce::var& v)
{
    const auto id = v.getProperty ("id", {}).toString();
    auto* param = proc.apvts.getParameter (id);
    if (param == nullptr) return;
    const bool begin = (bool) v.getProperty ("begin", false);
    if (begin && openGestures.insert (id).second) param->beginChangeGesture();
    if (! begin && openGestures.erase (id) > 0) param->endChangeGesture();
}

void MouthTubeEditor::onPresetAction (const juce::var& v)
{
    auto& pm = proc.presets;
    const auto action = v.getProperty ("action", {}).toString();
    const auto name = v.getProperty ("name", {}).toString();
    if (action == "load") pm.loadUser (name);
    else if (action == "save") pm.saveUser (name);
    else if (action == "delete") pm.deleteUser (name);
    else if (action == "step") pm.step ((int) v.getProperty ("dir", 1));
    lastPresetKey = {};   // resend the list
}

void MouthTubeEditor::sendPresets()
{
    auto& pm = proc.presets;
    auto toVar = [] (const juce::StringArray& a) { juce::Array<juce::var> r; for (auto& s : a) r.add (s); return juce::var (r); };
    auto* obj = new juce::DynamicObject();
    obj->setProperty ("user", toVar (pm.userNames()));
    obj->setProperty ("current", pm.currentName());
    obj->setProperty ("kind", pm.currentKind());
    obj->setProperty ("modified", lastModified);
    browser.emitEventIfBrowserIsVisible ("presets", juce::var (obj));
}

void MouthTubeEditor::timerCallback()
{
    // 0) preset name, list and the "changed since loading" mark
    if (proc.stateRestored.exchange (false)) proc.presets.snapshot();
    const bool modified = proc.presets.isModified();
    const auto key = proc.presets.currentKind() + "/" + proc.presets.currentName();
    if (modified != lastModified || key != lastPresetKey)
    {
        lastModified = modified;
        lastPresetKey = key;
        sendPresets();
    }

    // 1) parameter values the page doesn't know yet (automation, presets, undo, first load)
    for (auto* p : proc.getParameters())
    {
        auto* rp = dynamic_cast<juce::RangedAudioParameter*> (p);
        if (rp == nullptr) continue;
        const auto id = rp->getParameterID();
        const float value = rp->getValue();
        auto it = lastSent.find (id);
        if (needFullSync || it == lastSent.end() || std::abs (it->second - value) > 1.0e-6f)
        {
            auto* obj = new juce::DynamicObject();
            obj->setProperty ("id", id);
            obj->setProperty ("value", value);
            browser.emitEventIfBrowserIsVisible ("param", juce::var (obj));
            lastSent[id] = value;
        }
    }
    needFullSync = false;

    // 2) live view: tube shape, nasal coupling, length, and recent output for the spectrum
    auto& v = proc.view;
    juce::Array<juce::var> d, samples;
    d.ensureStorageAllocated (MouthTubeView::N);
    for (int i = 0; i < MouthTubeView::N; ++i)
        d.add (v.d[(size_t) i].load (std::memory_order_relaxed));
    const int pos = v.ringPos.load (std::memory_order_acquire);
    samples.ensureStorageAllocated (MouthTubeView::RING);
    for (int i = 0; i < MouthTubeView::RING; ++i)
        samples.add (v.ring[(size_t) ((pos + i) % MouthTubeView::RING)].load (std::memory_order_relaxed));

    auto* obj = new juce::DynamicObject();
    obj->setProperty ("d", d);
    obj->setProperty ("nasal", v.nasal.load());
    obj->setProperty ("len", v.len.load());
    obj->setProperty ("samples", samples);
    obj->setProperty ("sampleRate", proc.currentSampleRate.load());
    browser.emitEventIfBrowserIsVisible ("view", juce::var (obj));
}
