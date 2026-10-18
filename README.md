# mouth-tube

An audio effect that shapes sound the way a vocal tract does. It makes no sound of its own:
whatever comes in travels through a tube shaped like a mouth and comes out of the lips (and the
nose). Put it on a voice-like source to make it talk, or on drums, synths and field recordings.

The tract is a Kelly-Lochbaum waveguide: 40 sections from glottis to lips, plus a nasal branch
that opens at the velum. The controls describe a mouth (tongue, jaw, lips, velum) and the tube
shape follows from them. There are no vowel or consonant presets inside the engine; vowels,
nasals and closures come out of the shape alone. Length is real: the tube runs at its own rate,
so formants scale smoothly from a small mouth to a giant one.

Formats: **AU and VST3 effect for macOS** (Apple Silicon + Intel), plus a standalone app.

## Download and install

Grab `mouth-tube-macOS.zip` from the
[latest build](https://github.com/bytebent/mouth-tube/releases/tag/latest),
unzip it and follow `INSTALL.txt` (right-click `Install.command` → Open).

## Controls

| Group | Control | What it does |
|---|---|---|
| Tongue position | Pad (vowel chart) | Where the tongue body sits: front ↔ back, close ↔ open. The middle of the chart is a relaxed, uniform tube. |
| Tongue | Tip | Raises the tongue tip to the ridge behind the teeth. At 100% it closes the tube there. |
| Jaw | Open | Opens (or closes) the mouth cavity in front of the tongue. |
| Lips | Opening | The opening at the very end of the tube. 0 = closed. |
| | Round | Rounds and pushes the lips forward: narrower opening, slightly longer tube. |
| Velum | Nasal | Opens the passage into the nose. With the mouth closed, sound comes out of the nose only. |
| Tract | Length | 7–35 cm. The marks on the knob show the human range (about 10–19 cm). |
| | Damping | How soft and lossy the tube walls are: ringing ↔ dull. |
| Output | Spread | Stereo: the left and right tubes differ slightly (length and tongue position). 0 = identical. |
| | Radiation | On: the lips turn air flow into sound pressure (+6 dB/octave), as a real mouth does. Off: flow out. |
| | Mix | Dry ↔ processed (the dry signal is delayed to line up). |
| | Output | −24 to +12 dB. |
| Presets | ◀ name ▶, Save | Your presets, saved to `~/Music/Bytebent/mouth-tube/Presets`. A `*` marks changes since loading. |

Stereo works as dual mono: one tube per channel. Mono in → stereo out is supported (and only
uses one tube until Spread is turned up). The plugin adds a small fixed latency (28 samples at
48 kHz), which it reports to the host.

The output gains are matched to a glottal-flow source (like Throat's flow output). With
Radiation on, brighter sources come out louder.

## Test bench

`tools/mouth-tube.html` is a self-contained page that runs the reference engine live in the
browser. Load an audio file and press Play to run it once through the tract while you move the
controls.

Rebuild it after changing the engine: `node tools/bench/build.js`.

## Repository layout

- `reference/mouth-tube.js` — the reference engine; defines the sound
- `source/MouthTubeDSP.h` — C++ port, real-time safe, no JUCE dependency
- `source/PluginProcessor.*` — JUCE effect wrapper (mono or stereo in, mono or stereo out)
- `source/PluginEditor.*`, `ui/index.html` — the interface, shown in a web view (also opens in a browser as a preview)
- `source/PresetManager.h` — saving and loading presets
- `packaging/` — installer script and instructions for the Mac download
- `tests/` — reference tests, the C++ vs reference comparison, and the plugin wrapper test
- `tools/mouth-tube.html` — browser test bench (built from `tools/bench/`)

## Tests

```sh
node tests/engine-test.js
g++ -O2 -std=c++17 -ffp-contract=off -o dsp_cli tests/dsp_cli.cpp && node tests/compare.js ./dsp_cli
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --target plugin_test
./build/plugin_test_artefacts/Release/plugin_test
```

GitHub Actions runs all of them on every push, and builds and validates the Mac plugin
(universal binary, auval, pluginval at strictness 7).

## Building on a Mac

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"
cmake --build build --config Release --target MouthTube_AU MouthTube_VST3 MouthTube_Standalone
```

## License note

The plugin is built with [JUCE](https://juce.com) 8, which is available under the AGPLv3
or a commercial licence. Keeping this repository public and open source satisfies the AGPL.
