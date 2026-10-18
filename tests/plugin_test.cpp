// Plugin wrapper test: the JUCE processor must sound exactly like the bare engine fed the same
// audio and settings, map every parameter correctly, restore its state, drive the web interface
// and handle user presets.
// Build target: plugin_test (see CMakeLists.txt).
#include "../source/PluginProcessor.h"
#include "../source/PluginEditor.h"
#include <iostream>

namespace mt = bytebent::mouthtube;
static int fails = 0;
static void check (const char* name, bool ok, const juce::String& info = {})
{
    std::cout << (ok ? "PASS " : "FAIL ") << name << (info.isEmpty() ? "" : "  " + info) << std::endl;
    if (! ok) ++fails;
}
static bool exactlyZero (double d) { return d >= 0.0 && d <= 0.0; }   // -1 means the render was silent

static void setParam (MouthTubeProcessor& p, const char* id, float value)
{
    auto* prm = p.apvts.getParameter (id);
    prm->setValueNotifyingHost (prm->convertTo0to1 (value));
}

// deterministic voice-like input: a pulse train with a little noise, and a second channel
static float inputSample (int ch, int i, double sr)
{
    const double t = i / sr;
    const double ph = std::fmod (t * (ch ? 196.0 : 130.0), 1.0);
    const double pulse = ph < 0.4 ? 0.5 * (1 - std::cos (juce::MathConstants<double>::pi * ph / 0.4)) : 0.0;
    const double noise = std::sin (i * 12.9898 + ch * 78.233) * 43758.5453;
    return (float) (0.12 * pulse + 0.01 * (noise - std::floor (noise) - 0.5));
}

// render through the plugin (host blocks of `block`) and through the engine directly
static double compare (MouthTubeProcessor& p, int inCh, int outCh, int total, int block, double sr)
{
    auto layout = p.getBusesLayout();
    layout.inputBuses.getReference (0) = inCh == 1 ? juce::AudioChannelSet::mono() : juce::AudioChannelSet::stereo();
    layout.outputBuses.getReference (0) = outCh == 1 ? juce::AudioChannelSet::mono() : juce::AudioChannelSet::stereo();
    if (! p.setBusesLayout (layout)) return -2.0;
    p.setPlayConfigDetails (inCh, outCh, sr, block);
    p.prepareToPlay (sr, block);

    mt::Engine e; e.prepare (sr);
    for (int i = 0; i < mt::kNumParams; ++i)
        e.setParam (i, p.apvts.getRawParameterValue (mt::paramInfo (i).id)->load());
    e.snap();
    std::vector<float> l ((size_t) total), r ((size_t) total), rl ((size_t) total), rr ((size_t) total);
    for (int i = 0; i < total; ++i) { l[(size_t) i] = inputSample (0, i, sr); r[(size_t) i] = inputSample (1, i, sr); }
    e.process (l.data(), inCh > 1 ? r.data() : nullptr, rl.data(), rr.data(), total);

    juce::AudioBuffer<float> buf (std::max (inCh, outCh), block);
    juce::MidiBuffer midi;
    double maxd = 0, energy = 0;
    for (int start = 0; start < total; start += block)
    {
        const int n = std::min (block, total - start);
        buf.setSize (std::max (inCh, outCh), n, false, false, true);
        buf.clear();
        for (int i = 0; i < n; ++i)
        {
            buf.setSample (0, i, l[(size_t) (start + i)]);
            if (inCh > 1) buf.setSample (1, i, r[(size_t) (start + i)]);
        }
        p.processBlock (buf, midi);
        for (int i = 0; i < n; ++i)
        {
            const double a = buf.getSample (0, i), b = rl[(size_t) (start + i)];
            maxd = std::max (maxd, std::abs (a - b));
            if (outCh > 1) maxd = std::max (maxd, (double) std::abs (buf.getSample (1, i) - rr[(size_t) (start + i)]));
            energy += b * b;
        }
    }
    return energy > 1e-6 ? maxd : -1.0;
}

// ---- the web interface, end to end: host → page and page → host ----
static juce::var evalJs (juce::WebBrowserComponent& b, const juce::String& js, std::function<void()> pump)
{
    juce::var result; bool done = false;
    b.evaluateJavascript (js, [&] (juce::WebBrowserComponent::EvaluationResult r) {
        if (auto* v = r.getResult()) result = *v;
        done = true; });
    for (int i = 0; i < 200 && ! done; ++i) pump();
    return result;
}

struct Harness
{
    MouthTubeProcessor p;
    std::unique_ptr<juce::AudioProcessorEditor> ed;
    juce::AudioBuffer<float> buf { 2, 480 };
    int t = 0;
    Harness()
    {
        p.setPlayConfigDetails (2, 2, 48000, 480);
        p.prepareToPlay (48000, 480);
        ed.reset (p.createEditor());
        ed->addToDesktop (0);
        ed->setVisible (true);
        for (int i = 0; i < 300; ++i) pump();             // page loads, frames start flowing
    }
    juce::WebBrowserComponent& browser() { return dynamic_cast<MouthTubeEditor&> (*ed).getBrowser(); }
    void pump()
    {
        juce::MidiBuffer midi;
        for (int i = 0; i < 480; ++i, ++t) { buf.setSample (0, i, inputSample (0, t, 48000)); buf.setSample (1, i, inputSample (1, t, 48000)); }
        p.processBlock (buf, midi);                       // 10 ms of audio per pump
        juce::MessageManager::getInstance()->runDispatchLoopUntil (10);
    }
    juce::var js (const juce::String& code) { auto r = evalJs (browser(), code, [this] { pump(); }); for (int i = 0; i < 25; ++i) pump(); return r; }
    float val (const char* id) { return p.apvts.getRawParameterValue (id)->load(); }
};

static void editorTest()
{
    Harness h;
    setParam (h.p, "damping", 0.62f);
    for (int i = 0; i < 30; ++i) h.pump();
    const auto shown = h.js ("[...document.querySelectorAll('.knob')].find(k=>k.querySelector('.name').textContent==='Damping').querySelector('.val').textContent").toString();
    check ("page shows a host-side parameter change", shown == "62%", shown);

    setParam (h.p, "length", 22.0f);
    for (int i = 0; i < 40; ++i) h.pump();
    const auto ro = h.js ("[document.getElementById('roLen').textContent, document.getElementById('roF1').textContent].join('|')").toString();
    check ("live view receives frames (length and formants)", ro.startsWith ("22.0 cm|") && ro.contains ("Hz"), ro);

    h.js ("(()=>{const d=[...document.querySelectorAll('.knob')].find(k=>k.querySelector('.name').textContent==='Tip').querySelector('.dial'); for(let i=0;i<5;i++) d.dispatchEvent(new KeyboardEvent('keydown',{key:'ArrowUp',bubbles:true})); return 1;})()");
    check ("turning a knob on the page changes the plugin parameter", std::abs (h.val ("tip") - 0.1f) < 1e-4f, juce::String (h.val ("tip")));

    const float x0 = h.val ("tongueX"), y0 = h.val ("tongueY");
    h.js ("(()=>{const p=document.getElementById('pad'); for(let i=0;i<4;i++) p.dispatchEvent(new KeyboardEvent('keydown',{key:'ArrowLeft',bubbles:true})); for(let i=0;i<2;i++) p.dispatchEvent(new KeyboardEvent('keydown',{key:'ArrowUp',bubbles:true})); return 1;})()");
    check ("tongue pad moves the tongue position parameters",
           std::abs (h.val ("tongueX") - (x0 - 0.12f)) < 1e-4f && std::abs (h.val ("tongueY") - (y0 - 0.06f)) < 1e-4f,
           juce::String (h.val ("tongueX")) + ", " + juce::String (h.val ("tongueY")));

    setParam (h.p, "tongueX", 0.9f);
    for (int i = 0; i < 30; ++i) h.pump();
    const auto padTxt = h.js ("document.getElementById('padX').textContent").toString();
    check ("pad follows a host-side tongue change", padTxt == "Back 80%", padTxt);

    h.js ("(document.querySelector('#segRad button[data-v=\"0\"]').click(), 1)");
    check ("page switch turns Radiation off", h.val ("radiation") < 0.5f);
    if (std::getenv ("MOUTH_HOLD") != nullptr)            // keep the window up for a screenshot
        for (int i = 0; i < 600; ++i) h.pump();
}

// ---- presets: user files, the page's preset bar, project state ----
static void presetTest()
{
    {
        MouthTubeProcessor p;
        auto& pm = p.presets;
        check ("no factory presets: a fresh instance has nothing to step to", pm.userNames().isEmpty() && pm.currentName() == "Init");
        pm.step (1);
        check ("arrows do nothing with no saved presets", pm.currentName() == "Init" && pm.currentKind() == "none");
    }

    Harness h;
    auto& p = h.p;

    // save through the page: Save, type a name, Enter
    setParam (p, "tongueX", 0.1f); setParam (p, "nasal", 0.4f); setParam (p, "length", 25.0f); setParam (p, "radiation", 0);
    h.js ("document.getElementById('pSave').click(), 1");
    h.js ("(()=>{const i=document.querySelector('#pName input'); i.value='Nasal ee'; i.dispatchEvent(new KeyboardEvent('keydown',{key:'Enter',bubbles:true})); return 1;})()");
    const auto file = PresetManager::userFolder().getChildFile ("Nasal ee.mouthtubepreset");
    check ("Save on the page writes a user preset file", file.existsAsFile(), file.getFullPathName());
    check ("page shows the saved name without a change mark",
           h.js ("document.getElementById('pTxt').textContent + '|' + document.getElementById('pMod').hidden").toString() == "Nasal ee|true");

    // a second preset, then load the first back from the menu
    setParam (p, "nasal", 0.0f); setParam (p, "lips", 0.1f); setParam (p, "round", 1.0f); setParam (p, "radiation", 1);
    p.presets.saveUser ("Round");
    for (int i = 0; i < 10; ++i) h.pump();
    check ("menu lists only saved presets, sorted",
           h.js ("[...document.querySelectorAll('#pMenu .it')].length + '|' + (document.getElementById('pName').click(), [...document.querySelectorAll('#pMenu .it')].map(e=>e.textContent).join(','))").toString() == "0|Nasal ee,Round");
    h.js ("([...document.querySelectorAll('#pMenu .it')].find(e=>e.textContent==='Nasal ee').click(), 1)");
    check ("menu loads the user preset with its values",
           std::abs (h.val ("nasal") - 0.4f) < 1e-4f && std::abs (h.val ("length") - 25.0f) < 1e-3f && h.val ("radiation") < 0.5f
           && std::abs (h.val ("lips") - 0.6f) < 1e-4f && std::abs (h.val ("round")) < 1e-6f);

    // change mark
    setParam (p, "jaw", 0.9f);
    for (int i = 0; i < 10; ++i) h.pump();
    check ("page marks the preset as changed", h.js ("String(document.getElementById('pMod').hidden)").toString() == "false");

    // arrows step through the saved presets and wrap around
    h.js ("(document.getElementById('pNext').click(), 1)");
    check ("next arrow loads the next preset", p.presets.currentName() == "Round" && std::abs (h.val ("round") - 1.0f) < 1e-6f, p.presets.currentName());
    h.js ("(document.getElementById('pNext').click(), 1)");
    check ("next arrow wraps around to the first", p.presets.currentName() == "Nasal ee" && std::abs (h.val ("nasal") - 0.4f) < 1e-4f, p.presets.currentName());
    h.js ("(document.getElementById('pPrev').click(), 1)");
    check ("previous arrow wraps the other way", p.presets.currentName() == "Round", p.presets.currentName());
    h.js ("(document.getElementById('pPrev').click(), 1)");

    // the preset name is saved with the project
    juce::MemoryBlock mb; p.getStateInformation (mb);
    MouthTubeProcessor q; q.setStateInformation (mb.getData(), (int) mb.getSize());
    check ("project state keeps the preset name", q.presets.currentName() == "Nasal ee" && q.presets.currentKind() == "user");

    // delete from the menu
    h.js ("(document.getElementById('pName').click(), [...document.querySelectorAll('#pMenu .it')].find(e=>e.textContent==='Nasal ee').querySelector('.del').click(), 1)");
    check ("delete button removes the user preset file", ! file.existsAsFile());
    const auto menuNow = h.js ("[...document.querySelectorAll('#pMenu .it')].map(e=>e.textContent).join(',')").toString();
    check ("menu no longer lists it", ! menuNow.contains ("Nasal ee"), menuNow);
}

int main()
{
    // keep test presets out of the real ~/Music
    const auto home = juce::File::createTempFile ("mouthtube-home");
    home.createDirectory();
    setenv ("HOME", home.getFullPathName().toRawUTF8(), 1);
    juce::ScopedJuceInitialiser_GUI init;

    {
        MouthTubeProcessor p;
        const double d = compare (p, 2, 2, 40000, 512, 48000);
        check ("defaults, stereo in/out: plugin output = engine output", exactlyZero (d), "max diff " + juce::String (d));
    }
    {
        MouthTubeProcessor p;
        setParam (p, "tongueX", 0.05f); setParam (p, "tongueY", 0.1f); setParam (p, "tip", 0.3f); setParam (p, "jaw", 0.8f);
        setParam (p, "lips", 0.4f); setParam (p, "round", 0.6f); setParam (p, "nasal", 0.3f); setParam (p, "length", 12.5f);
        setParam (p, "damping", 0.2f); setParam (p, "spread", 0.7f); setParam (p, "radiation", 0); setParam (p, "mix", 0.8f); setParam (p, "output", -4.5f);
        const double d = compare (p, 1, 2, 50000, 333, 44100);
        check ("every knob, mono in → stereo out, odd block size: identical to engine", exactlyZero (d), "max diff " + juce::String (d));
    }
    {
        MouthTubeProcessor p;
        setParam (p, "length", 33.0f); setParam (p, "spread", 0.5f);
        const double d = compare (p, 1, 1, 60000, 128, 96000);
        check ("mono in/out at 96 kHz: identical to engine", exactlyZero (d), "max diff " + juce::String (d));
        check ("latency reported to the host", p.getLatencySamples() > 0 && p.getLatencySamples() < 200, juce::String (p.getLatencySamples()));
    }
    {
        MouthTubeProcessor p;
        auto layout = p.getBusesLayout();
        layout.inputBuses.getReference (0) = juce::AudioChannelSet::stereo();
        layout.outputBuses.getReference (0) = juce::AudioChannelSet::mono();
        check ("stereo in → mono out is refused", ! p.checkBusesLayoutSupported (layout));
    }
    {
        MouthTubeProcessor a;
        setParam (a, "tongueX", 0.2f); setParam (a, "length", 29.0f); setParam (a, "radiation", 0); setParam (a, "output", 6.0f);
        setParam (a, "nasal", 0.7f); setParam (a, "spread", 0.25f);
        juce::MemoryBlock mb; a.getStateInformation (mb);
        MouthTubeProcessor b; b.setStateInformation (mb.getData(), (int) mb.getSize());
        bool same = true;
        juce::String bad;
        for (int i = 0; i < mt::kNumParams; ++i)
        {
            const char* id = mt::paramInfo (i).id;
            const float x = a.apvts.getRawParameterValue (id)->load(), y = b.apvts.getRawParameterValue (id)->load();
            if (std::abs (x - y) > 1e-5f) { same = false; bad << id << " "; }
        }
        check ("state saves and restores every parameter", same, bad);
        const float len = b.apvts.getRawParameterValue ("length")->load();
        check ("length (log range) round-trips through the host's 0-1 value", std::abs (len - 29.0f) < 1e-3f, juce::String (len));
    }
    {
        MouthTubeProcessor p;
        bool ok = true;
        for (int i = 0; i < mt::kNumParams; ++i)
        {
            const auto& inf = mt::paramInfo (i);
            auto* prm = p.apvts.getParameter (inf.id);
            const float lo = prm->convertFrom0to1 (0.0f), hi = prm->convertFrom0to1 (1.0f), def = p.apvts.getRawParameterValue (inf.id)->load();
            if (std::abs (lo - inf.min) > 1e-4 || std::abs (hi - inf.max) > 1e-4 || std::abs (def - inf.def) > 1e-4)
            { ok = false; std::cout << "  " << inf.id << " " << lo << ".." << hi << " def " << def << std::endl; }
        }
        check ("every parameter's range and default match the engine", ok);
    }

    editorTest();
    presetTest();

    std::cout << (fails ? juce::String (fails) + " FAILED" : juce::String ("all passed")) << std::endl;
    return fails ? 1 : 0;
}
